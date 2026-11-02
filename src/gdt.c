#include "gdt.h"
#include <stdint.h>

/* 64-bit Task State Segment: only the IST pointers matter to us */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];            /* stacks for privilege changes (unused: no user mode yet) */
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

#define IST_STACK_SIZE 16384

/* null, kernel code, kernel data, user data, user code, TSS (2 slots).
 * The order is dictated by the SYSRET instruction: with STAR[63:48] = 0x10 it
 * loads SS = 0x18|3 and CS = 0x20|3. */
static uint64_t gdt[7] __attribute__((aligned(16)));
static tss_t tss;
static uint8_t ist_stack_df[IST_STACK_SIZE]    __attribute__((aligned(16)));
static uint8_t ist_stack_crash[IST_STACK_SIZE] __attribute__((aligned(16)));

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL;     /* 0x08: 64-bit kernel code */
    gdt[2] = 0x00af92000000ffffULL;     /* 0x10: kernel data */
    gdt[3] = 0x00aff2000000ffffULL;     /* 0x18: user data (DPL 3) */
    gdt[4] = 0x00affa000000ffffULL;     /* 0x20: 64-bit user code (DPL 3) */

    tss.iomap_base = sizeof(tss);       /* no I/O permission bitmap */
    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(uintptr_t)(ist_stack_df + IST_STACK_SIZE);
    tss.ist[IST_CRASH - 1]        = (uint64_t)(uintptr_t)(ist_stack_crash + IST_STACK_SIZE);

    /* 16-byte TSS descriptor at 0x28: present, type 0x9 (available 64-bit TSS) */
    uint64_t base  = (uint64_t)(uintptr_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    gdt_ptr_t gp = { (uint16_t)(sizeof(gdt) - 1), (uint64_t)(uintptr_t)gdt };

    /* Load the GDT, reload CS with a far return, then reload the data segments */
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        : : "m"(gp) : "rax", "memory");

    __asm__ volatile("ltr %w0" : : "r"((uint16_t)0x28));
}

/* Where the CPU switches to when a user-mode interrupt or exception arrives.
 * Must point at the top of the running task's kernel stack. */
void gdt_set_kernel_stack(uint64_t rsp0) {
    tss.rsp[0] = rsp0;
}
