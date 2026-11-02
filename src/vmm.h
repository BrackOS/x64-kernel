#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

/* Virtual memory: one address space (one PML4) per process.
 *
 * Layout. The kernel is identity-mapped in the first 4 GB (PML4 slot 0,
 * supervisor-only) and that slot is shared by every address space, so kernel
 * code keeps working no matter which space is loaded. User memory lives in
 * PML4 slots 1-255: virtual addresses USER_BASE .. USER_TOP. User programs
 * are therefore position independent (PIE) and linked for this range.
 *
 * PHYS_TO_VIRT is the identity today. All kernel code goes through it so a
 * later move to a higher-half kernel only has to change these two macros. */
#define PHYS_TO_VIRT(p)   ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v)   ((uint64_t)(uintptr_t)(v))

#define PAGE_SIZE         4096ULL
#define USER_BASE         0x0000008000000000ULL   /* 512 GB */
#define USER_TOP          0x00007FFFFFFFF000ULL   /* exclusive */
#define USER_STACK_TOP    USER_TOP
#define USER_STACK_PAGES  16                       /* 64 KB initial stack */

/* Page permissions for vmm_map_page() and friends (readable is implied) */
#define VM_WRITE  0x1
#define VM_EXEC   0x2

typedef struct address_space {
    uint64_t *pml4;          /* kernel pointer to the top-level table */
    uint64_t  pml4_phys;     /* what goes into CR3 */
} address_space_t;

void             vmm_init(void);
address_space_t *vmm_kernel_space(void);          /* the boot tables: no user mappings */
address_space_t *vmm_current(void);
address_space_t *vmm_create(void);                /* empty user space, kernel mapped */
void             vmm_destroy(address_space_t *as);/* frees every user page and table */
address_space_t *vmm_clone(address_space_t *as);  /* deep copy of all user pages (fork) */
void             vmm_switch(address_space_t *as); /* load CR3 */

int      vmm_map_page(address_space_t *as, uint64_t virt, uint64_t phys, int flags);
int      vmm_alloc_page(address_space_t *as, uint64_t virt, int flags);   /* zeroed */
int      vmm_unmap_page(address_space_t *as, uint64_t virt, int free_frame);
int      vmm_protect(address_space_t *as, uint64_t virt, int flags);
uint64_t vmm_translate(address_space_t *as, uint64_t virt, int *flags_out);  /* 0 if unmapped */

/* Copy between kernel memory and a user address space. These walk the page
 * tables, so they never fault and never depend on which space is loaded.
 * Return 0 on success, -1 if any page is unmapped or not user-accessible. */
int vmm_copy_to_space(address_space_t *as, uint64_t uaddr, const void *src, size_t n);
int vmm_copy_from_space(address_space_t *as, uint64_t uaddr, void *dst, size_t n);
int vmm_zero_in_space(address_space_t *as, uint64_t uaddr, size_t n);
/* Copy a NUL-terminated string (at most max-1 bytes + NUL). Returns its length or -1. */
int vmm_strncpy_from_space(address_space_t *as, uint64_t uaddr, char *dst, size_t max);

/* Same, for the address space that is currently loaded (used by syscalls) */
int copy_from_user(void *dst, uint64_t usrc, size_t n);
int copy_to_user(uint64_t udst, const void *src, size_t n);
int strncpy_from_user(char *dst, uint64_t usrc, size_t max);

#endif
