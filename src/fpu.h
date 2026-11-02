#ifndef FPU_H
#define FPU_H

/* x87/SSE state. The kernel itself is built without SSE, but user programs
 * use it, so every user task keeps its own FXSAVE area. */
void fpu_init(void);                   /* enable SSE, snapshot the clean state */
void fpu_save(void *area);             /* area: 512 bytes, 16-byte aligned */
void fpu_restore(const void *area);
void fpu_reset_state(void *area);      /* fill with the clean state */

#endif
