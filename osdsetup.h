/*
 * osd_setup: turn on OSD layer 6 from plain U-Boot (no stock firmware).
 * Verified on hardware 2026-09-24 (square square, full frame, 1080i).
 *
 * Needs HDMI already running: source avstart.scr (U-Boot av_launch) first.
 * Uses RAM at phys 0x03000000..0x031c3000 for the region header + pixels.
 *
 * Output size is in 0xbf4400b8 (h<<16 | w, per field: 540 x 1920 for
 * U-Boot's default 1080i, 1080 x 1920 for 1080p). The OSD scaler has two
 * modes, picked by 0xbf440100:
 *   0x000e0001 (U-Boot, interlaced): scale = src/dst in 16.16 fixed point,
 *     size regs = src<<16 | dst-per-field. Used for 1080i.
 *   0x000c0003 (stock firmware, progressive): scale in 4.12 (0xaaa =
 *     0.667). Used for 1080p / 720p (boot script patches U-Boot's display
 *     mode, see ncboot.txt).
 * Mixing them (firmware mode on U-Boot's 1080i) gave a ~4x tall picture.
 * At 1080p also see osd_vscale_fix () (vertical scaler bypass bit).
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

/*
 * U-Boot's own layer scaler setup (link 0x801502b0) sets bit 8 of
 * 0xbf440124 when its source and output heights are equal, which they are
 * at 1080p; the OSD scaler then passes our 720 lines through unscaled
 * (mixer line count 0xbf440144 reads 721 instead of 1081, picture in the
 * top 2/3 of the screen). Never set at 1080i (540 per field). Cleared by
 * osd_setup and again from the SDK's key / idle loop in case U-Boot's
 * display code sets it later.
 */
static inline void osd_vscale_fix(void) {
    if ((REG32(0xbf4400b8) >> 16) >= 720 && (REG32(0xbf440124) & 0x100u)) {
        REG32(0xbf440124) &= ~0x100u;
    }
}

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

    if (dst_h >= 720) {
        /* Progressive output (1080p / 720p): stock firmware's mode, 4.12
         * ratios (verified at 1080p60 together with osd_vscale_fix) */
        REG32(0xbf440100) = 0x000c0003;
        REG32(0xbf440108) = 0x0e000000;
        REG32(0xbf44010c) = 0x1fa40000;
        REG32(0xbf440120) = 0x00000011;
        REG32(0xbf440110) = (OSD_SRC_W << 16) | dst_w;
        REG32(0xbf440114) = (OSD_SRC_W << 12) / dst_w;
        REG32(0xbf440128) = (OSD_SRC_H << 16) | dst_h;
        REG32(0xbf44012c) = (OSD_SRC_H << 12) / dst_h;
        osd_vscale_fix();
    } else {
        /* Interlaced output (1080i: dst_h = 540 per field): U-Boot's own
         * mode, 16.16 ratios */
        REG32(0xbf440100) = 0x000e0001;
        REG32(0xbf440108) = 0x07000000;
        REG32(0xbf44010c) = 0x0fd20000;
        REG32(0xbf440120) = 0x00000021;
        REG32(0xbf440110) = (OSD_SRC_W << 16) | dst_w;
        REG32(0xbf440114) = (OSD_SRC_W << 16) / dst_w;
        REG32(0xbf440128) = (OSD_SRC_H << 16) | dst_h;
        REG32(0xbf44012c) = (OSD_SRC_H << 16) / dst_h;
    }

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
