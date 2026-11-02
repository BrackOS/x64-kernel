#ifndef EMBEDDED_H
#define EMBEDDED_H

#include <stdint.h>
#include <stddef.h>

/* User programs compiled into the kernel image (see user/ and the Makefile) */
int embedded_find(const char *name, const uint8_t **data, size_t *len);
void embedded_list(void);

#endif
