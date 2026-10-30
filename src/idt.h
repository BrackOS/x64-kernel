#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT Gate Types
#define IDT_GATE_INTERRUPT 0x0E
#define IDT_GATE_TRAP      0x0F

// IDT Entry Structure (16-byte long-mode gate descriptor)
typedef struct {
    uint16_t offset_low;      // Offset bits 0-15
    uint16_t selector;        // Code segment selector
    uint8_t  ist;             // Interrupt Stack Table
    uint8_t  type_attr;       // Type and attributes
    uint16_t offset_mid;      // Offset bits 16-31
    uint32_t offset_high;     // Offset bits 32-63
    uint32_t reserved;        // Reserved (must be zero)
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

// Exception handlers
void idt_init(void);
void exception_handler(uintptr_t vector, uintptr_t error_code, uintptr_t ip);
void irq_handler(uintptr_t vector);
void idt_set_entry(int index, uintptr_t handler, uint16_t selector, uint8_t type);

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

// Stub address tables (defined in idt_stubs*.s): one entry per vector
extern uintptr_t isr_stub_table[32];   // CPU exceptions, vectors 0-31
extern uintptr_t irq_stub_table[16];   // hardware IRQs 0-15, vectors 32-47

void pic_enable_irq(int irq);
void pic_disable_irq(int irq);
void interrupts_enable(void);
void interrupts_disable(void);

#endif
