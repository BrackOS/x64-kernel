#ifndef READLINE_H
#define READLINE_H

#include <stddef.h>

/* Longest line the editor can handle (including the NUL) */
#define READLINE_MAX 256
/* Number of commands remembered */
#define HISTORY_MAX  16

/* Read one line from the keyboard into buf (at most size-1 characters).
 * Supports: Left/Right, Home/End (Ctrl-A/Ctrl-E), Backspace, Delete,
 * Up/Down command history, Ctrl-K (kill to end), Ctrl-U (clear line),
 * Ctrl-C (cancel). The line is echoed to the screen starting at the
 * current cursor position and the cursor ends on the next line.
 * Non-empty lines are added to the history. */
void readline(char *buf, size_t size);

/* History access: index 0 is the oldest entry */
int history_count(void);
const char *history_get(int index);
void history_clear(void);

#endif
