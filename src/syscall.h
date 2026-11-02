#ifndef SYSCALL_H
#define SYSCALL_H

#include "trap.h"

/* Turn on the SYSCALL instruction. User code calls it with the Linux x86-64
 * convention: number in rax, arguments in rdi, rsi, rdx, r10, r8, r9, result
 * in rax (negative errno on failure). rcx and r11 are clobbered. */
void syscall_init(void);

void syscall_dispatch(trap_frame_t *frame);   /* called from the entry stub */

#endif
