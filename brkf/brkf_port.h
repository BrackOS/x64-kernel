#ifndef BRKF_PORT_H
#define BRKF_PORT_H

/* The BRKF core is plain C with no operating-system dependencies. This header
 * is the only place that knows whether it is built into the BrackOS kernel
 * (-DBRKF_KERNEL) or into a normal program on Linux/Windows/macOS. */
#include <stdint.h>
#include <stddef.h>

#ifdef BRKF_KERNEL
#include "../src/string.h"
#else
#include <string.h>
#endif

#endif
