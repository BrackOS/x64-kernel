#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Multiboot2 tag types used by the kernel */
#define MB_TAG_END          0
#define MB_TAG_MMAP         6
#define MB_TAG_FRAMEBUFFER  8
#define MB_TAG_ACPI_OLD     14   /* copy of the ACPI 1.0 RSDP */
#define MB_TAG_ACPI_NEW     15   /* copy of the ACPI 2.0+ RSDP */

/* Memory map tag layout (Multiboot2 spec) */
typedef struct {
    uint32_t type;          /* 6 = memory map */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} mb_mmap_tag_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;          /* 1 = available RAM */
    uint32_t reserved;
} mb_mmap_entry_t;

#define MB_MEM_AVAILABLE  1

int multiboot_cmdline_has(const char *word);   /* word on the GRUB command line? */

/* Store the Multiboot2 info pointer that GRUB passed to the kernel */
void multiboot_init(uint32_t mbi_addr);

/* Return the first tag of the given type, or NULL if GRUB did not pass it */
const void *multiboot_find_tag(uint32_t type);

#endif
