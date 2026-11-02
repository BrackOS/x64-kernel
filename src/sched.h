#ifndef SCHED_H
#define SCHED_H

#include "task.h"
#include "trap.h"

/* Round-robin scheduler. The kernel itself is not preemptible: tasks switch
 * only when they block, yield, exit, or when the timer interrupts *user*
 * code. That keeps the heap and the page allocator safe without locks. */

void    sched_init(void);                 /* the boot context becomes task 1 "kshell" */
task_t *sched_current(void);
task_t *task_by_pid(int pid);

task_t *task_create_kernel(const char *name, void (*fn)(void *), void *arg);
task_t *task_alloc(const char *name);     /* blank task with a kernel stack, state UNUSED->READY is the caller's job */
void    task_prepare_user_stack(task_t *t, const trap_frame_t *frame);   /* first run: iretq into user mode */
void    task_free(task_t *t);

void    schedule(void);                   /* pick the next task and switch to it */
void    sched_yield(void);
void    sched_block(void);                /* current task sleeps until sched_wake() */
void    sched_wake(task_t *t);
void    sched_sleep_ticks(uint64_t ticks);
void    sched_tick(trap_frame_t *frame);  /* called from the timer interrupt */
void    sched_exit_current(int exit_status) __attribute__((noreturn));
int     sched_ready_count(void);
void    sched_make_ready(task_t *t);

/* wait4 semantics: >0 child pid, 0 nothing yet (nohang), -10 no such child */
int     sched_kill(int pid);              /* 0 on success */
int     task_wait(int pid, int *status, int nohang);
void    task_reap_zombies(void);          /* collect exited orphans (kshell calls this) */
void    task_print_list(void);            /* the `ps` command */

/* Used by the syscall entry code */
extern uint64_t kernel_rsp_scratch;

#endif
