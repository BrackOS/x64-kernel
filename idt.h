#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT Gate Types
#define IDT_GATE_INTERRUPT 0x0E
#define IDT_GATE_TRAP      0x0F

// IDT Entry Structure
typedef struct {
    uint16_t offset_low;      // Offset bits 0-15
    uint16_t selector;        // Code segment selector
    uint8_t  ist;             // Interrupt Stack Table (unused in 32-bit compat)
    uint8_t  type_attr;       // Type and attributes
    uint16_t offset_mid;      // Offset bits 16-31
    uint32_t offset_high;     // Offset bits 32-63
    uint32_t reserved;        // Reserved (must be zero)
} __attribute__((packed)) idt_entry_t;

// IDT Pointer Structure (for LIDT instruction)
typedef struct {
    uint16_t limit;           // Size of IDT - 1
    uint64_t base;            // Base address of IDT
} __attribute__((packed)) idt_ptr_t;

// Exception handlers
void idt_init(void);
void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type);

// Interrupt numbers
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1
#define IRQ_CASCADE     2
#define IRQ_SERIAL2     3
#define IRQ_SERIAL1     4
#define IRQ_PARALLEL    5
#define IRQ_FLOPPY      6
#define IRQ_PARALLEL2   7
#define IRQ_REAL_TIME   8
#define IRQ_MOUSE       12
#define IRQ_COPROCESSOR 13
#define IRQ_ATA_DISK1   14
#define IRQ_ATA_DISK2   15

// ISR stubs (defined in idt.s)
extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_8(void);
extern void irq_stub_0(void);
extern void irq_stub_1(void);
extern void irq_stub_12(void);
extern void irq_stub_14(void);

void pic_enable_irq(int irq);
void pic_disable_irq(int irq);
void interrupts_enable(void);
void interrupts_disable(void);

#endif
