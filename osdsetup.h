/*
 * osd_setup: turn on OSD layer 6 from plain U-Boot (no stock firmware).
 * Verified on hardware 2026-09-24 (square square, full frame, 1080i).
 *
 * Needs HDMI already running: source avstart.scr (U-Boot av_launch) first.
 * Uses RAM at phys 0x03000000..0x031c3000 for the region header + pixels.
 *
 * Output size is in 0xbf4400b8 (h<<16 | w, per field: 540 x 1920 for
 * U-Boot's 1080i). The OSD scaler has two modes, picked by 0xbf440100:
 *   0x000e0001 (U-Boot, interlaced): scale = src/dst in 16.16 fixed point,
 *     size regs = src<<16 | dst-per-field. Used here.
 *   0x000c0003 (stock firmware, 1080p): scale in 4.12 (0xaaa = 0.667).
 * Mixing them (firmware mode on U-Boot's 1080i) gave a ~4x tall picture.
 */
#ifndef OSDSETUP_H
#define OSDSETUP_H

#include "osd.h"

/* A program can #define OSD_HDR_PHYS before including this to move the
 * plane (0x1c3000 bytes: header, pixels 0x1000 after it) */
#ifndef OSD_HDR_PHYS
#define OSD_HDR_PHYS    0x03000000u
#endif
#define OSD_PIX_PHYS    (OSD_HDR_PHYS + 0x1000u)
#define OSD_SRC_W       1280
#define OSD_SRC_H       720

#ifndef REG32
#define REG32(addr)     (*(volatile u32 *) (addr))
#endif

/* Fills fb, returns 0 on success, -1 if the display is not running */
static inline int osd_setup(struct fb *fb) {
    volatile u32 *hdr = (volatile u32 *) (0xa0000000u | OSD_HDR_PHYS);
    u32 out = REG32(0xbf4400b8);
    u32 dst_w = out & 0xffff, dst_h = out >> 16;
    int i;

    if (dst_w < 640 || dst_w > 1920 || dst_h < 240 || dst_h > 1080) {
        return -1;
    }

    /* Region header, same layout as the stock firmware's */
    for (i = 0; i < 18; i++) {
        hdr[i] = 0;
    }
    hdr[0] = 0x30300001;
    hdr[1] = 0x00010001;
    hdr[2] = (OSD_SRC_H << 16) | OSD_SRC_W;
    hdr[3] = (OSD_SRC_W << 16) | 0xff;
    hdr[4] = 0xa0000000u | OSD_PIX_PHYS;
    hdr[6] = 0xa0000000u | (OSD_HDR_PHYS + 0x1c2040);

    fb->pix = (volatile u16 *) (0xa0000000u | OSD_PIX_PHYS);
    fb->w = OSD_SRC_W;
    fb->h = OSD_SRC_H;
    fb->pitch = OSD_SRC_W;
    fb_clear(fb, TRANSPARENT);

    /* Scaler, U-Boot's own mode: 1280x720 -> output, 16.16 ratios */
    REG32(0xbf440100) = 0x000e0001;
    REG32(0xbf440108) = 0x07000000;
    REG32(0xbf44010c) = 0x0fd20000;
    REG32(0xbf440120) = 0x00000021;
    REG32(0xbf440110) = (OSD_SRC_W << 16) | dst_w;
    REG32(0xbf440114) = (OSD_SRC_W << 16) / dst_w;
    REG32(0xbf440128) = (OSD_SRC_H << 16) | dst_h;
    REG32(0xbf44012c) = (OSD_SRC_H << 16) / dst_h;

    /* Layer control / enables (values from the stock firmware) */
    REG32(0xbf44006c) = 0x00d70111;
    REG32(0xbf440070) = 0x010000ff;
    REG32(0xbf440090) = 0x010000ff;
    REG32(0xbf4400a8) = 0x30000000;
    REG32(0xbf440160) = 0x1e028000;
    REG32(0xbf441034) = 0x9012d0d0;
    REG32(0xbf441028) = OSD_HDR_PHYS >> 3;
    REG32(0xbf440060) = 0x00000001;
    REG32(0xbf440000) = 0x10001100;
    return 0;
}

#endif
