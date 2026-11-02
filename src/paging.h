#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Tighten the boot page tables: unmap page 0 (null dereferences fault),
 * unmap a guard page under the kernel stack, make code read-only+executable,
 * make read-only data non-writable, and make everything else non-executable
 * (W^X). Uses the NX bit when the CPU has it. */
void paging_init(void);
uint64_t paging_nx_bit(void);
int paging_make_uncached(uint64_t phys);

#endif
