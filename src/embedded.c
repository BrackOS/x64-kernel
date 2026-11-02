#include "embedded.h"
#include "string.h"
#include "vga.h"

/* Each program is pulled in with .incbin; the path is relative to the build
 * directory (the project root). `make` builds the user programs first. */
#define EMBED(sym, path)                                    \
    __asm__(".section .rodata\n"                            \
            ".balign 16\n"                                  \
            ".global " #sym "_start\n"                      \
            #sym "_start:\n"                                \
            ".incbin \"" path "\"\n"                        \
            ".global " #sym "_end\n"                        \
            #sym "_end:\n"                                  \
            ".byte 0\n"                                     \
            ".text\n");                                     \
    extern const uint8_t sym##_start[], sym##_end[];

EMBED(prog_hello,   "user/hello.elf")
EMBED(prog_forktest,"user/forktest.elf")
EMBED(prog_spin,    "user/spin.elf")
EMBED(prog_crash,   "user/crash.elf")
EMBED(prog_memtest, "user/memtest.elf")
EMBED(prog_echo,    "user/echo.elf")
EMBED(prog_fstest,  "user/fstest.elf")

typedef struct { const char *name; const uint8_t *start, *end; } embedded_t;

static const embedded_t programs[] = {
    { "hello",    prog_hello_start,    prog_hello_end },
    { "forktest", prog_forktest_start, prog_forktest_end },
    { "spin",     prog_spin_start,     prog_spin_end },
    { "crash",    prog_crash_start,    prog_crash_end },
    { "memtest",  prog_memtest_start,  prog_memtest_end },
    { "echo",     prog_echo_start,     prog_echo_end },
    { "fstest",   prog_fstest_start,   prog_fstest_end },
};

int embedded_find(const char *name, const uint8_t **data, size_t *len) {
    /* accept "hello", "/bin/hello" and "hello.elf" */
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '/') base = p + 1;
    char clean[24];
    strncpy_safe(clean, base, sizeof(clean));
    size_t n = strlen(clean);
    if (n > 4 && strcmp(clean + n - 4, ".elf") == 0) clean[n - 4] = '\0';

    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        if (strcmp(clean, programs[i].name) == 0) {
            *data = programs[i].start;
            *len = (size_t)(programs[i].end - programs[i].start);
            return 0;
        }
    }
    return -1;
}

void embedded_list(void) {
    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        vga_print("  ");
        vga_print(programs[i].name);
        vga_print("\n");
    }
}
