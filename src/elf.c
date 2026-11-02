#include "elf.h"
#include "string.h"

/* ---- ELF64 structures (only what we need) ---- */
#define EI_NIDENT 16
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct { int64_t d_tag; uint64_t d_val; } __attribute__((packed)) elf64_dyn_t;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } __attribute__((packed)) elf64_rela_t;

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 62
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_PHDR    6
#define PF_X 1
#define PF_W 2
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define R_X86_64_NONE      0
#define R_X86_64_RELATIVE  8

#define MAX_PHNUM 32

static int fail(const char **why, const char *msg) {
    if (why) *why = msg;
    return -1;
}

/* Make sure every page in [start, end) is mapped; add `flags` to pages that already exist */
static int ensure_pages(address_space_t *as, uint64_t start, uint64_t end, int flags) {
    for (uint64_t p = start & ~(PAGE_SIZE - 1); p < end; p += PAGE_SIZE) {
        int cur;
        if (vmm_translate(as, p, &cur)) {
            if ((cur | flags) != cur && vmm_protect(as, p, cur | flags) != 0) return -1;
        } else if (vmm_alloc_page(as, p, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

int elf_load(address_space_t *as, const uint8_t *img, size_t len, elf_info_t *info, const char **why) {
    memset(info, 0, sizeof(*info));
    if (len < sizeof(elf64_ehdr_t)) return fail(why, "file too small");

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return fail(why, "not an ELF file");
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return fail(why, "not a 64-bit little-endian ELF");
    if (eh->e_machine != EM_X86_64) return fail(why, "not an x86-64 program");
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return fail(why, "not an executable");
    if (eh->e_phentsize != sizeof(elf64_phdr_t) || eh->e_phnum == 0 || eh->e_phnum > MAX_PHNUM)
        return fail(why, "bad program header table");
    if (eh->e_phoff > len || (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t) > len - eh->e_phoff)
        return fail(why, "program headers outside the file");

    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff);
    uint64_t base = (eh->e_type == ET_DYN) ? ELF_PIE_BASE : 0;
    uint64_t top = 0;                       /* end of the highest segment */
    const elf64_phdr_t *dynamic = NULL;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dynamic = &ph[i];
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;

        uint64_t va = base + ph[i].p_vaddr;
        uint64_t end = va + ph[i].p_memsz;
        if (ph[i].p_filesz > ph[i].p_memsz) return fail(why, "segment file size larger than memory size");
        if (ph[i].p_offset > len || ph[i].p_filesz > len - ph[i].p_offset) return fail(why, "segment outside the file");
        if (va < USER_BASE || end < va || end > USER_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE - (1ULL << 30))
            return fail(why, "segment outside the user address range");

        /* Load as writable first so we can fill it; tighten to the real flags afterwards */
        if (ensure_pages(as, va, end, VM_WRITE) != 0) return fail(why, "out of memory");
        if (ph[i].p_filesz && vmm_copy_to_space(as, va, img + ph[i].p_offset, ph[i].p_filesz) != 0)
            return fail(why, "could not copy segment");
        /* (the BSS part is already zero: new pages are zero-filled) */
        if (end > top) top = end;
    }
    if (top == 0) return fail(why, "no loadable segments");

    /* Relocations (PIE): only RELATIVE ones, which is all a static-PIE needs */
    if (dynamic && eh->e_type == ET_DYN) {
        uint64_t rela = 0, relasz = 0, relaent = sizeof(elf64_rela_t);
        if (dynamic->p_offset > len || dynamic->p_filesz > len - dynamic->p_offset)
            return fail(why, "dynamic section outside the file");
        const elf64_dyn_t *d = (const elf64_dyn_t *)(img + dynamic->p_offset);
        for (size_t i = 0; i < dynamic->p_filesz / sizeof(elf64_dyn_t) && d[i].d_tag != DT_NULL; i++) {
            if (d[i].d_tag == DT_RELA)    rela = d[i].d_val;
            if (d[i].d_tag == DT_RELASZ)  relasz = d[i].d_val;
            if (d[i].d_tag == DT_RELAENT) relaent = d[i].d_val;
        }
        if (relasz) {
            if (relaent != sizeof(elf64_rela_t)) return fail(why, "unexpected relocation entry size");
            for (uint64_t off = 0; off + sizeof(elf64_rela_t) <= relasz; off += sizeof(elf64_rela_t)) {
                elf64_rela_t r;
                if (vmm_copy_from_space(as, base + rela + off, &r, sizeof(r)) != 0)
                    return fail(why, "bad relocation table");
                uint32_t type = (uint32_t)(r.r_info & 0xFFFFFFFF);
                if (type == R_X86_64_NONE) continue;
                if (type != R_X86_64_RELATIVE) return fail(why, "unsupported relocation type (link as static PIE)");
                uint64_t value = base + (uint64_t)r.r_addend;
                if (vmm_copy_to_space(as, base + r.r_offset, &value, sizeof(value)) != 0)
                    return fail(why, "relocation target not mapped");
            }
        }
    }

    /* Now apply the real permissions: code is not writable, data is not
     * executable. A page shared by two segments gets the union of their flags. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        uint64_t va = base + ph[i].p_vaddr, end = va + ph[i].p_memsz;
        for (uint64_t p = va & ~(PAGE_SIZE - 1); p < end; p += PAGE_SIZE) {
            int want = 0;
            for (int j = 0; j < eh->e_phnum; j++) {
                if (ph[j].p_type != PT_LOAD || ph[j].p_memsz == 0) continue;
                uint64_t jv = base + ph[j].p_vaddr, je = jv + ph[j].p_memsz;
                if (p < je && p + PAGE_SIZE > jv) {
                    want |= ((ph[j].p_flags & PF_W) ? VM_WRITE : 0) | ((ph[j].p_flags & PF_X) ? VM_EXEC : 0);
                }
            }
            vmm_protect(as, p, want);
        }
    }

    info->entry = base + eh->e_entry;
    info->brk_start = (top + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    info->load_base = base;
    info->phnum = eh->e_phnum;
    info->phent = eh->e_phentsize;
    for (int i = 0; i < eh->e_phnum; i++) {     /* where did the program headers land? */
        if (ph[i].p_type == PT_LOAD && eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
            info->phdr_addr = base + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    }
    return 0;
}

/* ---- Initial user stack ---- */

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_RANDOM 25

#define MAX_STACK_ARGS 64
#define MAX_STACK_BYTES 8192

uint64_t elf_build_stack(address_space_t *as, const elf_info_t *info,
                         int argc, char *const argv[], int envc, char *const envp[]) {
    if (argc < 0 || envc < 0 || argc + envc > MAX_STACK_ARGS) return 0;

    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        if (vmm_alloc_page(as, USER_STACK_TOP - (uint64_t)i * PAGE_SIZE, VM_WRITE) != 0) return 0;
    }

    /* Strings go at the very top; remember where each one landed */
    uint64_t sp = USER_STACK_TOP;
    uint64_t argv_addr[MAX_STACK_ARGS], envp_addr[MAX_STACK_ARGS];
    size_t total = 0;

    for (int i = envc - 1; i >= 0; i--) {
        size_t n = strlen(envp[i]) + 1;
        total += n;
        if (total > MAX_STACK_BYTES) return 0;
        sp -= n;
        if (vmm_copy_to_space(as, sp, envp[i], n) != 0) return 0;
        envp_addr[i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        total += n;
        if (total > MAX_STACK_BYTES) return 0;
        sp -= n;
        if (vmm_copy_to_space(as, sp, argv[i], n) != 0) return 0;
        argv_addr[i] = sp;
    }

    /* 16 bytes of "random" data for AT_RANDOM (not secret: there is no entropy source yet) */
    sp -= 16;
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(0xA5 ^ (i * 37));
    if (vmm_copy_to_space(as, sp, rnd, 16) != 0) return 0;
    uint64_t random_addr = sp;

    /* The pointer block: argc, argv[], NULL, envp[], NULL, auxv pairs */
    uint64_t aux[][2] = {
        { AT_PHDR, info->phdr_addr }, { AT_PHENT, info->phent }, { AT_PHNUM, info->phnum },
        { AT_PAGESZ, PAGE_SIZE }, { AT_ENTRY, info->entry },
        { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_RANDOM, random_addr }, { AT_NULL, 0 },
    };
    size_t naux = sizeof(aux) / sizeof(aux[0]);
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + naux * 2;
    sp -= words * 8;
    sp &= ~15ULL;                           /* the ABI wants rsp 16-byte aligned at entry */

    uint64_t block[1 + MAX_STACK_ARGS + 1 + MAX_STACK_ARGS + 1 + 24];
    size_t k = 0;
    block[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) block[k++] = argv_addr[i];
    block[k++] = 0;
    for (int i = 0; i < envc; i++) block[k++] = envp_addr[i];
    block[k++] = 0;
    for (size_t i = 0; i < naux; i++) { block[k++] = aux[i][0]; block[k++] = aux[i][1]; }

    if (vmm_copy_to_space(as, sp, block, k * 8) != 0) return 0;
    return sp;
}
