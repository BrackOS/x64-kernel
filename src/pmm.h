#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096

/* Physical page allocator. Built from the Multiboot2 memory map; manages the
 * first 4 GB (the range the kernel identity-maps). Frame 0 is never handed
 * out, so a return value of 0 always means "out of memory". */
void     pmm_init(uint32_t mbi_addr);
uint64_t pmm_alloc_frame(void);                    /* one 4 KB frame */
uint64_t pmm_alloc_frames(uint32_t count);         /* contiguous run */
void     pmm_free_frame(uint64_t addr);
void     pmm_free_frames(uint64_t addr, uint32_t count);
uint32_t pmm_total_frames(void);
uint32_t pmm_free_frame_count(void);
void     pmm_print_stats(void);

#endif
