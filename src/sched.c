#include "sched.h"
#include "gdt.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "fpu.h"
#include "irqflags.h"

#ifdef HOST_TEST
#define STI_INSN "nop"
#else
#define STI_INSN "sti"
#endif

extern char stack_top[];                    /* boot stack (boot.s) */

static task_t tasks[MAX_TASKS] __attribute__((aligned(16)));
static task_t *current_task;
static int next_pid = 2;

uint64_t kernel_rsp_scratch = 0;            /* read by syscall_entry */

/* ---- Low-level switching (GNU assembler, Intel syntax) ---- */

void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
void ret_to_user(void);
void kthread_start(void);
void kthread_exit_c(void);

__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    /* switch_context(save_rsp in rdi, new_rsp in rsi) */
    ".global switch_context\n"
    "switch_context:\n"
    "    push rbp\n"
    "    push rbx\n"
    "    push r12\n"
    "    push r13\n"
    "    push r14\n"
    "    push r15\n"
    "    mov [rdi], rsp\n"
    "    mov rsp, rsi\n"
    "    pop r15\n"
    "    pop r14\n"
    "    pop r13\n"
    "    pop r12\n"
    "    pop rbx\n"
    "    pop rbp\n"
    "    ret\n"
    /* First run of a kernel thread: rbx = function, r12 = argument */
    ".global kthread_start\n"
    "kthread_start:\n"
    "    " STI_INSN "\n"
    "    mov rdi, r12\n"
    "    call rbx\n"
    "    call kthread_exit_c\n"
    "1:  hlt\n"
    "    jmp 1b\n"
    /* First run of a user task: the stack holds a trap frame; return through it */
    ".global ret_to_user\n"
    "ret_to_user:\n"
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

/* ---- Task table ---- */

task_t *sched_current(void) { return current_task; }

task_t *task_by_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) return &tasks[i];
    }
    return NULL;
}

static int slot_of(const task_t *t) { return (int)(t - tasks); }

task_t *task_alloc(const char *name) {
    for (int i = 2; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) continue;
        task_t *t = &tasks[i];
        uint8_t *stack = (uint8_t *)kmalloc(KSTACK_SIZE);
        if (!stack) return NULL;
        memset(t, 0, sizeof(*t));
        t->pid = next_pid++;
        t->cwd = 1;                         /* BRKF root */
        t->state = TASK_BLOCKED;            /* reserved, not runnable until made ready */
        t->kstack = stack;
        t->kstack_top = ((uint64_t)(uintptr_t)stack + KSTACK_SIZE) & ~15ULL;
        strncpy_safe(t->name, name, sizeof(t->name));
        return t;
    }
    return NULL;
}

void task_free(task_t *t) {
    if (!t->kstack_static && t->kstack) kfree(t->kstack);
    memset(t, 0, sizeof(*t));               /* state becomes TASK_UNUSED */
}

void sched_make_ready(task_t *t) {
    t->slice_left = TIME_SLICE;
    t->state = TASK_READY;
}

task_t *task_create_kernel(const char *name, void (*fn)(void *), void *arg) {
    task_t *t = task_alloc(name);
    if (!t) return NULL;
    t->ppid = current_task ? current_task->pid : 0;

    uint64_t *sp = (uint64_t *)(uintptr_t)(t->kstack_top - 16);
    sp -= 7;                                 /* 6 saved registers + return address */
    sp[0] = 0;                               /* r15 */
    sp[1] = 0;                               /* r14 */
    sp[2] = 0;                               /* r13 */
    sp[3] = (uint64_t)(uintptr_t)arg;        /* r12 */
    sp[4] = (uint64_t)(uintptr_t)fn;         /* rbx */
    sp[5] = 0;                               /* rbp */
    sp[6] = (uint64_t)(uintptr_t)kthread_start;
    t->rsp = (uint64_t)(uintptr_t)sp;

    sched_make_ready(t);
    return t;
}

void task_prepare_user_stack(task_t *t, const trap_frame_t *frame) {
    trap_frame_t *tf = (trap_frame_t *)(uintptr_t)(t->kstack_top - sizeof(trap_frame_t));
    *tf = *frame;
    uint64_t *sp = (uint64_t *)tf;
    *--sp = (uint64_t)(uintptr_t)ret_to_user;
    for (int i = 0; i < 6; i++) *--sp = 0;   /* callee-saved registers */
    t->rsp = (uint64_t)(uintptr_t)sp;
}

/* ---- The scheduler ---- */

static task_t *pick_next(task_t *prev) {
    int start = slot_of(prev);
    for (int i = 1; i <= MAX_TASKS; i++) {
        int idx = (start + i) % MAX_TASKS;
        if (idx == 0) continue;              /* the idle task is only a fallback */
        if (tasks[idx].state == TASK_READY) return &tasks[idx];
    }
    if (prev->state == TASK_RUNNING) return prev;
    return &tasks[0];
}

void schedule(void) {
    uintptr_t flags = irq_save();
    task_t *prev = current_task;
    task_t *next = pick_next(prev);

    if (next != prev) {
        if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
        next->state = TASK_RUNNING;
        next->slice_left = TIME_SLICE;
        current_task = next;

        gdt_set_kernel_stack(next->kstack_top);   /* user -> kernel transitions land here */
        kernel_rsp_scratch = next->kstack_top;
        address_space_t *want = next->as ? next->as : vmm_current();   /* kernel threads run on any space */
        if (want != vmm_current()) vmm_switch(want);
        if (prev->as) fpu_save(prev->fpu);
        if (next->as) fpu_restore(next->fpu);

        switch_context(&prev->rsp, next->rsp);
    }
    irq_restore(flags);                       /* each task restores its own interrupt state */
}

void sched_yield(void) {
    schedule();
}

void sched_block(void) {
    uintptr_t flags = irq_save();
    current_task->state = TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

void sched_wake(task_t *t) {
    if (t && t->state == TASK_BLOCKED) {
        t->wake_tick = 0;
        sched_make_ready(t);
    }
}

void sched_sleep_ticks(uint64_t ticks) {
    uintptr_t flags = irq_save();
    current_task->wake_tick = system_timer.ticks + (ticks ? ticks : 1);
    current_task->state = TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

int sched_ready_count(void) {
    int n = 0;
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state == TASK_READY) n++;
    return n;
}

/* Timer interrupt (EOI already sent). Wakes sleepers and preempts user code. */
void sched_tick(trap_frame_t *frame) {
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->wake_tick != 0 && system_timer.ticks >= t->wake_tick) {
            t->wake_tick = 0;
            sched_make_ready(t);
        }
    }
    current_task->cpu_ticks++;

    /* Only user-mode code is preempted; kernel code runs until it blocks */
    if (frame && TRAP_FROM_USER(frame) && current_task->as) {
        if (current_task->slice_left > 0) current_task->slice_left--;
        if (current_task->slice_left == 0) {
            if (sched_ready_count() > 0) schedule();
            else current_task->slice_left = TIME_SLICE;
        }
    }
}

/* ---- Exit, wait, reap ---- */

void kthread_exit_c(void) {
    sched_exit_current(0);
}

void sched_exit_current(int exit_status) {
    uintptr_t flags = irq_save();
    (void)flags;
    task_t *t = current_task;

    if (t->as) {                              /* free the user memory (never while it is loaded) */
        address_space_t *dead = t->as;
        t->as = NULL;
        vmm_switch(vmm_kernel_space());
        vmm_destroy(dead);
    }
    t->exit_status = exit_status;
    t->state = TASK_ZOMBIE;

    for (int i = 1; i < MAX_TASKS; i++) {     /* orphans go to task 1 */
        if (tasks[i].state != TASK_UNUSED && tasks[i].ppid == t->pid) tasks[i].ppid = 1;
    }
    task_t *parent = task_by_pid(t->ppid);
    if (parent && parent->state == TASK_BLOCKED && parent->waiting_for != 0) {
        sched_make_ready(parent);
    }

    schedule();                               /* never returns: nothing makes a zombie ready */
    for (;;) __asm__ volatile("cli; hlt");
}

/* Stop a user process from outside (Ctrl-C, `kill`). Kernel threads cannot be killed. */
int sched_kill(int pid) {
    task_t *t = task_by_pid(pid);
    if (!t || !t->as || t->state == TASK_ZOMBIE) return -1;
    if (t == current_task) sched_exit_current(9);

    uintptr_t flags = irq_save();
    address_space_t *dead = t->as;
    t->as = NULL;
    if (vmm_current() == dead) vmm_switch(vmm_kernel_space());
    vmm_destroy(dead);
    t->exit_status = 9;                         /* "killed by signal 9" */
    t->state = TASK_ZOMBIE;
    t->wake_tick = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].ppid == t->pid) tasks[i].ppid = 1;
    }
    task_t *parent = task_by_pid(t->ppid);
    if (parent && parent->state == TASK_BLOCKED && parent->waiting_for != 0) sched_make_ready(parent);
    irq_restore(flags);
    return 0;
}

int task_wait(int pid, int *status, int nohang) {
    task_t *me = current_task;
    for (;;) {
        uintptr_t flags = irq_save();
        int have_child = 0;
        for (int i = 1; i < MAX_TASKS; i++) {
            task_t *c = &tasks[i];
            if (c->state == TASK_UNUSED || c->ppid != me->pid || c == me) continue;
            if (pid > 0 && c->pid != pid) continue;
            have_child = 1;
            if (c->state == TASK_ZOMBIE) {
                int cpid = c->pid;
                if (status) *status = c->exit_status;
                task_free(c);
                irq_restore(flags);
                return cpid;
            }
        }
        if (!have_child) { irq_restore(flags); return -10; }     /* -ECHILD */
        if (nohang)      { irq_restore(flags); return 0; }

        me->waiting_for = (pid > 0) ? pid : -1;
        me->state = TASK_BLOCKED;
        schedule();                           /* the exiting child wakes us */
        me->waiting_for = 0;
        irq_restore(flags);
    }
}

void task_reap_zombies(void) {
    uintptr_t flags = irq_save();
    for (int i = 2; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE && tasks[i].ppid == current_task->pid) task_free(&tasks[i]);
    }
    irq_restore(flags);
}

/* ---- Setup and listing ---- */

static void idle_main(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile(STI_INSN "; hlt");   /* sleep until an interrupt ... */
        schedule();                           /* ... then let anything that became ready run */
    }
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));

    /* Task 0: idle. A kernel thread that only runs when nothing else can. */
    task_t *idle = &tasks[0];
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK_SIZE);
    idle->pid = 0;
    idle->state = TASK_READY;
    idle->kstack = stack;
    idle->kstack_top = ((uint64_t)(uintptr_t)stack + KSTACK_SIZE) & ~15ULL;
    strncpy_safe(idle->name, "idle", sizeof(idle->name));
    {
        uint64_t *sp = (uint64_t *)(uintptr_t)(idle->kstack_top - 16);
        sp -= 7;
        sp[3] = 0;
        sp[4] = (uint64_t)(uintptr_t)idle_main;
        sp[6] = (uint64_t)(uintptr_t)kthread_start;
        idle->rsp = (uint64_t)(uintptr_t)sp;
    }

    /* Task 1: whatever is running right now (the shell), on the boot stack */
    task_t *boot = &tasks[1];
    boot->pid = 1;
    boot->ppid = 0;
    boot->state = TASK_RUNNING;
    boot->kstack_static = 1;
    boot->kstack_top = (uint64_t)(uintptr_t)stack_top;
    boot->slice_left = TIME_SLICE;
    strncpy_safe(boot->name, "kshell", sizeof(boot->name));
    current_task = boot;

    gdt_set_kernel_stack(boot->kstack_top);
    kernel_rsp_scratch = boot->kstack_top;
}

static const char *state_name(task_state_t s) {
    switch (s) {
        case TASK_READY:   return "ready";
        case TASK_RUNNING: return "running";
        case TASK_BLOCKED: return "blocked";
        case TASK_ZOMBIE:  return "zombie";
        default:           return "?";
    }
}

void task_print_list(void) {
    vga_print("PID  PPID STATE    KIND    TICKS  NAME\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_UNUSED) continue;
        vga_print_int(t->pid);
        vga_print(t->pid < 10 ? "    " : (t->pid < 100 ? "   " : "  "));
        vga_print_int(t->ppid);
        vga_print(t->ppid < 10 ? "    " : (t->ppid < 100 ? "   " : "  "));
        const char *st = state_name(t->state);
        vga_print(st);
        for (size_t k = strlen(st); k < 9; k++) vga_print(" ");
        vga_print(t->as ? "user    " : "kernel  ");
        vga_print_int((int)t->cpu_ticks);
        vga_print("      ");
        vga_print(t->name);
        vga_print("\n");
    }
}
