#include "keyboard_tables.h"
#include "keyboard.h"
#include "timer.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>

#define KBD_STATUS_PORT 0x64
#define KBD_DATA_PORT   0x60

#define KBD_STATUS_OUTPUT_FULL 0x01   /* a byte is waiting in port 0x60 */
#define KBD_STATUS_AUX_DATA    0x20   /* ...and it came from the mouse, not the keyboard */

#define SC_ESC        0x01
#define SC_BACKSPACE  0x0E
#define SC_TAB        0x0F
#define SC_ENTER      0x1C
#define SC_CTRL       0x1D
#define SC_CTRL_REL   0x9D
#define SC_CAPSLOCK   0x3A

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int caps_lock = 0;
static int extended_key = 0;   /* set after an 0xE0 prefix byte */

/* Wait a little for the next key. If interrupts are on and the PIT is
 * ticking, sleep until the next interrupt (<= 10 ms) instead of burning
 * 100% CPU; otherwise fall back to a PAUSE spin. */
static void keyboard_idle(void) {
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    if ((flags & 0x200) && timer_get_ticks() != 0) {
        __asm__ volatile("hlt");
    } else {
        __asm__ volatile("pause");
    }
}

/* Discard anything already waiting in the controller */
void keyboard_init(void) {
    shift_pressed = 0;
    ctrl_pressed = 0;
    caps_lock = 0;
    extended_key = 0;
    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_FULL) {
        (void)inb(KBD_DATA_PORT);
    }
}

/* Non-blocking check: is a keyboard scancode waiting to be read?
 * (Mouse bytes sharing the same port are not reported.) */
int keyboard_has_input(void) {
    uint8_t st = inb(KBD_STATUS_PORT);
    return (st & KBD_STATUS_OUTPUT_FULL) && !(st & KBD_STATUS_AUX_DATA);
}

/* Block until a key press produces a character or one of the KEY_* codes.
 * Returns '\n' for Enter, '\b' for Backspace, '\t' for Tab, 27 for Esc. */
char keyboard_getchar(void) {
    for (;;) {
        uint8_t st = inb(KBD_STATUS_PORT);
        if (!(st & KBD_STATUS_OUTPUT_FULL)) {
            keyboard_idle();
            continue;
        }
        uint8_t sc = inb(KBD_DATA_PORT);
        if (st & KBD_STATUS_AUX_DATA) {
            continue;                         /* stray mouse byte */
        }

        /* 0xE0 prefix: arrows, keypad Enter, right Ctrl, ... */
        if (sc == 0xE0) {
            extended_key = 1;
            continue;
        }
        if (sc == 0xE1) {                     /* Pause/Break key: ignore */
            continue;
        }
        if (extended_key) {
            extended_key = 0;
            switch (sc) {
                case 0x48: return KEY_UP;
                case 0x50: return KEY_DOWN;
                case 0x4B: return KEY_LEFT;
                case 0x4D: return KEY_RIGHT;
                case 0x47: return KEY_HOME;
                case 0x4F: return KEY_END;
                case 0x53: return KEY_DELETE;
                case 0x1C: return '\n';       /* keypad Enter */
                case 0x35: return '/';        /* keypad / */
                case SC_CTRL:     ctrl_pressed = 1; continue;
                case SC_CTRL_REL: ctrl_pressed = 0; continue;
                default:   continue;          /* releases and other keys */
            }
        }

        /* Modifier keys */
        if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift_pressed = 1; continue; }
        if (sc == SC_LSHIFT_REL || sc == SC_RSHIFT_REL) { shift_pressed = 0; continue; }
        if (sc == SC_CTRL)     { ctrl_pressed = 1; continue; }
        if (sc == SC_CTRL_REL) { ctrl_pressed = 0; continue; }
        if (sc == SC_CAPSLOCK) { caps_lock = !caps_lock; continue; }

        if (sc & 0x80) continue;              /* key release */

        /* Keys the ASCII tables leave as 0 */
        switch (sc) {
            case SC_ESC:       return 27;
            case SC_BACKSPACE: return '\b';
            case SC_TAB:       return '\t';
            case SC_ENTER:     return '\n';
            default: break;
        }

        char c = shift_pressed ? scancode_ascii_shift[sc] : scancode_ascii[sc];
        if (c == 0) continue;

        /* Caps Lock flips the case of letters only (Shift+Caps gives lowercase) */
        if (caps_lock) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        }
        /* Ctrl+letter gives the control code (Ctrl-H = backspace, ...) */
        if (ctrl_pressed) {
            if (c >= 'a' && c <= 'z') c = (char)(c & 0x1F);
            else if (c >= 'A' && c <= 'Z') c = (char)(c & 0x1F);
        }
        return c;
    }
}
