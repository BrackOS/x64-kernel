#include "process.h"
#include "sched.h"
#include "elf.h"
#include "embedded.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "fat32.h"
#include "fs_shell.h"
#include "fpu.h"
#include "gdt.h"
#include "brkf_kernel.h"

#define E_NOENT   (-2)
#define E_NOMEM   (-12)
#define E_NOEXEC  (-8)
#define E_2BIG    (-7)

int proc_load_image(const char *path, const uint8_t **data, size_t *len, int *needs_free) {
    *needs_free = 0;
    int has_slash = 0;
    for (const char *p = path; *p; p++) if (*p == '/') has_slash = 1;

    /* 1. a bare name that is built into the kernel */
    if (!has_slash && embedded_find(path, data, len) == 0) return 0;

    /* 2. the BRKF disk: the path as given, then /bin/<name> for a bare name */
    if (kbrkf_mounted()) {
        uint8_t *buf; size_t n;
        uint32_t cwd = sched_current()->cwd ? sched_current()->cwd : 1;
        int r = kbrkf_read_file(cwd, path, &buf, &n);
        if (r != 0 && !has_slash && strlen(path) < 48) {
            char alt[64];
            strcpy(alt, "/bin/");
            strcpy(alt + 5, path);
            r = kbrkf_read_file(1, alt, &buf, &n);
        }
        if (r == 0) { *data = buf; *len = n; *needs_free = 1; return 0; }
    }

    /* 3. built-in by file name (e.g. /bin/hello when there is no disk) */
    if (embedded_find(path, data, len) == 0) return 0;

    /* 4. a file in the current FAT32 directory */
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == ':') name = p + 1;
    dir_entry_t e;
    if (fat32_find_in_dir(current_dir.cluster, name, &e) != 0) return E_NOENT;
    if (e.file_size == 0 || e.file_size > (8u << 20)) return E_NOEXEC;
    uint8_t *buf = (uint8_t *)kmalloc(e.file_size);
    if (!buf) return E_NOMEM;
    int got = fat32_read_file(current_dir.cluster, name, buf, e.file_size);
    if (got <= 0) { kfree(buf); return E_NOENT; }
    *data = buf;
    *len = (size_t)got;
    *needs_free = 1;
    return 0;
}

/* Build a fresh address space holding the program, its stack and heap bookkeeping.
 * On success *sp_out is the initial stack pointer. */
static address_space_t *build_image(const uint8_t *img, size_t len, int argc, char *const argv[],
                                    int envc, char *const envp[], elf_info_t *info,
                                    uint64_t *sp_out, const char **why) {
    address_space_t *as = vmm_create();
    if (!as) { *why = "out of memory"; return NULL; }
    if (elf_load(as, img, len, info, why) != 0) { vmm_destroy(as); return NULL; }
    uint64_t sp = elf_build_stack(as, info, argc, argv, envc, envp);
    if (!sp) { *why = "could not build the user stack"; vmm_destroy(as); return NULL; }
    *sp_out = sp;
    return as;
}

static void init_user_frame(trap_frame_t *f, uint64_t entry, uint64_t sp) {
    memset(f, 0, sizeof(*f));
    f->rip = entry;
    f->cs = SEL_USER_CODE;
    f->ss = SEL_USER_DATA;
    f->rflags = 0x202;                       /* interrupts on */
    f->rsp = sp;
}

static void init_task_memory(task_t *t, const elf_info_t *info) {
    t->brk_start = t->brk = info->brk_start;
    t->mmap_next = USER_STACK_TOP - (256ULL << 20);     /* anonymous mappings grow down from here */
}

int proc_spawn(const char *path, int argc, char *const argv[], int verbose) {
    const uint8_t *img; size_t len; int must_free;
    int r = proc_load_image(path, &img, &len, &must_free);
    if (r != 0) {
        if (verbose) { vga_print(path); vga_print(": program not found\n"); }
        return r;
    }

    elf_info_t info; uint64_t sp; const char *why = "";
    address_space_t *as = build_image(img, len, argc, argv, 0, NULL, &info, &sp, &why);
    if (must_free) kfree((void *)img);
    if (!as) {
        if (verbose) { vga_print(path); vga_print(": cannot run: "); vga_print(why); vga_print("\n"); }
        return E_NOEXEC;
    }

    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    task_t *t = task_alloc(base);
    if (!t) { vmm_destroy(as); return E_NOMEM; }
    t->ppid = sched_current()->pid;
    t->cwd = sched_current()->cwd ? sched_current()->cwd : 1;
    t->as = as;
    init_task_memory(t, &info);
    fpu_reset_state(t->fpu);

    trap_frame_t f;
    init_user_frame(&f, info.entry, sp);
    task_prepare_user_stack(t, &f);
    int pid = t->pid;
    sched_make_ready(t);
    return pid;
}

int proc_fork(const trap_frame_t *frame) {
    task_t *parent = sched_current();
    if (!parent->as) return E_NOEXEC;

    task_t *child = task_alloc(parent->name);
    if (!child) return E_NOMEM;
    child->as = vmm_clone(parent->as);
    if (!child->as) { task_free(child); return E_NOMEM; }

    child->ppid = parent->pid;
    memcpy(child->files, parent->files, sizeof(child->files));   /* the child inherits open files */
    child->cwd = parent->cwd;
    child->brk_start = parent->brk_start;
    child->brk = parent->brk;
    child->mmap_next = parent->mmap_next;
    fpu_save(parent->fpu);                          /* current registers -> parent's area ... */
    memcpy(child->fpu, parent->fpu, sizeof(child->fpu));   /* ... and on to the child */

    trap_frame_t cf = *frame;
    cf.rax = 0;                                     /* fork returns 0 in the child */
    task_prepare_user_stack(child, &cf);
    int pid = child->pid;
    sched_make_ready(child);
    return pid;
}

int proc_exec(trap_frame_t *frame, const char *path, int argc, char *const argv[], int envc, char *const envp[]) {
    task_t *t = sched_current();
    const uint8_t *img; size_t len; int must_free;
    int r = proc_load_image(path, &img, &len, &must_free);
    if (r != 0) return r;

    elf_info_t info; uint64_t sp; const char *why = "";
    address_space_t *as = build_image(img, len, argc, argv, envc, envp, &info, &sp, &why);
    if (must_free) kfree((void *)img);
    if (!as) return E_NOEXEC;

    /* Point of no return: switch to the new image and drop the old one */
    address_space_t *old = t->as;
    t->as = as;
    vmm_switch(as);
    vmm_destroy(old);
    init_task_memory(t, &info);
    fpu_reset_state(t->fpu);
    fpu_restore(t->fpu);

    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    strncpy_safe(t->name, base, sizeof(t->name));

    init_user_frame(frame, info.entry, sp);         /* the syscall now "returns" into the new program */
    return 0;
}

void proc_user_fault(const trap_frame_t *frame, uint64_t vector, uint64_t error) {
    task_t *t = sched_current();
    int sig = 11;                                   /* SIGSEGV */
    const char *what = "invalid memory access";
    switch (vector) {
        case 0:  sig = 8;  what = "division by zero"; break;
        case 6:  sig = 4;  what = "illegal instruction"; break;
        case 13: sig = 11; what = "protection fault"; break;
        case 14: sig = 11; what = "invalid memory access"; break;
        case 16: case 19: sig = 8; what = "floating point error"; break;
        case 17: sig = 7;  what = "misaligned access"; break;
        default: break;
    }
    uint64_t cr2 = 0;
    if (vector == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    vga_set_color(12, 0);
    vga_print("\nProcess ");
    vga_print_int(t->pid);
    vga_print(" (");
    vga_print(t->name);
    vga_print(") killed: ");
    vga_print(what);
    vga_print(" at ");
    vga_print_hex(frame->rip);
    if (vector == 14) {
        vga_print(", address ");
        vga_print_hex(cr2);
        if (error & 0x10) vga_print(" (not executable)");
        else if (error & 0x2) vga_print(error & 0x1 ? " (write to read-only)" : " (write, not mapped)");
        else vga_print(error & 0x1 ? " (no permission)" : " (not mapped)");
    }
    vga_print("\n");
    vga_set_color(7, 0);

    sched_exit_current(sig);
}
