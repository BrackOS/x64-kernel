#include "idt.h"
#include <stdint.h>
#include "io.h"
#include "vga.h"
#include "timer.h"

#define IDT_ENTRIES 256

static idt_entry_t idt[IDT_ENTRIES] __attribute__((aligned(16)));
static idt_ptr_t idt_ptr;

// PIC (Programmable Interrupt Controller) ports
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20

// CPU exception names, indexed by vector
static const char *const exception_names[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point error", "Alignment check", "Machine check", "SIMD floating-point error",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "VMM communication", "Security", "Reserved"
};

// Called from isr_common. Prints what happened and halts.
void exception_handler(uintptr_t vector, uintptr_t error_code, uintptr_t ip) {
    vga_set_color(12, 0);  // Red text
    vga_print("\nEXCEPTION ");
    vga_print_int((int)vector);
    vga_print(": ");
    vga_print(vector < 32 ? exception_names[vector] : "Unknown");
    vga_print("\n  Error code: ");
    vga_print_hex(error_code);
    vga_print("\n  At address: ");
    vga_print_hex(ip);
    vga_print("\n");
    if (vector == 14) {
        uintptr_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        vga_print("  Fault address (CR2): ");
        vga_print_hex(cr2);
        vga_print("\n");
    }
    vga_set_color(7, 0);
    vga_print("System halted.\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

// Read the In-Service Register of one PIC (used to spot spurious IRQs)
static uint8_t pic_read_isr(uint16_t cmd_port) {
    outb(cmd_port, 0x0B);   // OCW3: read ISR
    return inb(cmd_port);
}

void irq_handler(uintptr_t vector) {
    /* The PIC is remapped so hardware IRQ n arrives as vector 32 + n */
    int irq = (int)vector - 32;

    /* Spurious IRQ 7 / 15: the PIC raised the line but nothing is in
     * service. Do not acknowledge IRQ 7; for IRQ 15 only the master
     * (which saw the cascade line) needs an EOI. */
    if (irq == 7 && !(pic_read_isr(PIC1_CMD) & 0x80)) {
        return;
    }
    if (irq == 15 && !(pic_read_isr(PIC2_CMD) & 0x80)) {
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    switch (irq) {
        case IRQ_TIMER:
            timer_irq_handler();
            break;
        case IRQ_KEYBOARD:
        case IRQ_MOUSE:
        case IRQ_ATA_DISK1:
        case IRQ_ATA_DISK2:
            /* Devices are currently polled; nothing to do yet */
            break;
        default:
            break;
    }

    /* End of interrupt: the slave PIC needs one too for IRQs 8-15 */
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

// Set an IDT entry (layout depends on CPU mode; see idt.h)
void idt_set_entry(int index, uintptr_t handler, uint16_t selector, uint8_t type) {
#if defined(__x86_64__)
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].offset_mid = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].selector = selector;
    idt[index].ist = 0;
    idt[index].type_attr = handler ? (type | 0x80) : 0;  // Present only if used
    idt[index].reserved = 0;
#else
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].offset_high = (handler >> 16) & 0xFFFF;
    idt[index].selector = selector;
    idt[index].zero = 0;
    idt[index].type_attr = handler ? (type | 0x80) : 0;  // Present only if used
#endif
}

// Initialize PIC (Programmable Interrupt Controller)
static void pic_init(void) {
    // ICW1: Start initialization
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    
    // ICW2: Set interrupt offsets
    outb(PIC1_DATA, 0x20);  // Master PIC: interrupts 32-39
    outb(PIC2_DATA, 0x28);  // Slave PIC: interrupts 40-47
    
    // ICW3: Set cascading
    outb(PIC1_DATA, 0x04);  // Master: slave at IRQ2
    outb(PIC2_DATA, 0x02);  // Slave: cascaded to master
    
    // ICW4: Set 8086 mode
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    
    // Mask all interrupts for now
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// Enable specific IRQ
void pic_enable_irq(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask &= ~(1 << (irq % 8));  // Clear bit to enable
    outb(port, mask);
    if (irq >= 8) {
        // Slave IRQs reach the CPU through the master's cascade line (IRQ 2)
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));
    }
}

// Disable specific IRQ
void pic_disable_irq(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask |= (1 << (irq % 8));   // Set bit to disable
    outb(port, mask);
}

// Initialize IDT and PIC
void idt_init(void) {
    // Clear IDT
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, 0, 0, 0);
    }
    
    // CPU exceptions (vectors 0-31): report and halt instead of triple-faulting
    for (int i = 0; i < 32; i++) {
        idt_set_entry(i, isr_stub_table[i], 0x08, IDT_GATE_INTERRUPT);
    }

    // Hardware IRQs 0-15 (vectors 32-47), including spurious IRQ 7 / 15
    for (int i = 0; i < 16; i++) {
        idt_set_entry(32 + i, irq_stub_table[i], 0x08, IDT_GATE_INTERRUPT);
    }

    // Initialize PIC
    pic_init();
    
    // Load IDT
    idt_ptr.limit = (sizeof(idt) - 1);
    idt_ptr.base = (uintptr_t)&idt[0];
    
    asm volatile("lidt %0" : : "m"(idt_ptr));
    
    // Enable interrupts
    asm volatile("sti");
    
    vga_print("IDT: Initialized\n");
}

// Disable interrupts
void interrupts_disable(void) {
    asm volatile("cli");
}

// Enable interrupts
void interrupts_enable(void) {
    asm volatile("sti");
}
