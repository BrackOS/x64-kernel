#ifndef GDT_H
#define GDT_H

/* Interrupt Stack Table slots used by the IDT (see idt.c) */
#define IST_DOUBLE_FAULT   1    /* #DF: always runs on a known-good stack */
#define IST_CRASH          2    /* #PF, #SS, #GP, NMI, #MC: report even if the stack is bad */

/* Build the GDT with a 64-bit TSS, load it, and set up the IST stacks.
 * Must run before idt_init(). */
void gdt_init(void);

#endif
