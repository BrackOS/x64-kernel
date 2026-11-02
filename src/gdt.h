#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Interrupt Stack Table slots used by the IDT (see idt.c) */
#define IST_DOUBLE_FAULT   1    /* #DF: always runs on a known-good stack */
#define IST_CRASH          2    /* NMI and machine check: report even if the stack is bad */

/* Segment selectors */
#define SEL_KERNEL_CODE    0x08
#define SEL_KERNEL_DATA    0x10
#define SEL_USER_DATA      (0x18 | 3)
#define SEL_USER_CODE      (0x20 | 3)

/* Build the GDT with a 64-bit TSS, load it, and set up the IST stacks.
 * Must run before idt_init(). */
void gdt_init(void);
void gdt_set_kernel_stack(uint64_t rsp0);   /* TSS.rsp0 for user -> kernel transitions */

#endif
