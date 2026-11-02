#include "mouse.h"
#include "io.h"
#include "fb.h"
#include "input.h"
#include "idt.h"
#include "serial.h"
#include <stdint.h>

/* PS/2 controller ports and status bits */
#define PS2_DATA        0x60
#define PS2_STATUS_CMD  0x64
#define PS2_OUT_FULL    0x01     /* a byte is waiting in the data port */
#define PS2_IN_FULL     0x02     /* controller is not ready for another byte */
#define PS2_AUX_DATA    0x20     /* the waiting byte came from the mouse */

#define MOUSE_ACK       0xFA

mouse_state_t mouse_state = {0};

static int mouse_ready  = 0;     /* 1 once the mouse is set up */
static int mouse_irq_on = 0;     /* 1 once IRQ12 is enabled */
static uint8_t packet[3];
static int packet_idx = 0;

static void ps2_mouse_init(void);
static void ps2_mouse_update(void);
static mouse_state_t *ps2_mouse_get_state(void);
static const char *ps2_mouse_get_name(void);

mouse_driver_t ps2_mouse_driver = {
    .init = ps2_mouse_init,
    .update = ps2_mouse_update,
    .get_state = ps2_mouse_get_state,
    .get_name = ps2_mouse_get_name,
};

mouse_driver_t *active_mouse_driver = &ps2_mouse_driver;

/* ---- Talking to the controller ---- */

static int wait_write(void) {
    for (int t = 100000; t > 0; t--) {
        if (!(inb(PS2_STATUS_CMD) & PS2_IN_FULL)) return 0;
    }
    return -1;
}

static int wait_read(void) {
    for (int t = 100000; t > 0; t--) {
        if (inb(PS2_STATUS_CMD) & PS2_OUT_FULL) return 0;
    }
    return -1;
}

/* Send a command byte to the mouse itself (not the keyboard). 0 if it ACKs. */
static int mouse_send(uint8_t byte) {
    if (wait_write()) return -1;
    outb(PS2_STATUS_CMD, 0xD4);          /* "next byte goes to the mouse" */
    if (wait_write()) return -1;
    outb(PS2_DATA, byte);
    if (wait_read()) return -1;
    return (inb(PS2_DATA) == MOUSE_ACK) ? 0 : -1;
}

static void ps2_mouse_init(void) {
    if (mouse_ready) return;

    while (inb(PS2_STATUS_CMD) & PS2_OUT_FULL) (void)inb(PS2_DATA);   /* flush */

    if (wait_write()) return;
    outb(PS2_STATUS_CMD, 0xA8);          /* enable the mouse port */

    if (wait_write()) return;
    outb(PS2_STATUS_CMD, 0x20);          /* read the controller configuration */
    if (wait_read()) return;
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                         /* mouse interrupt on */
    cfg &= (uint8_t)~0x20;               /* mouse clock on */
    if (wait_write()) return;
    outb(PS2_STATUS_CMD, 0x60);          /* write the configuration back */
    if (wait_write()) return;
    outb(PS2_DATA, cfg);

    if (mouse_send(0xF6) != 0 || mouse_send(0xF4) != 0) {    /* defaults, then start reporting */
        serial_print("MOUSE: no PS/2 mouse answered\n");
        return;
    }

    mouse_state.x = fb.available ? (int)fb.width / 2 : 512;
    mouse_state.y = fb.available ? (int)fb.height / 2 : 384;
    packet_idx = 0;
    mouse_ready = 1;
    mouse_irq_on = 1;
    pic_enable_irq(12);
}

/* ---- Packets ---- */

static void handle_byte(uint8_t b) {
    if (packet_idx == 0 && !(b & 0x08)) return;     /* not a first byte: out of sync, skip */
    packet[packet_idx++] = b;
    if (packet_idx < 3) return;
    packet_idx = 0;

    if (packet[0] & 0xC0) return;                   /* overflow: discard this packet */

    /* 9-bit signed movement: the sign bits live in the first byte */
    int dx = (int)packet[1] - (int)((packet[0] << 4) & 0x100);
    int dy = (int)packet[2] - (int)((packet[0] << 3) & 0x100);
    dy = -dy;                                       /* PS/2 counts up as positive; the screen counts down */

    int w = fb.available ? (int)fb.width  : 1024;
    int h = fb.available ? (int)fb.height : 768;
    int x = mouse_state.x + dx;
    int y = mouse_state.y + dy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > w - 1) x = w - 1;
    if (y > h - 1) y = h - 1;

    mouse_state.dx = dx;
    mouse_state.dy = dy;
    mouse_state.x = x;
    mouse_state.y = y;
    mouse_state.buttons = packet[0] & 0x07;
    mouse_state.left_pressed   = (packet[0] & 0x01) != 0;
    mouse_state.right_pressed  = (packet[0] & 0x02) != 0;
    mouse_state.middle_pressed = (packet[0] & 0x04) != 0;

    input_push_mouse(x, y, dx, dy, mouse_state.buttons);
}

static void drain(void) {
    for (;;) {
        uint8_t st = inb(PS2_STATUS_CMD);
        if (!(st & PS2_OUT_FULL) || !(st & PS2_AUX_DATA)) break;
        handle_byte(inb(PS2_DATA));
    }
}

/* IRQ12 */
void mouse_irq_handler(void) {
    drain();
}

/* Polling fallback: only used while the interrupt is not on */
static void ps2_mouse_update(void) {
    if (mouse_ready && !mouse_irq_on) drain();
}

static mouse_state_t *ps2_mouse_get_state(void) { return &mouse_state; }
static const char *ps2_mouse_get_name(void) { return "PS/2 Mouse"; }

int mouse_irq_active(void) {
    return mouse_irq_on;
}

/* ---- Universal interface ---- */

void mouse_init(void) {
    if (active_mouse_driver && active_mouse_driver->init) active_mouse_driver->init();
}

void mouse_update(void) {
    if (active_mouse_driver && active_mouse_driver->update) active_mouse_driver->update();
}

mouse_state_t *mouse_get(void) {
    return active_mouse_driver ? active_mouse_driver->get_state() : &mouse_state;
}

const char *mouse_get_driver_name(void) {
    return active_mouse_driver ? active_mouse_driver->get_name() : "none";
}
