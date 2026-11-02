#include "fpu.h"
#include "string.h"
#include <stdint.h>

static uint8_t clean_state[512] __attribute__((aligned(16)));

void fpu_init(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);               /* EM off: the FPU is there */
    cr0 |= (1ULL << 1);                /* MP on */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR | OSXMMEXCPT: SSE allowed in user mode */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile("fninit");
    __asm__ volatile("fxsave (%0)" : : "r"(clean_state) : "memory");
}

void fpu_save(void *area)          { __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory"); }
void fpu_restore(const void *area) { __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory"); }
void fpu_reset_state(void *area)   { memcpy(area, clean_state, 512); }
