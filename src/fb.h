#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t *buffer;        /* front buffer: the video memory GRUB gave us */
    uint32_t *draw;          /* where drawing happens: the back buffer, or == buffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;          /* bytes per row (same for both buffers) */
    uint32_t bpp;
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
    int      available;      /* 1 if GRUB set a usable 32-bit graphics mode */
    int      double_buffered;
} framebuffer_t;

extern framebuffer_t fb;

/* Setup */
int  fb_init(void);                 /* read GRUB's framebuffer tag; 1 if usable */
void fb_enable_backbuffer(void);    /* allocate the back buffer (needs the heap) */

/* Putting the picture on screen. Drawing only touches the back buffer and
 * remembers the changed region; fb_present() copies just that region. */
void fb_present(void);
void fb_present_all(void);
void fb_mark_dirty(int x, int y, int w, int h);

/* Clipping: all drawing is limited to this rectangle (default: whole screen) */
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);

/* Colors and pixels */
uint32_t fb_make_color(uint8_t r, uint8_t g, uint8_t b);
uint32_t fb_get_pixel(int x, int y);
void fb_set_pixel(int x, int y, uint32_t color);

/* Shapes */
void fb_clear(uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);   /* 1 px outline */
void fb_draw_hline(int x, int y, int w, uint32_t color);
void fb_draw_vline(int x, int y, int h, uint32_t color);
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* Copying */
void fb_blit(int x, int y, const uint32_t *src, int w, int h, int src_stride_px);
void fb_copy_rect(int sx, int sy, int w, int h, int dx, int dy);  /* overlap-safe */
void fb_xor_rect(int x, int y, int w, int h, uint32_t mask);

#endif
