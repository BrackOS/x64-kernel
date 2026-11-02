#include "syscall.h"
#include "sched.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "input.h"
#include "timer.h"
#include "io.h"
#include "gdt.h"
#include "vfs.h"

/* Linux x86-64 system call numbers (the subset we implement) */
#define SYS_read         0
#define SYS_write        1
#define SYS_open         2
#define SYS_close        3
#define SYS_stat         4
#define SYS_fstat        5
#define SYS_lstat        6
#define SYS_lseek        8
#define SYS_ftruncate    77
#define SYS_getcwd       79
#define SYS_chdir        80
#define SYS_rename       82
#define SYS_mkdir        83
#define SYS_rmdir        84
#define SYS_unlink       87
#define SYS_readlink     89
#define SYS_getdents64   217
#define SYS_mmap         9
#define SYS_munmap       11
#define SYS_brk          12
#define SYS_sched_yield  24
#define SYS_nanosleep    35
#define SYS_getpid       39
#define SYS_fork         57
#define SYS_execve       59
#define SYS_exit         60
#define SYS_wait4        61
#define SYS_getppid      110
#define SYS_exit_group   231

#define ENOENT   2
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define ENOSYS  38

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

uint64_t user_rsp_scratch = 0;           /* used by syscall_entry while switching stacks */

void syscall_entry(void);

/* The SYSCALL instruction does not switch stacks or save anything, so this
 * stub does it by hand and builds the same trap frame the interrupt stubs do. */
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".global syscall_entry\n"
    "syscall_entry:\n"
    "    mov [rip + user_rsp_scratch], rsp\n"
    "    mov rsp, [rip + kernel_rsp_scratch]\n"
    "    push 0x1B\n"                                   /* ss  (user data selector) */
    "    push qword ptr [rip + user_rsp_scratch]\n"     /* user rsp */
    "    push r11\n"                                    /* rflags (saved by SYSCALL) */
    "    push 0x23\n"                                   /* cs  (user code selector) */
    "    push rcx\n"                                    /* rip (saved by SYSCALL) */
    "    push 0\n"                                      /* error code */
    "    push 0x80\n"                                   /* vector: marks a syscall */
    "    push rax\n"
    "    push rbx\n"
    "    push rcx\n"
    "    push rdx\n"
    "    push rsi\n"
    "    push rdi\n"
    "    push rbp\n"
    "    push r8\n"
    "    push r9\n"
    "    push r10\n"
    "    push r11\n"
    "    push r12\n"
    "    push r13\n"
    "    push r14\n"
    "    push r15\n"
    "    mov rdi, rsp\n"
    "    cld\n"
    "    call syscall_dispatch\n"
    "    cli\n"
    "    pop r15\n"
    "    pop r14\n"
    "    pop r13\n"
    "    pop r12\n"
    "    pop r11\n"
    "    pop r10\n"
    "    pop r9\n"
    "    pop r8\n"
    "    pop rbp\n"
    "    pop rdi\n"
    "    pop rsi\n"
    "    pop rdx\n"
    "    pop rcx\n"
    "    pop rbx\n"
    "    pop rax\n"
    "    add rsp, 16\n"
    "    iretq\n"
    ".att_syntax prefix\n"
);

void syscall_init(void) {
    write_msr(MSR_EFER, read_msr64(MSR_EFER) | 1);                    /* SCE: enable SYSCALL */
    write_msr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)SEL_KERNEL_CODE << 32));
    write_msr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    write_msr(MSR_SFMASK, 0x700);                                     /* clear IF, DF, TF on entry */
}

/* ---- Individual system calls ---- */

static long sys_write(long fd, uint64_t ubuf, uint64_t count) {
    if (vfs_is_file_fd((int)fd)) return vfs_write((int)fd, ubuf, count);
    if (fd != 1 && fd != 2) return -9;                       /* EBADF */
    char buf[256];
    uint64_t done = 0;
    while (done < count) {
        size_t chunk = (count - done > sizeof(buf)) ? sizeof(buf) : (size_t)(count - done);
        if (copy_from_user(buf, ubuf + done, chunk) != 0) return done ? (long)done : -EFAULT;
        for (size_t i = 0; i < chunk; i++) vga_putchar(buf[i]);
        done += chunk;
    }
    return (long)done;
}

/* Console input: one line at a time, with echo and backspace */
static long sys_read(long fd, uint64_t ubuf, uint64_t count) {
    if (vfs_is_file_fd((int)fd)) return vfs_read((int)fd, ubuf, count);
    if (fd != 0) return -9;                                  /* EBADF */
    if (count == 0) return 0;
    char line[256];
    size_t max = count < sizeof(line) ? (size_t)count : sizeof(line);
    size_t n = 0;

    for (;;) {
        input_event_t ev;
        if (!input_poll(&ev)) { sched_sleep_ticks(1); continue; }   /* let others run meanwhile */
        if (ev.type != INPUT_KEY) continue;
        char c = ev.key;
        if (c == '\b') { if (n > 0) { n--; vga_backspace(); } continue; }
        if (c == '\n') { line[n++] = '\n'; vga_putchar('\n'); break; }
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            line[n++] = c;
            vga_putchar(c);
        }
        if (n >= max) break;
    }
    if (copy_to_user(ubuf, line, n) != 0) return -EFAULT;
    return (long)n;
}

static long sys_nanosleep(uint64_t ureq) {
    uint64_t ts[2];
    if (copy_from_user(ts, ureq, sizeof(ts)) != 0) return -EFAULT;
    uint64_t ticks = ts[0] * 100 + (ts[1] + 9999999) / 10000000;   /* 10 ms per tick, rounded up */
    sched_sleep_ticks(ticks);
    return 0;
}

#define PAGE_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define BRK_LIMIT  (256ULL << 20)

static long sys_brk(uint64_t addr) {
    task_t *t = sched_current();
    if (addr == 0 || addr < t->brk_start || addr > t->brk_start + BRK_LIMIT) return (long)t->brk;

    uint64_t old_end = PAGE_UP(t->brk), new_end = PAGE_UP(addr);
    if (new_end > old_end) {
        for (uint64_t p = old_end; p < new_end; p += PAGE_SIZE) {
            if (vmm_alloc_page(t->as, p, VM_WRITE) != 0) {          /* undo and report failure */
                for (uint64_t q = old_end; q < p; q += PAGE_SIZE) vmm_unmap_page(t->as, q, 1);
                return (long)t->brk;
            }
        }
    } else {
        for (uint64_t p = new_end; p < old_end; p += PAGE_SIZE) vmm_unmap_page(t->as, p, 1);
    }
    t->brk = addr;
    return (long)addr;
}

#define MAP_ANONYMOUS 0x20
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

static long sys_mmap(uint64_t len, int prot, int flags, long fd) {
    task_t *t = sched_current();
    if (len == 0 || !(flags & MAP_ANONYMOUS) || fd != -1) return -EINVAL;   /* files: Phase 2 */
    uint64_t size = PAGE_UP(len);
    if (size > t->mmap_next || t->mmap_next - size < PAGE_UP(t->brk) + (1ULL << 20)) return -ENOMEM;

    uint64_t base = t->mmap_next - size;
    int vf = ((prot & PROT_WRITE) ? VM_WRITE : 0) | ((prot & PROT_EXEC) ? VM_EXEC : 0);
    for (uint64_t p = base; p < base + size; p += PAGE_SIZE) {
        if (vmm_alloc_page(t->as, p, vf) != 0) {
            for (uint64_t q = base; q < p; q += PAGE_SIZE) vmm_unmap_page(t->as, q, 1);
            return -ENOMEM;
        }
    }
    t->mmap_next = base;
    return (long)base;
}

static long sys_munmap(uint64_t addr, uint64_t len) {
    task_t *t = sched_current();
    if ((addr & (PAGE_SIZE - 1)) || len == 0) return -EINVAL;
    for (uint64_t p = addr; p < addr + PAGE_UP(len); p += PAGE_SIZE) vmm_unmap_page(t->as, p, 1);
    return 0;
}

/* Copy a NULL-terminated array of string pointers from user space */
#define EXEC_MAX 16
#define EXEC_STR 128
static int copy_string_array(uint64_t uarr, char (*store)[EXEC_STR], char **out) {
    int n = 0;
    if (uarr == 0) return 0;
    for (;;) {
        uint64_t p;
        if (copy_from_user(&p, uarr + (uint64_t)n * 8, 8) != 0) return -EFAULT;
        if (p == 0) return n;
        if (n >= EXEC_MAX) return -7;                    /* E2BIG */
        if (strncpy_from_user(store[n], p, EXEC_STR) < 0) return -EFAULT;
        out[n] = store[n];
        n++;
    }
}

static long sys_execve(trap_frame_t *f, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    char path[EXEC_STR];
    if (strncpy_from_user(path, upath, sizeof(path)) < 0) return -EFAULT;

    char (*args)[EXEC_STR] = (char (*)[EXEC_STR])kmalloc(2 * EXEC_MAX * EXEC_STR);
    if (!args) return -ENOMEM;
    char *argp[EXEC_MAX], *envp[EXEC_MAX];
    int argc = copy_string_array(uargv, args, argp);
    int envc = argc < 0 ? argc : copy_string_array(uenvp, args + EXEC_MAX, envp);
    long r;
    if (argc < 0 || envc < 0) r = (argc < 0) ? argc : envc;
    else {
        if (argc == 0) { strncpy_safe(args[0], path, EXEC_STR); argp[0] = args[0]; argc = 1; }
        r = proc_exec(f, path, argc, argp, envc, envp);
    }
    kfree(args);
    return r;
}

static long sys_wait4(long pid, uint64_t ustatus, long options) {
    int status = 0;
    int r = task_wait((int)pid, &status, (int)(options & 1));
    if (r > 0 && ustatus && copy_to_user(ustatus, &status, sizeof(status)) != 0) return -EFAULT;
    return r;
}

void syscall_dispatch(trap_frame_t *f) {
    __asm__ volatile("sti");                 /* the entry stub ran with interrupts off */

    long ret;
    switch (f->rax) {
        case SYS_read:        ret = sys_read((long)f->rdi, f->rsi, f->rdx); break;
        case SYS_write:       ret = sys_write((long)f->rdi, f->rsi, f->rdx); break;
        case SYS_open:        ret = vfs_open(f->rdi, (int)f->rsi, (int)f->rdx); break;
        case SYS_close:       ret = vfs_close((int)f->rdi); break;
        case SYS_stat:        ret = vfs_stat(f->rdi, f->rsi, 1); break;
        case SYS_lstat:       ret = vfs_stat(f->rdi, f->rsi, 0); break;
        case SYS_fstat:       ret = vfs_fstat((int)f->rdi, f->rsi); break;
        case SYS_lseek:       ret = vfs_lseek((int)f->rdi, (int64_t)f->rsi, (int)f->rdx); break;
        case SYS_ftruncate:   ret = vfs_ftruncate((int)f->rdi, f->rsi); break;
        case SYS_getdents64:  ret = vfs_getdents64((int)f->rdi, f->rsi, f->rdx); break;
        case SYS_getcwd:      ret = vfs_getcwd(f->rdi, f->rsi); break;
        case SYS_chdir:       ret = vfs_chdir(f->rdi); break;
        case SYS_rename:      ret = vfs_rename(f->rdi, f->rsi); break;
        case SYS_mkdir:       ret = vfs_mkdir(f->rdi, (int)f->rsi); break;
        case SYS_rmdir:       ret = vfs_rmdir(f->rdi); break;
        case SYS_unlink:      ret = vfs_unlink(f->rdi); break;
        case SYS_readlink:    ret = vfs_readlink(f->rdi, f->rsi, f->rdx); break;
        case SYS_mmap:        ret = sys_mmap(f->rsi, (int)f->rdx, (int)f->r10, (long)f->r8); break;
        case SYS_munmap:      ret = sys_munmap(f->rdi, f->rsi); break;
        case SYS_brk:         ret = sys_brk(f->rdi); break;
        case SYS_sched_yield: sched_yield(); ret = 0; break;
        case SYS_nanosleep:   ret = sys_nanosleep(f->rdi); break;
        case SYS_getpid:      ret = sched_current()->pid; break;
        case SYS_getppid:     ret = sched_current()->ppid; break;
        case SYS_fork:        ret = proc_fork(f); break;
        case SYS_execve:      ret = sys_execve(f, f->rdi, f->rsi, f->rdx); break;
        case SYS_wait4:       ret = sys_wait4((long)f->rdi, f->rsi, (long)f->rdx); break;
        case SYS_exit:
        case SYS_exit_group:  sched_exit_current((int)((f->rdi & 0xFF) << 8)); /* does not return */
        default:              ret = -ENOSYS; break;
    }
    f->rax = (uint64_t)ret;
}
