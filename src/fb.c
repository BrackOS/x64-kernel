#include "fb.h"
#include "multiboot.h"
#include "heap.h"
#include "serial.h"
#include <stdint.h>

framebuffer_t fb = {0};

/* Multiboot2 framebuffer tag (type 8). For direct-color modes (fb_type 1)
 * the color layout follows the common part. */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;       /* 0 = indexed, 1 = direct RGB, 2 = EGA text */
    uint16_t reserved;
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
} __attribute__((packed)) mb_fb_tag_t;

/* Clip rectangle (exclusive right/bottom edges) and the region changed since
 * the last present */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static int dirty_any = 0;

static inline void fill32(uint32_t *p, uint32_t v, size_t n) {
    __asm__ volatile("cld; rep stosl" : "+D"(p), "+c"(n) : "a"(v) : "memory");
}

static inline void copy32(uint32_t *dst, const uint32_t *src, size_t n) {
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

/* Overlap-safe copy of n pixels */
static void move32(uint32_t *dst, const uint32_t *src, size_t n) {
    if (dst <= src) {
        copy32(dst, src, n);
    } else {
        while (n--) dst[n] = src[n];
    }
}

int fb_init(void) {
    fb.available = 0;
    const mb_fb_tag_t *tag = (const mb_fb_tag_t *)multiboot_find_tag(MB_TAG_FRAMEBUFFER);
    if (!tag) {
        serial_print("FB: no framebuffer tag from GRUB\n");
        return 0;
    }
    if (tag->fb_type != 1 || tag->bpp != 32) {
        serial_print("FB: GRUB gave a text or non-32-bit mode, using the text console\n");
        return 0;
    }
    if (tag->addr == 0 || tag->addr >= 0x100000000ULL ||
        tag->width < 320 || tag->width > 4096 || tag->height < 200 || tag->height > 4096 ||
        tag->pitch < tag->width * 4) {
        serial_print("FB: unusable framebuffer, using the text console\n");
        return 0;
    }

    fb.buffer = (uint32_t *)(uintptr_t)tag->addr;
    fb.draw   = fb.buffer;                 /* no back buffer until the heap exists */
    fb.width  = tag->width;
    fb.height = tag->height;
    fb.pitch  = tag->pitch;
    fb.bpp    = 32;
    fb.red_pos   = tag->red_pos;   fb.red_size   = tag->red_size;
    fb.green_pos = tag->green_pos; fb.green_size = tag->green_size;
    fb.blue_pos  = tag->blue_pos;  fb.blue_size  = tag->blue_size;
    if (fb.red_size == 0 || fb.green_size == 0 || fb.blue_size == 0) {   /* assume XRGB */
        fb.red_pos = 16;  fb.red_size = 8;
        fb.green_pos = 8; fb.green_size = 8;
        fb.blue_pos = 0;  fb.blue_size = 8;
    }
    fb.double_buffered = 0;
    fb.available = 1;
    fb_reset_clip();
    dirty_any = 0;
    return 1;
}

void fb_enable_backbuffer(void) {
    if (!fb.available || fb.double_buffered) return;

    size_t bytes = (size_t)fb.pitch * fb.height;
    uint32_t *back = (uint32_t *)kmalloc(bytes);
    if (!back) {
        serial_print("FB: no memory for a back buffer, drawing directly\n");
        return;                            /* keep drawing straight to the screen */
    }
    copy32(back, fb.buffer, bytes / 4);    /* keep what is already on screen */
    fb.draw = back;
    fb.double_buffered = 1;
    dirty_any = 0;
}

/* ---- Dirty region and presenting ---- */

void fb_mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > (int)fb.width)  x1 = (int)fb.width;
    if (y1 > (int)fb.height) y1 = (int)fb.height;
    if (x >= x1 || y >= y1) return;
    if (!dirty_any) {
        dirty_x0 = x; dirty_y0 = y; dirty_x1 = x1; dirty_y1 = y1;
        dirty_any = 1;
    } else {
        if (x  < dirty_x0) dirty_x0 = x;
        if (y  < dirty_y0) dirty_y0 = y;
        if (x1 > dirty_x1) dirty_x1 = x1;
        if (y1 > dirty_y1) dirty_y1 = y1;
    }
}

void fb_present(void) {
    if (!fb.available || !dirty_any) return;
    if (fb.double_buffered) {
        size_t stride = fb.pitch / 4;
        size_t n = (size_t)(dirty_x1 - dirty_x0);
        for (int y = dirty_y0; y < dirty_y1; y++) {
            size_t off = (size_t)y * stride + (size_t)dirty_x0;
            copy32(fb.buffer + off, fb.draw + off, n);
        }
    }
    dirty_any = 0;
}

void fb_present_all(void) {
    fb_mark_dirty(0, 0, (int)fb.width, (int)fb.height);
    fb_present();
}

/* ---- Clipping ---- */

void fb_reset_clip(void) {
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = (int)fb.width; clip_y1 = (int)fb.height;
}

void fb_set_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > (int)fb.width  ? (int)fb.width  : x + w;
    clip_y1 = y + h > (int)fb.height ? (int)fb.height : y + h;
}

/* Intersect a rectangle with the clip rectangle; returns 0 if nothing is left */
static int clip_rect(int *x, int *y, int *w, int *h) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

/* ---- Colors and pixels ---- */

uint32_t fb_make_color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r >> (8 - fb.red_size))   << fb.red_pos) |
           ((uint32_t)(g >> (8 - fb.green_size)) << fb.green_pos) |
           ((uint32_t)(b >> (8 - fb.blue_size))  << fb.blue_pos);
}

uint32_t fb_get_pixel(int x, int y) {
    if (!fb.available || x < 0 || y < 0 || x >= (int)fb.width || y >= (int)fb.height) return 0;
    return fb.draw[(size_t)y * (fb.pitch / 4) + (size_t)x];
}

void fb_set_pixel(int x, int y, uint32_t color) {
    if (!fb.available) return;
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    fb.draw[(size_t)y * (fb.pitch / 4) + (size_t)x] = color;
    fb_mark_dirty(x, y, 1, 1);
}

/* ---- Shapes ---- */

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!fb.available || !clip_rect(&x, &y, &w, &h)) return;
    size_t stride = fb.pitch / 4;
    for (int row = y; row < y + h; row++) {
        fill32(fb.draw + (size_t)row * stride + (size_t)x, color, (size_t)w);
    }
    fb_mark_dirty(x, y, w, h);
}

void fb_clear(uint32_t color) {
    int saved[4] = { clip_x0, clip_y0, clip_x1, clip_y1 };
    fb_reset_clip();
    fb_fill_rect(0, 0, (int)fb.width, (int)fb.height, color);
    clip_x0 = saved[0]; clip_y0 = saved[1]; clip_x1 = saved[2]; clip_y1 = saved[3];
}

void fb_draw_hline(int x, int y, int w, uint32_t color) { fb_fill_rect(x, y, w, 1, color); }
void fb_draw_vline(int x, int y, int h, uint32_t color) { fb_fill_rect(x, y, 1, h, color); }

void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        fb_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ---- Copying ---- */

/* Draw a w x h block of pixels (rows are src_stride_px pixels apart) at x, y */
void fb_blit(int x, int y, const uint32_t *src, int w, int h, int src_stride_px) {
    if (!fb.available || !src) return;
    int cx = x, cy = y, cw = w, ch = h;
    if (!clip_rect(&cx, &cy, &cw, &ch)) return;
    size_t stride = fb.pitch / 4;
    for (int row = 0; row < ch; row++) {
        const uint32_t *s = src + (size_t)(cy - y + row) * (size_t)src_stride_px + (size_t)(cx - x);
        copy32(fb.draw + (size_t)(cy + row) * stride + (size_t)cx, s, (size_t)cw);
    }
    fb_mark_dirty(cx, cy, cw, ch);
}

/* Copy a rectangle of the picture to another place in it (scrolling) */
void fb_copy_rect(int sx, int sy, int w, int h, int dx, int dy) {
    if (!fb.available || w <= 0 || h <= 0) return;
    /* source must be on screen */
    if (sx < 0 || sy < 0 || sx + w > (int)fb.width || sy + h > (int)fb.height) return;
    int cx = dx, cy = dy, cw = w, ch = h;
    if (!clip_rect(&cx, &cy, &cw, &ch)) return;
    int ssx = sx + (cx - dx), ssy = sy + (cy - dy);

    size_t stride = fb.pitch / 4;
    if (cy <= ssy) {
        for (int row = 0; row < ch; row++) {
            move32(fb.draw + (size_t)(cy + row) * stride + (size_t)cx,
                   fb.draw + (size_t)(ssy + row) * stride + (size_t)ssx, (size_t)cw);
        }
    } else {
        for (int row = ch - 1; row >= 0; row--) {
            move32(fb.draw + (size_t)(cy + row) * stride + (size_t)cx,
                   fb.draw + (size_t)(ssy + row) * stride + (size_t)ssx, (size_t)cw);
        }
    }
    fb_mark_dirty(cx, cy, cw, ch);
}

/* Invert the bits in mask for every pixel of a rectangle (self-inverse) */
void fb_xor_rect(int x, int y, int w, int h, uint32_t mask) {
    if (!fb.available || !clip_rect(&x, &y, &w, &h)) return;
    size_t stride = fb.pitch / 4;
    for (int row = y; row < y + h; row++) {
        uint32_t *p = fb.draw + (size_t)row * stride + (size_t)x;
        for (int i = 0; i < w; i++) p[i] ^= mask;
    }
    fb_mark_dirty(x, y, w, h);
}
