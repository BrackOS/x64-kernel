#include "paging.h"
#include "vga.h"
#include "io.h"
#include <stdint.h>

/* Page tables built by boot.s: 2 MB identity map of the first 4 GB */
extern uint64_t pdt[2048];

/* Section boundaries from linker.ld, and the stack guard page from boot.s */
extern char __kernel_start[], __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[], __kernel_bss_end[];
extern char stack_guard[];

#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_PS   (1ULL << 7)
#define PTE_NX   (1ULL << 63)

#define PAGE     4096ULL
#define LOW_MB   4                          /* first N MB are mapped with 4 KB pages */
#define LOW_PTS  (LOW_MB / 2)

static uint64_t pt_low[LOW_PTS][512] __attribute__((aligned(4096)));

static int cpu_has_nx(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (a < 0x80000001u) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u));
    return (d >> 20) & 1;
}

static uint64_t down(uint64_t x) { return x & ~(PAGE - 1); }
static uint64_t up(uint64_t x)   { return (x + PAGE - 1) & ~(PAGE - 1); }

void paging_init(void) {
    uint64_t end = (uint64_t)(uintptr_t)__kernel_bss_end;
    if (end > (uint64_t)LOW_MB * 1024 * 1024) {
        vga_print("PAGING: kernel larger than 4 MB, keeping boot mappings\n");
        return;
    }

    uint64_t nx = 0;
    if (cpu_has_nx()) {
        write_msr(0xC0000080, read_msr(0xC0000080) | (1u << 11));   /* EFER.NXE */
        nx = PTE_NX;
    }

    /* Default for the first 4 MB: present, writable, not executable */
    for (int t = 0; t < LOW_PTS; t++) {
        for (int i = 0; i < 512; i++) {
            uint64_t phys = ((uint64_t)t * 512 + i) * PAGE;
            pt_low[t][i] = phys | PTE_P | PTE_RW | nx;
        }
    }

    #define PTE_OF(addr) pt_low[(addr) / (2 * 1024 * 1024)][((addr) / PAGE) % 512]

    PTE_OF(0ULL) = 0;                                               /* null page: not present */

    for (uint64_t a = down((uint64_t)(uintptr_t)__text_start);      /* code: read + execute */
         a < up((uint64_t)(uintptr_t)__text_end); a += PAGE) {
        PTE_OF(a) = a | PTE_P;
    }
    for (uint64_t a = down((uint64_t)(uintptr_t)__rodata_start);    /* constants: read only */
         a < up((uint64_t)(uintptr_t)__rodata_end); a += PAGE) {
        PTE_OF(a) = a | PTE_P | nx;
    }
    PTE_OF((uint64_t)(uintptr_t)stack_guard) = 0;                   /* stack guard page */

    /* Hook the two page tables into the page directory (first GB) */
    for (int t = 0; t < LOW_PTS; t++) {
        pdt[t] = (uint64_t)(uintptr_t)pt_low[t] | PTE_P | PTE_RW;
    }
    /* Everything else stays 2 MB pages, but never executable */
    if (nx) {
        for (int i = LOW_PTS; i < 2048; i++) pdt[i] |= nx;
    }

    /* Make the CPU honour read-only pages in kernel mode, then flush the TLB */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | (1ULL << 16)) : "memory");   /* CR0.WP */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
