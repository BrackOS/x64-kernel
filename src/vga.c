#include "vga.h"
#include "io.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;

static size_t row;
static size_t col;
static uint8_t color;
static uint16_t* buffer;

static inline uint8_t vga_entry_color(uint8_t fg, uint8_t bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

/* Move the blinking hardware cursor to the current row/col */
static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* Make sure the hardware cursor is visible (underline shape, scanlines 14-15) */
static void vga_enable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (uint8_t)((inb(0x3D5) & 0xC0) | 14));
    outb(0x3D4, 0x0B);
    outb(0x3D5, (uint8_t)((inb(0x3D5) & 0xE0) | 15));
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    color = vga_entry_color(fg, bg);
}

void vga_clear(void) {
    buffer = VGA_MEMORY;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            buffer[index] = vga_entry(' ', color);
        }
    }
    row = 0;
    col = 0;
    vga_update_cursor();
}

void vga_init(void) {
    color = vga_entry_color(7, 0);
    vga_enable_cursor();
    vga_clear();
}

static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            buffer[(y - 1) * VGA_WIDTH + x] = buffer[y * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', color);
    }
    row = VGA_HEIGHT - 1;
}

static void vga_newline(void) {
    col = 0;
    if (++row == VGA_HEIGHT) {
        vga_scroll();
    }
}

/* Write one character without touching the hardware cursor (callers
 * update it once, so printing a string costs 4 port writes, not 4 per char) */
static void vga_put(char c) {
    if (c == '\n') {
        vga_newline();
        return;
    }
    if (c == '\r') {
        col = 0;
        return;
    }
    if (c == '\b') {
        /* Move the cursor back one cell (wrapping to the previous line).
         * It does not erase; "\b \b" is the usual erase sequence. */
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = VGA_WIDTH - 1;
        }
        return;
    }
    if (c == '\t') {
        do {
            vga_put(' ');
        } while (col % 8 != 0);
        return;
    }

    const size_t index = row * VGA_WIDTH + col;
    buffer[index] = vga_entry((unsigned char) c, color);
    if (++col == VGA_WIDTH) {
        vga_newline();
    }
}

void vga_putchar(char c) {
    vga_put(c);
    vga_update_cursor();
}

/* Step back one cell and blank it */
void vga_backspace(void) {
    if (col == 0 && row == 0) return;
    if (col == 0) {
        row--;
        col = VGA_WIDTH - 1;
    } else {
        col--;
    }
    buffer[row * VGA_WIDTH + col] = vga_entry(' ', color);
    vga_update_cursor();
}

void vga_print(const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        vga_put(str[i]);
    }
    vga_update_cursor();
}

/* Print a signed decimal number */
void vga_print_int(int num) {
    char buffer_[16];                /* "-2147483648" + NUL fits in 12 */
    int i = 0;
    unsigned int n = (num < 0) ? (0u - (unsigned int)num) : (unsigned int)num;

    do {
        buffer_[i++] = (char)('0' + (n % 10));
        n /= 10;
    } while (n > 0);
    if (num < 0) buffer_[i++] = '-';

    /* Digits were produced least-significant first; print them reversed */
    for (int j = i - 1; j >= 0; j--) {
        vga_put(buffer_[j]);
    }
    vga_update_cursor();
}

/* Print a value as 0x followed by hex digits (width follows the CPU word size) */
void vga_print_hex(uintptr_t v) {
    static const char digits[] = "0123456789ABCDEF";
    vga_put('0');
    vga_put('x');
    for (int i = (int)(sizeof(uintptr_t) * 2) - 1; i >= 0; i--) {
        vga_put(digits[(v >> (i * 4)) & 0xF]);
    }
    vga_update_cursor();
}
