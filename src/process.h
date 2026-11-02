#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "trap.h"

#define MAX_ARGS 32

/* Start a new process from a program name (embedded program or a file in the
 * current FAT32 directory). The caller becomes the parent. Returns the pid, or
 * a negative error (the reason is printed to the console if `verbose`). */
int proc_spawn(const char *path, int argc, char *const argv[], int verbose);

/* The system-call halves of fork and execve. `frame` is the caller's saved
 * registers. fork returns the child's pid; execve does not return on success
 * (the frame is rewritten so the syscall "returns" into the new program). */
int proc_fork(const trap_frame_t *frame);
int proc_exec(trap_frame_t *frame, const char *path, int argc, char *const argv[], int envc, char *const envp[]);

/* A user process did something illegal (bad memory access, illegal instruction...) */
void proc_user_fault(const trap_frame_t *frame, uint64_t vector, uint64_t error) __attribute__((noreturn));

/* Find a program image: embedded first, then the FAT32 current directory.
 * *needs_free is set if the buffer came from kmalloc. */
int proc_load_image(const char *path, const uint8_t **data, size_t *len, int *needs_free);

#endif
