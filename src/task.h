#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "vmm.h"

#define MAX_TASKS     64
#define KSTACK_SIZE   16384
#define TIME_SLICE    5          /* timer ticks (10 ms each) before a user task is preempted */

#define TASK_MAX_FDS 16

/* An open file (file descriptors 0-2 are the console and have no entry) */
typedef struct {
    int      used;
    uint32_t ino;
    uint32_t flags;          /* O_* flags it was opened with */
    uint64_t pos;            /* file offset, or directory read position */
} task_file_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,                  /* can run, waiting for the CPU */
    TASK_RUNNING,                /* is running now */
    TASK_BLOCKED,                /* waiting for a child, a timer or a wake-up */
    TASK_ZOMBIE,                 /* exited; waiting for its parent to collect the status */
} task_state_t;

/* A task is either a kernel thread (as == NULL) or a user process. */
typedef struct task {
    int      pid, ppid;
    char     name[24];
    task_state_t state;

    uint64_t rsp;                /* saved kernel stack pointer while not running */
    uint8_t *kstack;             /* kernel stack (heap block) */
    int      kstack_static;      /* 1 if it is not ours to free (the boot stack) */
    uint64_t kstack_top;

    address_space_t *as;         /* user address space, NULL for kernel threads */
    task_file_t files[TASK_MAX_FDS];   /* open files */
    uint32_t cwd;                /* current directory (BRKF inode, 1 = root) */
    uint64_t brk_start, brk;     /* program break (heap) */
    uint64_t mmap_next;          /* next anonymous mapping goes below this */

    int      exit_status;        /* wait4-style: (code << 8), or the signal number */
    int      waiting_for;        /* wait4 target: -1 any child, >0 a pid, 0 not waiting */
    uint64_t wake_tick;          /* sleeping until this timer tick (0 = not sleeping) */
    uint32_t slice_left;
    uint64_t cpu_ticks;          /* how long it has run */

    uint8_t  fpu[512] __attribute__((aligned(16)));   /* FXSAVE area (user tasks) */
} __attribute__((aligned(16))) task_t;

#endif
