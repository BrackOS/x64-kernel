#ifndef SERIAL_H
#define SERIAL_H

/* COM1 (0x3F8) debug output, 115200 8N1. Everything printed through the VGA
 * driver is mirrored here, so `make run` shows it in the host terminal. */
void serial_init(void);
void serial_putc(char c);
void serial_print(const char *s);

#endif
