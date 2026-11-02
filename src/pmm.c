#include "pmm.h"
#include "multiboot.h"
#include "vga.h"
#include "string.h"
#include <stdint.h>

#define MAX_FRAMES  (1u << 20)          /* 4 GB / 4 KB */

/* One bit per frame: 1 = used / unusable, 0 = free */
static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames = 0;       /* usable frames found in the map */
static uint32_t free_frames  = 0;
static uint32_t search_hint  = 0;

static inline int  frame_used(uint32_t f) { return (bitmap[f >> 5] >> (f & 31)) & 1; }
static inline void frame_set(uint32_t f)  { bitmap[f >> 5] |=  (1u << (f & 31)); }
static inline void frame_clr(uint32_t f)  { bitmap[f >> 5] &= ~(1u << (f & 31)); }

static void mark_free_range(uint64_t base, uint64_t len) {
    uint64_t end = base + len;
    if (end > (uint64_t)MAX_FRAMES * PMM_FRAME_SIZE) end = (uint64_t)MAX_FRAMES * PMM_FRAME_SIZE;
    uint64_t first = (base + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;   /* round start up */
    uint64_t last  = end / PMM_FRAME_SIZE;                           /* round end down */
    for (uint64_t f = first; f < last; f++) {
        if (frame_used((uint32_t)f)) { frame_clr((uint32_t)f); total_frames++; free_frames++; }
    }
}

static void reserve_range(uint64_t base, uint64_t len) {
    uint64_t first = base / PMM_FRAME_SIZE;                          /* round start down */
    uint64_t last  = (base + len + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (last > MAX_FRAMES) last = MAX_FRAMES;
    for (uint64_t f = first; f < last; f++) {
        if (!frame_used((uint32_t)f)) { frame_set((uint32_t)f); free_frames--; }
    }
}

void pmm_init(uint32_t mbi_addr) {
    extern char __kernel_bss_end[];

    memset(bitmap, 0xFF, sizeof(bitmap));          /* everything used until proven free */
    total_frames = free_frames = 0;

    const mb_mmap_tag_t *tag = (const mb_mmap_tag_t *)multiboot_find_tag(MB_TAG_MMAP);
    if (tag && tag->entry_size >= sizeof(mb_mmap_entry_t)) {
        uint32_t count = (tag->size - sizeof(mb_mmap_tag_t)) / tag->entry_size;
        const uint8_t *base = (const uint8_t *)tag + sizeof(mb_mmap_tag_t);
        for (uint32_t i = 0; i < count; i++) {
            const mb_mmap_entry_t *e = (const mb_mmap_entry_t *)(base + (uint64_t)i * tag->entry_size);
            if (e->type == MB_MEM_AVAILABLE) mark_free_range(e->base, e->length);
        }
    } else {
        /* No map from GRUB: assume the 15 MB above the first megabyte exist */
        vga_print("PMM: no memory map from GRUB, assuming 1-16 MB\n");
        mark_free_range(0x100000, 15u * 1024 * 1024);
    }

    /* Never hand out: the first megabyte (BIOS, VGA, page 0), the kernel
     * image with its BSS, and GRUB's boot information block. */
    reserve_range(0, 0x100000);
    reserve_range(0x100000, (uint64_t)(uintptr_t)__kernel_bss_end - 0x100000);
    if (mbi_addr) {
        reserve_range(mbi_addr, *(const uint32_t *)(uintptr_t)mbi_addr);
    }
    search_hint = 0;
}

uint64_t pmm_alloc_frames(uint32_t count) {
    if (count == 0 || count > free_frames) return 0;

    uint32_t run = 0, start = 0;
    for (uint32_t n = 0; n < MAX_FRAMES; n++) {
        uint32_t f = (n + search_hint) % MAX_FRAMES;
        if (f == 0) run = 0;                        /* a run cannot wrap around */
        if (frame_used(f)) { run = 0; continue; }
        if (run == 0) start = f;
        if (++run == count) {
            for (uint32_t i = 0; i < count; i++) frame_set(start + i);
            free_frames -= count;
            search_hint = start + count;
            return (uint64_t)start * PMM_FRAME_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_frame(void) {
    return pmm_alloc_frames(1);
}

void pmm_free_frames(uint64_t addr, uint32_t count) {
    uint64_t f0 = addr / PMM_FRAME_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t f = f0 + i;
        if (f == 0 || f >= MAX_FRAMES) continue;
        if (frame_used((uint32_t)f)) { frame_clr((uint32_t)f); free_frames++; }
    }
}

void pmm_free_frame(uint64_t addr) {
    pmm_free_frames(addr, 1);
}

uint32_t pmm_total_frames(void)      { return total_frames; }
uint32_t pmm_free_frame_count(void)  { return free_frames; }

void pmm_print_stats(void) {
    vga_print("Physical memory (4 KB frames):\n  Usable: ");
    vga_print_int((int)total_frames);
    vga_print(" frames, ");
    vga_print_int((int)(total_frames / 256));
    vga_print(" MB\n  Free:   ");
    vga_print_int((int)free_frames);
    vga_print(" frames, ");
    vga_print_int((int)(free_frames / 256));
    vga_print(" MB\n  In use: ");
    vga_print_int((int)(total_frames - free_frames));
    vga_print(" frames (kernel, boot info, heap)\n");
}
