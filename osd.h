/*
 * Tiny graphics library for the OSD plane (verified on hardware).
 *
 * The display's OSD layer 6 is a 1280x720 ARGB1555 image:
 *   bit 15 = opaque (0 = transparent, video/background shows through)
 *   bits 14..10 red, 9..5 green, 4..0 blue (5 bits each)
 * Layer address register 0xbf441028 = region header physical addr >> 3.
 * Header word 2 = h<<16 | w, word 3 = pitch_px<<16 | alpha,
 * word 4 = pixel buffer (uncached CPU address).
 *
 * fb_init () reads the layer the stock firmware set up (inside a hook).
 * From plain U-Boot, use osd_setup () in osdsetup.h instead.
 */
#ifndef OSD_LIB_H
#define OSD_LIB_H

#include "font8x16.h"

typedef unsigned int u32;
typedef unsigned short u16;

#define OSD_LAYER6_REG  0xbf441028

/* Colour from 8-bit r, g, b (opaque) */
#define RGB(r, g, b)    ((u16) (0x8000 | (((r) >> 3) << 10) | (((g) >> 3) << 5) | ((b) >> 3)))
#define TRANSPARENT     ((u16) 0x0000)

#define BLACK           RGB(0, 0, 0)
#define WHITE           RGB(255, 255, 255)
#define RED             RGB(255, 0, 0)
#define GREEN           RGB(0, 255, 0)
/* Exact RGB (0, 0, 255) = 0x801f is the layer's colour key and shows as
 * transparent (seen on hardware), so "blue" is one step darker. */
#define BLUE            RGB(0, 0, 240)
#define YELLOW          RGB(255, 255, 0)
#define CYAN            RGB(0, 255, 255)
#define MAGENTA         RGB(255, 0, 255)
#define GREY            RGB(128, 128, 128)

struct fb {
    volatile u16 *pix;
    int w, h, pitch;
};

/* Returns 0 on success, -1 if the header does not look like an OSD region */
static inline int fb_init(struct fb *fb) {
    u32 phys = ((*(volatile u32 *) OSD_LAYER6_REG) & 0x03ffffffu) << 3;
    volatile u32 *hdr = (volatile u32 *) (0xa0000000u | phys);
    u32 buf = hdr[4];

    fb->w = hdr[2] & 0xffff;
    fb->h = hdr[2] >> 16;
    fb->pitch = hdr[3] >> 16;
    fb->pix = (volatile u16 *) buf;
    if (fb->w <= 0 || fb->w > 1920 || fb->h <= 0 || fb->h > 1080 || fb->pitch < fb->w
            || buf < 0xa0100000u || buf >= 0xa8000000u) {
        return -1;
    }
    return 0;
}

static inline void fb_pixel(struct fb *fb, int x, int y, u16 c) {
    if (x >= 0 && y >= 0 && x < fb->w && y < fb->h) {
        fb->pix[y * fb->pitch + x] = c;
    }
}

static inline void fb_rect(struct fb *fb, int x, int y, int w, int h, u16 c) {
    int i, j;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > fb->w) {
        w = fb->w - x;
    }
    if (y + h > fb->h) {
        h = fb->h - y;
    }
    for (j = 0; j < h; j++) {
        volatile u16 *row = fb->pix + (y + j) * fb->pitch + x;
        for (i = 0; i < w; i++) {
            row[i] = c;
        }
    }
}

static inline void fb_clear(struct fb *fb, u16 c) {
    fb_rect(fb, 0, 0, fb->w, fb->h, c);
}

/* Draw one character, each font pixel becomes a scale x scale block.
 * bg = TRANSPARENT leaves the background untouched. */
static inline void fb_char(struct fb *fb, int x, int y, char ch, int scale, u16 fg, u16 bg) {
    const unsigned char *g;
    int row, col;

    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    g = font8x16[ch - 32];
    for (row = 0; row < 16; row++) {
        for (col = 0; col < 8; col++) {
            if (g[row] & (0x80 >> col)) {
                fb_rect(fb, x + col * scale, y + row * scale, scale, scale, fg);
            } else if (bg != TRANSPARENT) {
                fb_rect(fb, x + col * scale, y + row * scale, scale, scale, bg);
            }
        }
    }
}

static inline void fb_text(struct fb *fb, int x, int y, const char *s, int scale, u16 fg, u16 bg) {
    for (; *s; s++, x += 8 * scale) {
        fb_char(fb, x, y, *s, scale, fg, bg);
    }
}

#endif
