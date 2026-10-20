#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Special keys returned by keyboard_getchar() (control-character range) */
#define KEY_UP     0x11
#define KEY_DOWN   0x12
#define KEY_LEFT   0x13
#define KEY_RIGHT  0x14

void keyboard_init(void);
int keyboard_has_input(void);
char keyboard_getchar(void);

#endif
