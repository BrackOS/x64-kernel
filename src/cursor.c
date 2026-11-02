#include "cursor.h"
#include "fb.h"

// 8x8 arrow cursor bitmap (top-left origin)
// Each row is a byte: bit 7=left, bit 0=right
static const uint8_t cursor_bitmap[CURSOR_HEIGHT] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0x00,
};

static uint32_t cursor_color = 0;       // set in cursor_init (needs the screen's color format)
static uint32_t outline_color = 0;
static int cursor_visible = 0;
static int cursor_x = 0, cursor_y = 0;
static uint32_t under[CURSOR_WIDTH * CURSOR_HEIGHT];   // the pixels the cursor covers

void cursor_init(void) {
    cursor_color = fb_make_color(0, 0, 0);
    outline_color = fb_make_color(255, 255, 255);
    cursor_visible = 0;
}

// Put back the pixels the cursor was covering
void cursor_hide(void) {
    if (!cursor_visible) return;
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            fb_set_pixel(cursor_x + col, cursor_y + row, under[row * CURSOR_WIDTH + col]);
        }
    }
    cursor_visible = 0;
    fb_present();
}

// The screen was redrawn under us: do not restore stale pixels
void cursor_forget(void) {
    cursor_visible = 0;
}

void cursor_draw(int x, int y) {
    cursor_hide();
    cursor_x = x;
    cursor_y = y;

    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            under[row * CURSOR_WIDTH + col] = fb_get_pixel(x + col, y + row);
        }
    }
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            if (cursor_bitmap[row] & (0x80 >> col)) {
                // white edge pixels keep the arrow visible on dark backgrounds
                int edge = (col == 0) || (row == 0) || !(cursor_bitmap[row] & (0x80 >> (col + 1)));
                fb_set_pixel(x + col, y + row, edge ? outline_color : cursor_color);
            }
        }
    }
    cursor_visible = 1;
    fb_present();
}

void cursor_update(int x, int y) {
    cursor_draw(x, y);
}

void cursor_set_color(uint32_t color) {
    cursor_color = color;
}
