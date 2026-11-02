#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Local APIC + I/O APIC interrupt delivery. The legacy 8259 PICs are used
 * until apic_init() succeeds; if anything about the machine looks wrong it
 * returns 0 and the PIC keeps working exactly as before. */

#define APIC_SPURIOUS_VECTOR 0x2F

int      apic_init(void);                 /* call with the PIT timer running; 1 = APIC mode is on */
int      apic_is_active(void);
void     lapic_eoi(void);
void     ioapic_enable_irq(int isa_irq);  /* route an ISA IRQ (vector 32 + irq) to this CPU */
void     ioapic_disable_irq(int isa_irq);
void     apic_print_info(void);

#endif
