#include "apic.h"
#include "acpi.h"
#include "paging.h"
#include "timer.h"
#include "vga.h"
#include "serial.h"
#include "io.h"
#include "vmm.h"
#include "irqflags.h"

/* Local APIC registers (byte offsets) */
#define LAPIC_ID        0x020
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define LVT_MASKED      (1u << 16)
#define LVT_PERIODIC    (1u << 17)
#define MSR_APIC_BASE   0x1B

/* I/O APIC registers */
#define IOAPIC_REGSEL   0x00
#define IOAPIC_WIN      0x10
#define IOAPIC_VER      0x01
#define IOAPIC_REDTBL   0x10

#define TIMER_VECTOR    32            /* the existing IRQ 0 vector: timer_irq_handler runs for it */

static int active = 0;
static const char *status = "not started";
static acpi_info_t acpi;
static volatile uint32_t *lapic = 0;
static uint32_t ticks_per_10ms = 0;

static inline uint32_t lapic_read(uint32_t reg)              { return lapic[reg / 4]; }
static inline void lapic_write(uint32_t reg, uint32_t value) { lapic[reg / 4] = value; }

int apic_is_active(void) { return active; }

void lapic_eoi(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

/* ---- I/O APIC ---- */

static uint32_t ioapic_read(int n, uint32_t reg) {
    volatile uint32_t *base = (volatile uint32_t *)PHYS_TO_VIRT(acpi.ioapic[n].addr);
    base[IOAPIC_REGSEL / 4] = reg;
    return base[IOAPIC_WIN / 4];
}

static void ioapic_write(int n, uint32_t reg, uint32_t value) {
    volatile uint32_t *base = (volatile uint32_t *)PHYS_TO_VIRT(acpi.ioapic[n].addr);
    base[IOAPIC_REGSEL / 4] = reg;
    base[IOAPIC_WIN / 4] = value;
}

/* Which I/O APIC input (global system interrupt) does an ISA IRQ arrive on? */
static uint32_t irq_to_gsi(int irq, uint16_t *flags) {
    *flags = 0;
    for (int i = 0; i < acpi.iso_count; i++) {
        if (acpi.iso[i].source == irq) { *flags = acpi.iso[i].flags; return acpi.iso[i].gsi; }
    }
    return (uint32_t)irq;
}

static int find_ioapic(uint32_t gsi) {
    for (int i = 0; i < acpi.ioapic_count; i++) {
        uint32_t max = (ioapic_read(i, IOAPIC_VER) >> 16) & 0xFF;
        if (gsi >= acpi.ioapic[i].gsi_base && gsi <= acpi.ioapic[i].gsi_base + max) return i;
    }
    return -1;
}

static void ioapic_set_irq(int irq, int enable) {
    uint16_t flags;
    uint32_t gsi = irq_to_gsi(irq, &flags);
    int io = find_ioapic(gsi);
    if (io < 0) return;
    uint32_t pin = gsi - acpi.ioapic[io].gsi_base;

    /* ISA defaults: active high, edge triggered. "conforms" (0) means the default. */
    uint32_t low = (uint32_t)(32 + irq);
    if ((flags & 3) == 3) low |= (1u << 13);          /* active low */
    if (((flags >> 2) & 3) == 3) low |= (1u << 15);   /* level triggered */
    if (!enable) low |= LVT_MASKED;

    ioapic_write(io, IOAPIC_REDTBL + 2 * pin + 1, (lapic_read(LAPIC_ID) >> 24) << 24);  /* to this CPU */
    ioapic_write(io, IOAPIC_REDTBL + 2 * pin, low);
}

void ioapic_enable_irq(int irq)  { if (active) ioapic_set_irq(irq, 1); }
void ioapic_disable_irq(int irq) { if (active) ioapic_set_irq(irq, 0); }

/* ---- Bring-up ---- */

static int cpu_has_apic(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d >> 9) & 1;
}

static int fail(const char *why) {
    status = why;
    serial_print("APIC: not used: ");
    serial_print(why);
    serial_print("\n");
    return 0;
}

#define PAUSE() __asm__ volatile("pause")

/* Wait until the PIT has ticked `n` more times; 0 if it never does */
static int wait_pit_ticks(uint64_t n) {
    uint64_t target = system_timer.ticks + n;
    for (uint64_t spin = 0; spin < 600000000ULL; spin++) {
        if (system_timer.ticks >= target) return 1;
        PAUSE();
    }
    return 0;
}

int apic_init(void) {
    if (active) return 1;
    if (!cpu_has_apic()) return fail("CPU has no local APIC");

    const char *why;
    if (acpi_init(&acpi, &why) != 0) return fail(why);

    uint64_t msr = read_msr64(MSR_APIC_BASE);
    uint64_t base = acpi.lapic_addr;
    if (base < 0x400000 || base >= 0x100000000ULL) return fail("local APIC address out of range");
    if (!(msr & (1ULL << 11))) write_msr(MSR_APIC_BASE, msr | (1ULL << 11));      /* enable */

    /* Device registers must not be cached */
    if (paging_make_uncached(base) != 0) return fail("cannot map the local APIC");
    for (int i = 0; i < acpi.ioapic_count; i++) {
        if (paging_make_uncached(acpi.ioapic[i].addr) != 0) return fail("cannot map an I/O APIC");
    }
    lapic = (volatile uint32_t *)PHYS_TO_VIRT(base);

    lapic_write(LAPIC_SVR, 0x100 | APIC_SPURIOUS_VECTOR);     /* software enable */
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);                 /* the PIC "virtual wire" is not used */
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED | 0xFE);

    /* Measure the APIC timer against the PIT, which is still ticking at 100 Hz */
    lapic_write(LAPIC_TIMER_DIV, 0x3);                        /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VECTOR);  /* one-shot, not delivered */
    uint64_t t0 = system_timer.ticks;
    if (!wait_pit_ticks(1)) return fail("PIT is not ticking; cannot calibrate");
    (void)t0;
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    uint32_t start = lapic_read(LAPIC_TIMER_CUR);
    if (!wait_pit_ticks(10)) return fail("PIT stopped during calibration");
    uint32_t end = lapic_read(LAPIC_TIMER_CUR);
    uint32_t per_100ms = start - end;
    ticks_per_10ms = per_100ms / 10;
    if (ticks_per_10ms < 1000) return fail("local APIC timer calibration failed");

    if (acpi.pcat_compat) {                                   /* route interrupts through the APIC */
        outb(0x22, 0x70);
        outb(0x23, inb(0x23) | 1);
    }

    /* Switch over with interrupts off: the APIC timer replaces the PIT, and the
     * 8259s are silenced so nothing arrives twice. */
    uintptr_t flags = irq_save();
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, ticks_per_10ms);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    active = 1;
    status = "active";
    irq_restore(flags);

    serial_print("APIC: active\n");
    return 1;
}

void apic_print_info(void) {
    vga_print("Interrupt controller: ");
    vga_print(active ? "APIC (local APIC + I/O APIC)\n" : "legacy 8259 PIC\n");
    vga_print("  Status: ");
    vga_print(status);
    vga_print("\n");
    if (active) {
        vga_print("  Local APIC at ");
        vga_print_hex(acpi.lapic_addr);
        vga_print(", id ");
        vga_print_int((int)(lapic_read(LAPIC_ID) >> 24));
        vga_print(", timer ");
        vga_print_int((int)ticks_per_10ms);
        vga_print(" counts per 10 ms\n");
        for (int i = 0; i < acpi.ioapic_count; i++) {
            vga_print("  I/O APIC ");
            vga_print_int(acpi.ioapic[i].id);
            vga_print(" at ");
            vga_print_hex(acpi.ioapic[i].addr);
            vga_print(", inputs from ");
            vga_print_int((int)acpi.ioapic[i].gsi_base);
            vga_print("\n");
        }
        vga_print("  Processors listed: ");
        vga_print_int(acpi.cpu_count);
        vga_print(" (only the boot processor is used)\n");
        for (int i = 0; i < acpi.iso_count; i++) {
            vga_print("  IRQ ");
            vga_print_int(acpi.iso[i].source);
            vga_print(" -> input ");
            vga_print_int((int)acpi.iso[i].gsi);
            vga_print("\n");
        }
    }
}
