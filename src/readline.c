#include "readline.h"
#include "keyboard.h"
#include "vga.h"
#include "heap.h"
#include "string.h"

/* ---- History -------------------------------------------------------- */

static char *history[HISTORY_MAX];
static int hist_count = 0;

int history_count(void) {
    return hist_count;
}

const char *history_get(int index) {
    if (index < 0 || index >= hist_count) return NULL;
    return history[index];
}

void history_clear(void) {
    for (int i = 0; i < hist_count; i++) {
        kfree(history[i]);
        history[i] = NULL;
    }
    hist_count = 0;
}

static void history_add(const char *line) {
    if (line[0] == '\0') return;
    /* Skip an immediate repeat of the previous command */
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) return;

    size_t n = strlen(line) + 1;
    char *copy = (char *)kmalloc(n);
    if (copy == NULL) return;            /* out of memory: just don't remember it */
    memcpy(copy, line, n);

    if (hist_count == HISTORY_MAX) {     /* drop the oldest */
        kfree(history[0]);
        for (int i = 1; i < HISTORY_MAX; i++) history[i - 1] = history[i];
        hist_count--;
    }
    history[hist_count++] = copy;
}

/* ---- Screen helpers --------------------------------------------------
 * The editor keeps the screen cursor at line position `pos`. These helpers
 * redraw part of the line and put the cursor back where it belongs. They
 * rely on '\b' wrapping to the previous row, so lines longer than one
 * screen row work. */

static void put_range(const char *b, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) vga_putchar(b[i]);
}

static void put_spaces(size_t n) {
    while (n--) vga_putchar(' ');
}

static void back(size_t n) {
    while (n--) vga_putchar('\b');
}

/* ---- The editor ---------------------------------------------------- */

void readline(char *buf, size_t size) {
    if (size < 2) {
        if (size == 1) buf[0] = '\0';
        return;
    }
    if (size > READLINE_MAX) size = READLINE_MAX;

    static char saved[READLINE_MAX];     /* line being typed while browsing history */
    size_t len = 0, pos = 0;
    int nav = hist_count;                /* == hist_count: the line being typed */
    buf[0] = '\0';

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            put_range(buf, pos, len);    /* finish drawing, then newline */
            vga_putchar('\n');
            buf[len] = '\0';
            history_add(buf);
            return;
        }
        else if (c == 3) {               /* Ctrl-C: abandon the line */
            put_range(buf, pos, len);
            vga_print("^C\n");
            buf[0] = '\0';
            return;
        }
        else if (c == '\b' || c == 127) {          /* Backspace */
            if (pos > 0) {
                for (size_t i = pos; i < len; i++) buf[i - 1] = buf[i];
                len--; pos--;
                buf[len] = '\0';
                vga_putchar('\b');
                put_range(buf, pos, len);
                vga_putchar(' ');
                back(len - pos + 1);
            }
        }
        else if (c == KEY_DELETE) {                /* Delete */
            if (pos < len) {
                for (size_t i = pos; i + 1 < len; i++) buf[i] = buf[i + 1];
                len--;
                buf[len] = '\0';
                put_range(buf, pos, len);
                vga_putchar(' ');
                back(len - pos + 1);
            }
        }
        else if (c == KEY_LEFT) {
            if (pos > 0) { back(1); pos--; }
        }
        else if (c == KEY_RIGHT) {
            if (pos < len) { vga_putchar(buf[pos]); pos++; }
        }
        else if (c == KEY_HOME || c == 1) {        /* Home / Ctrl-A */
            back(pos); pos = 0;
        }
        else if (c == KEY_END || c == 5) {         /* End / Ctrl-E */
            put_range(buf, pos, len); pos = len;
        }
        else if (c == 11) {                        /* Ctrl-K: kill to end of line */
            put_spaces(len - pos);
            back(len - pos);
            len = pos;
            buf[len] = '\0';
        }
        else if (c == KEY_UP || c == KEY_DOWN || c == 21) {
            const char *text = NULL;
            if (c == 21) {                         /* Ctrl-U: clear the whole line */
                text = "";
            } else if (c == KEY_UP && nav > 0) {
                if (nav == hist_count) {           /* remember what was being typed */
                    memcpy(saved, buf, len + 1);
                }
                nav--;
                text = history[nav];
            } else if (c == KEY_DOWN && nav < hist_count) {
                nav++;
                text = (nav == hist_count) ? saved : history[nav];
            }
            if (text != NULL) {
                /* Replace the whole line with `text` */
                size_t old_len = len;
                size_t new_len = strlen(text);
                if (new_len > size - 1) new_len = size - 1;
                back(pos);
                memcpy(buf, text, new_len);
                buf[new_len] = '\0';
                put_range(buf, 0, new_len);
                if (old_len > new_len) {
                    put_spaces(old_len - new_len);
                    back(old_len - new_len);
                }
                len = pos = new_len;
            }
        }
        else if (c >= 32 && c < 127) {             /* Printable: insert at cursor */
            if (len < size - 1) {
                for (size_t i = len; i > pos; i--) buf[i] = buf[i - 1];
                buf[pos] = c;
                len++;
                buf[len] = '\0';
                put_range(buf, pos, len);
                back(len - (pos + 1));
                pos++;
            }
        }
        /* anything else (other control codes) is ignored */
    }
}
