#include "vga.h"
#include "serial.h"
#include "io.h"
#include "fb.h"
#include "font.h"

/* Console size in character cells: 80x25 in VGA text mode, or as many 8x16
 * cells as the framebuffer holds when GRUB gave us a graphics mode. */
static size_t con_w = 80;
static size_t con_h = 25;
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;

static size_t row;
static size_t col;
static uint8_t color;
static uint16_t* buffer;

/* ---- Graphics console (used when GRUB set up a framebuffer) ----
 * Each cell is an 8x8 font glyph stretched to 8x16 pixels. */
#define CELL_W 8
#define CELL_H 16
#define GFX_CURSOR_MASK 0x00FFFFFFu

static int gfx = 0;
static uint32_t gfx_fg, gfx_bg;
static uint32_t gfx_palette[16];
static int gfx_cursor_on = 0;
static size_t gfx_cursor_row, gfx_cursor_col;

/* The cursor is an inverted underline, drawn with XOR so it can be removed exactly */
static void gfx_cursor_hide(void) {
    if (gfx_cursor_on) {
        fb_xor_rect((int)(gfx_cursor_col * CELL_W), (int)(gfx_cursor_row * CELL_H) + CELL_H - 3,
                    CELL_W, 3, GFX_CURSOR_MASK);
        gfx_cursor_on = 0;
    }
}

static void gfx_colors_update(void) {
    gfx_fg = gfx_palette[color & 0x0F];
    gfx_bg = gfx_palette[(color >> 4) & 0x0F];
}

static void cell_put(size_t r, size_t c, unsigned char ch);

static inline uint8_t vga_entry_color(uint8_t fg, uint8_t bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

/* Move the blinking hardware cursor to the current row/col */
static void vga_update_cursor(void) {
    if (gfx) {
        gfx_cursor_hide();
        fb_xor_rect((int)(col * CELL_W), (int)(row * CELL_H) + CELL_H - 3, CELL_W, 3, GFX_CURSOR_MASK);
        gfx_cursor_on = 1;
        gfx_cursor_row = row;
        gfx_cursor_col = col;
        fb_present();                      /* once per print call, not once per character */
        return;
    }
    uint16_t pos = (uint16_t)(row * con_w + col);
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
    if (gfx) gfx_colors_update();
}

static void cell_put(size_t r, size_t c, unsigned char ch) {
    if (gfx) {
        gfx_cursor_hide();
        font_draw_char_scaled((int)(c * CELL_W), (int)(r * CELL_H), (char)ch, gfx_fg, gfx_bg, 1, 2);
    } else {
        buffer[r * con_w + c] = vga_entry(ch, color);
    }
}

void vga_clear(void) {
    if (gfx) {
        gfx_cursor_on = 0;                 /* the clear wipes the cursor too */
        fb_clear(gfx_bg);
        row = 0;
        col = 0;
        vga_update_cursor();
        return;
    }
    buffer = VGA_MEMORY;
    for (size_t y = 0; y < con_h; y++) {
        for (size_t x = 0; x < con_w; x++) {
            const size_t index = y * con_w + x;
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
    if (gfx) {
        gfx_cursor_hide();
        fb_copy_rect(0, CELL_H, (int)(con_w * CELL_W), (int)((con_h - 1) * CELL_H), 0, 0);
        fb_fill_rect(0, (int)((con_h - 1) * CELL_H), (int)(con_w * CELL_W), CELL_H, gfx_bg);
        row = con_h - 1;
        return;
    }
    for (size_t y = 1; y < con_h; y++) {
        for (size_t x = 0; x < con_w; x++) {
            buffer[(y - 1) * con_w + x] = buffer[y * con_w + x];
        }
    }
    for (size_t x = 0; x < con_w; x++) {
        buffer[(con_h - 1) * con_w + x] = vga_entry(' ', color);
    }
    row = con_h - 1;
}

static void vga_newline(void) {
    col = 0;
    if (++row == con_h) {
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
            col = con_w - 1;
        }
        return;
    }
    if (c == '\t') {
        do {
            vga_put(' ');
        } while (col % 8 != 0);
        return;
    }

    cell_put(row, col, (unsigned char) c);
    if (++col == con_w) {
        vga_newline();
    }
}

/* Put one character on screen and mirror it to the serial port */
static void vga_emit(char c) {
    serial_putc(c);
    vga_put(c);
}

void vga_putchar(char c) {
    serial_putc(c);
    vga_put(c);
    vga_update_cursor();
}

/* Step back one cell and blank it */
void vga_backspace(void) {
    serial_print("\b \b");
    if (col == 0 && row == 0) return;
    if (col == 0) {
        row--;
        col = con_w - 1;
    } else {
        col--;
    }
    cell_put(row, col, ' ');
    vga_update_cursor();
}

void vga_print(const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        serial_putc(str[i]);
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
        vga_emit(buffer_[j]);
    }
    vga_update_cursor();
}

/* Print a value as 0x followed by hex digits (width follows the CPU word size) */
void vga_print_hex(uintptr_t v) {
    static const char digits[] = "0123456789ABCDEF";
    vga_emit('0');
    vga_emit('x');
    for (int i = (int)(sizeof(uintptr_t) * 2) - 1; i >= 0; i--) {
        vga_emit(digits[(v >> (i * 4)) & 0xF]);
    }
    vga_update_cursor();
}

/* Switch the console from VGA text mode to the framebuffer. Returns 0 (and
 * stays in text mode) if GRUB did not give us a graphics mode. */
int vga_use_framebuffer(void) {
    if (!fb.available) return 0;

    static const uint8_t rgb[16][3] = {
        {0x00,0x00,0x00}, {0x00,0x00,0xAA}, {0x00,0xAA,0x00}, {0x00,0xAA,0xAA},
        {0xAA,0x00,0x00}, {0xAA,0x00,0xAA}, {0xAA,0x55,0x00}, {0xAA,0xAA,0xAA},
        {0x55,0x55,0x55}, {0x55,0x55,0xFF}, {0x55,0xFF,0x55}, {0x55,0xFF,0xFF},
        {0xFF,0x55,0x55}, {0xFF,0x55,0xFF}, {0xFF,0xFF,0x55}, {0xFF,0xFF,0xFF},
    };
    for (int i = 0; i < 16; i++) {
        gfx_palette[i] = fb_make_color(rgb[i][0], rgb[i][1], rgb[i][2]);
    }
    gfx = 1;
    con_w = fb.width / CELL_W;
    con_h = fb.height / CELL_H;
    gfx_colors_update();
    gfx_cursor_on = 0;
    vga_clear();
    return 1;
}

int vga_is_graphics(void) {
    return gfx;
}
