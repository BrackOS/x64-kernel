#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int serial_ready = 0;

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);   /* enable and clear FIFOs */
    outb(COM1 + 4, 0x1E);   /* loopback self-test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) {
        serial_ready = 0;   /* no UART present */
        return;
    }
    outb(COM1 + 4, 0x0F);   /* normal operation */
    serial_ready = 1;
}

void serial_putc(char c) {
    if (!serial_ready) return;
    if (c == '\n') serial_putc('\r');
    for (int t = 100000; t > 0; t--) {
        if (inb(COM1 + 5) & 0x20) break;   /* transmit buffer empty */
    }
    outb(COM1, (uint8_t)c);
}

void serial_print(const char *s) {
    while (*s) serial_putc(*s++);
}
