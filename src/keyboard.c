#include "keyboard_tables.h"
#include "keyboard.h"
#include "input.h"
#include "mouse.h"
#include "idt.h"
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

/* Translate one scancode into a character or KEY_* code. Returns 1 if the
 * scancode completed a key press, 0 if it was a modifier, a key release, a
 * prefix byte or a key we ignore. */
static int kbd_decode(uint8_t sc, char *out) {
    /* 0xE0 prefix: arrows, keypad Enter, right Ctrl, ... */
    if (sc == 0xE0) {
        extended_key = 1;
           return 0;
    }
    if (sc == 0xE1) {                     /* Pause/Break key: ignore */
        return 0;
    }
    if (extended_key) {
        extended_key = 0;
        switch (sc) {
            case 0x48: *out = KEY_UP; return 1;
            case 0x50: *out = KEY_DOWN; return 1;
            case 0x4B: *out = KEY_LEFT; return 1;
            case 0x4D: *out = KEY_RIGHT; return 1;
            case 0x47: *out = KEY_HOME; return 1;
            case 0x4F: *out = KEY_END; return 1;
            case 0x53: *out = KEY_DELETE; return 1;
            case 0x1C: *out = '\n'; return 1;       /* keypad Enter */
            case 0x35: *out = '/'; return 1;        /* keypad / */
            case SC_CTRL: ctrl_pressed = 1; return 0;
            case SC_CTRL_REL: ctrl_pressed = 0; return 0;
            default:   return 0;          /* releases and other keys */
        }
    }

    /* Modifier keys */
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift_pressed = 1; return 0; }
    if (sc == SC_LSHIFT_REL || sc == SC_RSHIFT_REL) { shift_pressed = 0; return 0; }
    if (sc == SC_CTRL)     { ctrl_pressed = 1; return 0; }
    if (sc == SC_CTRL_REL) { ctrl_pressed = 0; return 0; }
    if (sc == SC_CAPSLOCK) { caps_lock = !caps_lock; return 0; }

    if (sc & 0x80) return 0;              /* key release */

    /* Keys the ASCII tables leave as 0 */
    switch (sc) {
        case SC_ESC: *out = 27; return 1;
        case SC_BACKSPACE: *out = '\b'; return 1;
        case SC_TAB: *out = '\t'; return 1;
        case SC_ENTER: *out = '\n'; return 1;
        default: break;
    }

    char c = shift_pressed ? scancode_ascii_shift[sc] : scancode_ascii[sc];
    if (c == 0) return 0;

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
    *out = c;
    return 1;
}

static int kbd_irq_on = 0;              /* 1 once IRQ1 is unmasked */

/* Read everything the controller has for us and queue the key presses */
static void kbd_drain(void) {
    for (;;) {
        uint8_t st = inb(KBD_STATUS_PORT);
        if (!(st & KBD_STATUS_OUTPUT_FULL)) break;
        if (st & KBD_STATUS_AUX_DATA) {
            if (mouse_irq_active()) break;        /* the mouse interrupt will take it */
            (void)inb(KBD_DATA_PORT);             /* stray mouse byte: discard */
            continue;
        }
        char c;
        if (kbd_decode(inb(KBD_DATA_PORT), &c)) input_push_key(c);
    }
}

/* IRQ1 */
void keyboard_irq_handler(void) {
    kbd_drain();
}

/* For code that runs before the keyboard interrupt is on (or with interrupts
 * off): check the controller by hand. Does nothing once IRQ1 is enabled. */
void keyboard_poll(void) {
    if (!kbd_irq_on) kbd_drain();
}

void keyboard_enable_irq(void) {
    kbd_irq_on = 1;
    pic_enable_irq(1);
}

int keyboard_irq_active(void) {
    return kbd_irq_on;
}

/* Non-blocking check: is a key press waiting? */
int keyboard_has_input(void) {
    return input_key_pending();
}

/* Block until a key press produces a character or one of the KEY_* codes.
 * Returns '\n' for Enter, '\b' for Backspace, '\t' for Tab, 27 for Esc.
 * Mouse events that arrive meanwhile are skipped. */
char keyboard_getchar(void) {
    input_event_t ev;
    for (;;) {
        input_wait(&ev);
        if (ev.type == INPUT_KEY) return ev.key;
    }
}
