#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"

/* Boot page tables from boot.s: slot 0 maps the first 4 GB for the kernel */
extern uint64_t pml4[512];
extern uint64_t paging_nx_bit(void);       /* 0 if the CPU has no NX */

#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_US   (1ULL << 2)
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL
#define PTE_PS   (1ULL << 7)

static address_space_t kernel_space;
static address_space_t *current_space = &kernel_space;

static inline uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)PHYS_TO_VIRT(phys);
}

static uint64_t new_table(void) {
    uint64_t p = pmm_alloc_frame();
    if (!p) return 0;
    memset(table_at(p), 0, PAGE_SIZE);
    return p;
}

void vmm_init(void) {
    kernel_space.pml4 = pml4;
    kernel_space.pml4_phys = VIRT_TO_PHYS(pml4);
    current_space = &kernel_space;
}

address_space_t *vmm_kernel_space(void) { return &kernel_space; }
address_space_t *vmm_current(void)      { return current_space; }

address_space_t *vmm_create(void) {
    address_space_t *as = (address_space_t *)kmalloc(sizeof(address_space_t));
    if (!as) return NULL;
    uint64_t p = new_table();
    if (!p) { kfree(as); return NULL; }
    as->pml4_phys = p;
    as->pml4 = table_at(p);
    as->pml4[0] = kernel_space.pml4[0];       /* share the kernel mapping */
    return as;
}

void vmm_switch(address_space_t *as) {
    current_space = as;
    __asm__ volatile("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}

/* Find the PTE for a user address, creating the intermediate tables if asked */
static uint64_t *walk(address_space_t *as, uint64_t virt, int create) {
    if (virt < USER_BASE || virt >= USER_TOP) return NULL;
    unsigned idx[3] = {
        (unsigned)((virt >> 39) & 511), (unsigned)((virt >> 30) & 511), (unsigned)((virt >> 21) & 511)
    };
    uint64_t *t = as->pml4;
    for (int k = 0; k < 3; k++) {
        uint64_t e = t[idx[k]];
        if (!(e & PTE_P)) {
            if (!create) return NULL;
            uint64_t p = new_table();
            if (!p) return NULL;
            e = p | PTE_P | PTE_RW | PTE_US;
            t[idx[k]] = e;
        }
        t = table_at(e & PTE_ADDR);
    }
    return &t[(virt >> 12) & 511];
}

static uint64_t flags_to_pte(int flags) {
    uint64_t pte = PTE_P | PTE_US;
    if (flags & VM_WRITE) pte |= PTE_RW;
    if (!(flags & VM_EXEC)) pte |= paging_nx_bit();
    return pte;
}

static inline void flush_page(address_space_t *as, uint64_t virt) {
    if (as == current_space) __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

int vmm_map_page(address_space_t *as, uint64_t virt, uint64_t phys, int flags) {
    if (virt & (PAGE_SIZE - 1) || phys & (PAGE_SIZE - 1)) return -1;
    uint64_t *pte = walk(as, virt, 1);
    if (!pte || (*pte & PTE_P)) return -1;
    *pte = phys | flags_to_pte(flags);
    flush_page(as, virt);
    return 0;
}

int vmm_alloc_page(address_space_t *as, uint64_t virt, int flags) {
    uint64_t f = pmm_alloc_frame();
    if (!f) return -1;
    memset(table_at(f), 0, PAGE_SIZE);
    if (vmm_map_page(as, virt, f, flags) != 0) {
        pmm_free_frame(f);
        return -1;
    }
    return 0;
}

int vmm_unmap_page(address_space_t *as, uint64_t virt, int free_frame) {
    uint64_t *pte = walk(as, virt, 0);
    if (!pte || !(*pte & PTE_P)) return -1;
    uint64_t phys = *pte & PTE_ADDR;
    *pte = 0;
    flush_page(as, virt);
    if (free_frame) pmm_free_frame(phys);
    return 0;
}

int vmm_protect(address_space_t *as, uint64_t virt, int flags) {
    uint64_t *pte = walk(as, virt, 0);
    if (!pte || !(*pte & PTE_P)) return -1;
    *pte = (*pte & PTE_ADDR) | flags_to_pte(flags);
    flush_page(as, virt);
    return 0;
}

uint64_t vmm_translate(address_space_t *as, uint64_t virt, int *flags_out) {
    uint64_t *pte = walk(as, virt & ~(PAGE_SIZE - 1), 0);
    if (!pte || !(*pte & PTE_P) || !(*pte & PTE_US)) return 0;
    if (flags_out) {
        *flags_out = ((*pte & PTE_RW) ? VM_WRITE : 0) | ((*pte & PTE_NX) ? 0 : VM_EXEC);
    }
    return (*pte & PTE_ADDR) | (virt & (PAGE_SIZE - 1));
}

/* Visit every present user leaf page: fn(virt, pte_pointer, ctx) */
typedef int (*leaf_fn)(uint64_t virt, uint64_t *pte, void *ctx);

static int for_each_leaf(address_space_t *as, leaf_fn fn, void *ctx) {
    for (unsigned i4 = 1; i4 < 256; i4++) {
        if (!(as->pml4[i4] & PTE_P)) continue;
        uint64_t *pdpt = table_at(as->pml4[i4] & PTE_ADDR);
        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_P)) continue;
            uint64_t *pd = table_at(pdpt[i3] & PTE_ADDR);
            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_P)) continue;
                uint64_t *pt = table_at(pd[i2] & PTE_ADDR);
                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if (!(pt[i1] & PTE_P)) continue;
                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                    ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    int r = fn(virt, &pt[i1], ctx);
                    if (r) return r;
                }
            }
        }
    }
    return 0;
}

void vmm_destroy(address_space_t *as) {
    if (!as || as == &kernel_space) return;
    if (as == current_space) vmm_switch(&kernel_space);   /* never free the tables in use */

    for (unsigned i4 = 1; i4 < 256; i4++) {
        if (!(as->pml4[i4] & PTE_P)) continue;
        uint64_t pdpt_p = as->pml4[i4] & PTE_ADDR;
        uint64_t *pdpt = table_at(pdpt_p);
        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_P)) continue;
            uint64_t pd_p = pdpt[i3] & PTE_ADDR;
            uint64_t *pd = table_at(pd_p);
            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_P)) continue;
                uint64_t pt_p = pd[i2] & PTE_ADDR;
                uint64_t *pt = table_at(pt_p);
                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if ((pt[i1] & PTE_P) && (pt[i1] & PTE_US)) pmm_free_frame(pt[i1] & PTE_ADDR);
                }
                pmm_free_frame(pt_p);
            }
            pmm_free_frame(pd_p);
        }
        pmm_free_frame(pdpt_p);
    }
    pmm_free_frame(as->pml4_phys);
    kfree(as);
}

static int clone_leaf(uint64_t virt, uint64_t *pte, void *ctx) {
    address_space_t *dst = (address_space_t *)ctx;
    uint64_t f = pmm_alloc_frame();
    if (!f) return -1;
    memcpy(table_at(f), table_at(*pte & PTE_ADDR), PAGE_SIZE);
    uint64_t *npte = walk(dst, virt, 1);
    if (!npte) { pmm_free_frame(f); return -1; }
    *npte = f | (*pte & ~PTE_ADDR);              /* same permissions */
    return 0;
}

address_space_t *vmm_clone(address_space_t *as) {
    address_space_t *copy = vmm_create();
    if (!copy) return NULL;
    if (for_each_leaf(as, clone_leaf, copy) != 0) {
        vmm_destroy(copy);
        return NULL;
    }
    return copy;
}

/* ---- Copying to and from a user address space ---- */

typedef enum { OP_TO, OP_FROM, OP_ZERO } copy_op_t;

static int copy_space(address_space_t *as, uint64_t uaddr, void *kbuf, size_t n, copy_op_t op) {
    if (n == 0) return 0;
    if (uaddr < USER_BASE || uaddr + n < uaddr || uaddr + n > USER_TOP) return -1;
    uint8_t *k = (uint8_t *)kbuf;
    while (n > 0) {
        uint64_t *pte = walk(as, uaddr & ~(PAGE_SIZE - 1), 0);
        if (!pte || !(*pte & PTE_P) || !(*pte & PTE_US)) return -1;
        if (op != OP_FROM && !(*pte & PTE_RW)) return -1;
        size_t off = (size_t)(uaddr & (PAGE_SIZE - 1));
        size_t chunk = (size_t)PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        uint8_t *page = (uint8_t *)table_at(*pte & PTE_ADDR) + off;
        if (op == OP_TO)        memcpy(page, k, chunk);
        else if (op == OP_FROM) memcpy(k, page, chunk);
        else                    memset(page, 0, chunk);
        if (op != OP_ZERO) k += chunk;
        uaddr += chunk;
        n -= chunk;
    }
    return 0;
}

int vmm_copy_to_space(address_space_t *as, uint64_t uaddr, const void *src, size_t n) {
    return copy_space(as, uaddr, (void *)src, n, OP_TO);
}
int vmm_copy_from_space(address_space_t *as, uint64_t uaddr, void *dst, size_t n) {
    return copy_space(as, uaddr, dst, n, OP_FROM);
}
int vmm_zero_in_space(address_space_t *as, uint64_t uaddr, size_t n) {
    return copy_space(as, uaddr, NULL, n, OP_ZERO);
}

int vmm_strncpy_from_space(address_space_t *as, uint64_t uaddr, char *dst, size_t max) {
    if (max == 0) return -1;
    for (size_t i = 0; i < max - 1; i++) {
        char c;
        if (vmm_copy_from_space(as, uaddr + i, &c, 1) != 0) return -1;
        dst[i] = c;
        if (c == '\0') return (int)i;
    }
    dst[max - 1] = '\0';
    return (int)(max - 1);
}

int copy_from_user(void *dst, uint64_t usrc, size_t n) { return vmm_copy_from_space(current_space, usrc, dst, n); }
int copy_to_user(uint64_t udst, const void *src, size_t n) { return vmm_copy_to_space(current_space, udst, src, n); }
int strncpy_from_user(char *dst, uint64_t usrc, size_t max) { return vmm_strncpy_from_space(current_space, usrc, dst, max); }
