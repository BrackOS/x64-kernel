#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* One queue of input events, filled from the keyboard and mouse interrupt
 * handlers and read by the shell (keys only) or a GUI loop (everything). */

typedef enum {
    INPUT_NONE = 0,
    INPUT_KEY,              /* a key press: .key holds the character or a KEY_* code */
    INPUT_MOUSE_MOVE,       /* the pointer moved: .x/.y position, .dx/.dy movement */
    INPUT_MOUSE_BUTTON,     /* a button changed: .buttons now held, .changed = which ones */
} input_type_t;

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

typedef struct {
    input_type_t type;
    char    key;
    uint8_t buttons;
    uint8_t changed;
    int     x, y;
    int     dx, dy;
} input_event_t;

void input_init(void);
int  input_poll(input_event_t *ev);       /* next event, or 0 if none (never blocks) */
void input_wait(input_event_t *ev);       /* sleep until an event arrives */
int  input_key_pending(void);
int  input_take_interrupt(void);          /* Ctrl-C pressed? (clears the flag) */             /* is a key press waiting in the queue? */

/* Called by the drivers */
void input_push_key(char c);
void input_push_mouse(int x, int y, int dx, int dy, uint8_t buttons);

#endif
