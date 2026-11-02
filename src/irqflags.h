#ifndef IRQFLAGS_H
#define IRQFLAGS_H

#include <stdint.h>

/* Turn interrupts off, remembering whether they were on. Pair with irq_restore(). */
#ifdef HOST_TEST
static inline uintptr_t irq_save(void) { return 0x200; }
static inline void irq_restore(uintptr_t f) { (void)f; }
#else
static inline uintptr_t irq_save(void) {
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uintptr_t flags) {
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
}
#endif

#endif
