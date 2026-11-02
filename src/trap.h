#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

/* Everything the interrupt, exception and syscall entry code saves on the
 * kernel stack. The order matches the push/pop order in idt_stubs.s and in
 * syscall_entry (syscall.c), so a pointer to the stack is a pointer to this. */
typedef struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;        /* interrupt number, or 0x80 for a syscall */
    uint64_t error;         /* error code (0 if the CPU pushes none) */
    uint64_t rip, cs, rflags, rsp, ss;    /* pushed by the CPU */
} trap_frame_t;

/* idt_stubs.s, syscall_entry and ret_to_user all rely on this exact layout */
_Static_assert(sizeof(trap_frame_t) == 176, "trap_frame_t size");
_Static_assert(__builtin_offsetof(trap_frame_t, rax) == 112, "rax offset");
_Static_assert(__builtin_offsetof(trap_frame_t, vector) == 120, "vector offset");
_Static_assert(__builtin_offsetof(trap_frame_t, error) == 128, "error offset");
_Static_assert(__builtin_offsetof(trap_frame_t, rip) == 136, "rip offset");
_Static_assert(__builtin_offsetof(trap_frame_t, ss) == 168, "ss offset");

#define TRAP_FROM_USER(f)  (((f)->cs & 3) == 3)

#endif
