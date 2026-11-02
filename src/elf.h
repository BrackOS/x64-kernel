#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>
#include "vmm.h"

/* What the loader learned about a program */
typedef struct {
    uint64_t entry;          /* where execution starts (already relocated) */
    uint64_t brk_start;      /* first free address after the program (heap starts here) */
    uint64_t phdr_addr;      /* where the program headers ended up (for auxv), 0 if unknown */
    uint16_t phnum, phent;
    uint64_t load_base;      /* 0 for fixed-address executables, the chosen base for PIE */
} elf_info_t;

/* Where position-independent programs are loaded */
#define ELF_PIE_BASE  (USER_BASE + 0x400000ULL)

/* Load an ELF64 x86-64 executable (ET_EXEC or ET_DYN/PIE) into a fresh address
 * space. Handles R_X86_64_RELATIVE relocations. Returns 0, or a negative error;
 * `why` (if not NULL) gets a short message. On failure the caller destroys the space. */
int elf_load(address_space_t *as, const uint8_t *img, size_t len, elf_info_t *info, const char **why);

/* Build the initial user stack (argc/argv/envp/auxv) and return the starting
 * stack pointer, or 0 on failure. Maps USER_STACK_PAGES pages below USER_STACK_TOP. */
uint64_t elf_build_stack(address_space_t *as, const elf_info_t *info,
                         int argc, char *const argv[], int envc, char *const envp[]);

#endif
