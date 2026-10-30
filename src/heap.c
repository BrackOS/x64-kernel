#include "heap.h"
#include "vga.h"
#include <stdint.h>

/* Every block (free or used) starts with a header. Sizes and the header are
 * rounded to 16 bytes so returned pointers are 16-byte aligned. */
#define HEAP_ALIGN      16
#define HEAP_MIN_BLOCK  32            /* smallest leftover worth splitting off */
#define MAGIC_USED      0xA110CA7Eu
#define MAGIC_FREE      0xF4EEB10Cu

typedef struct heap_block {
    struct heap_block *next;
    size_t size;                      /* whole block, header included */
    uint32_t magic;                   /* MAGIC_USED or MAGIC_FREE */
    uint32_t reserved;
} heap_block_t;

#define ALIGN_UP(x)  (((x) + (HEAP_ALIGN - 1)) & ~(size_t)(HEAP_ALIGN - 1))
#define HDR_SIZE     ALIGN_UP(sizeof(heap_block_t))

static heap_block_t *heap_head = NULL;
static uint8_t *heap_start = NULL;
static size_t heap_total = 0;

void heap_init(void *start, size_t size) {
    /* Align the start so every returned pointer is 16-byte aligned */
    uintptr_t s = ((uintptr_t)start + (HEAP_ALIGN - 1)) & ~(uintptr_t)(HEAP_ALIGN - 1);
    size -= (size_t)(s - (uintptr_t)start);
    size &= ~(size_t)(HEAP_ALIGN - 1);

    heap_start = (uint8_t *)s;
    heap_total = size;
    heap_head = (heap_block_t *)s;
    heap_head->next = NULL;
    heap_head->size = size;
    heap_head->magic = MAGIC_FREE;
}

void *kmalloc(size_t size) {
    if (size == 0 || heap_head == NULL) return NULL;
    if (size > heap_total) return NULL;

    size_t needed = ALIGN_UP(size) + HDR_SIZE;

    /* First fit */
    for (heap_block_t *block = heap_head; block; block = block->next) {
        if (block->magic == MAGIC_FREE && block->size >= needed) {
            /* Split off the leftover if it is big enough to be useful */
            if (block->size >= needed + HDR_SIZE + HEAP_MIN_BLOCK) {
                heap_block_t *rest = (heap_block_t *)((uint8_t *)block + needed);
                rest->next = block->next;
                rest->size = block->size - needed;
                rest->magic = MAGIC_FREE;
                block->next = rest;
                block->size = needed;
            }
            block->magic = MAGIC_USED;
            return (void *)((uint8_t *)block + HDR_SIZE);
        }
    }
    return NULL;   /* Out of memory */
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    uint8_t *p = (uint8_t *)ptr;
    if (p < heap_start + HDR_SIZE || p >= heap_start + heap_total) {
        vga_print("kfree: pointer outside heap\n");
        return;
    }
    heap_block_t *block = (heap_block_t *)(p - HDR_SIZE);
    if (block->magic != MAGIC_USED) {
        vga_print(block->magic == MAGIC_FREE ? "kfree: double free\n"
                                              : "kfree: invalid pointer\n");
        return;
    }
    block->magic = MAGIC_FREE;

    /* Coalesce with the next block if it is free */
    if (block->next && block->next->magic == MAGIC_FREE) {
        block->size += block->next->size;
        block->next = block->next->next;
    }

    /* Coalesce with the previous block if it is free */
    if (block != heap_head) {
        heap_block_t *prev = heap_head;
        while (prev && prev->next != block) prev = prev->next;
        if (prev && prev->magic == MAGIC_FREE) {
            prev->size += block->size;
            prev->next = block->next;
        }
    }
}

void heap_stats(void) {
    int free_blocks = 0, used_blocks = 0;
    size_t free_bytes = 0, used_bytes = 0;
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->magic == MAGIC_FREE) { free_blocks++; free_bytes += b->size; }
        else                        { used_blocks++; used_bytes += b->size; }
    }
    vga_print("Heap: ");
    vga_print_int((int)heap_total);
    vga_print(" bytes total\n  Used: ");
    vga_print_int(used_blocks);
    vga_print(" blocks, ");
    vga_print_int((int)used_bytes);
    vga_print(" bytes\n  Free: ");
    vga_print_int(free_blocks);
    vga_print(" blocks, ");
    vga_print_int((int)free_bytes);
    vga_print(" bytes\n");
}
