/*
 * box.h: the project's box headers (board.h, osd, ir, audio, usbfat) for
 * SDK code, which is built against sdk/libc.
 *
 * Those headers expect ../uboot.h, whose getc/puts/printf/malloc clash
 * with the C library. Here U-Boot's calls come in under ub_ names
 * (libc/ub_exports.S) and the short names the headers use are mapped to
 * them. Include this (or sdk.h) after the C library headers.
 * usbfat.h / ubusb.h define globals: only runtime.c defines BOX_WANT_USB
 * (apps use the sdk_ file functions). audio.h keeps its ring position in
 * statics: define BOX_WANT_AUDIO in the one file that plays sound.
 */
#ifndef SDK_BOX_H
#define SDK_BOX_H

#define UBOOT_H
typedef unsigned int u32;
#define REG32(addr)     (*(volatile u32 *) (addr))
#define REG8(addr)      (*(volatile unsigned char *) (addr))
int ub_getc (void);
int ub_tstc (void);
void ub_udelay (unsigned long us);
unsigned long ub_get_timer (unsigned long base);
#define getc        ub_getc
#define tstc        ub_tstc
#define udelay      ub_udelay
#define get_timer   ub_get_timer

/* Memory (phys, see sdk.h): heap 0x01600000-0x043f0000, OSD plane and
 * audio.h buffers above it, in the gap below the AV core's buffers.
 * Satellite box: the OSD / audio addresses are in its free upper 64 MB;
 * the heap moves there too (runtime.c). */
#define OSD_HDR_PHYS    0x04400000u
#ifndef AUD_BUF_PHYS
#define AUD_BUF_PHYS    0x045d0000u         /* 0x90000 bytes */
#endif

#include "board.h"
#include "ubaddr.h"
#include "fd650.h"

/*
 * Per-box board functions. board.h is the IPTV box (LEDs on GPIO 70/71,
 * STANDBY on GPIO 11); those pins are unknown on the satellite box, so
 * there STANDBY reads "not pressed" and the green LED is the front
 * panel's (FD650). sdk_box_sat is set by the runtime from the U-Boot build.
 */
extern int sdk_box_sat;

static inline int box_standby_pressed (void) {
    return sdk_box_sat ? 0 : standby_pressed ();
}

static inline void box_led_green (int on) {
    if (sdk_box_sat) {
        fd650_led (on);
    } else {
        led_green (on);
    }
}

static inline void box_led_red (int on) {
    if (!sdk_box_sat) {
        led_red (on);
    }
}

#define standby_pressed box_standby_pressed
#define led_green       box_led_green
#define led_red         box_led_red

#include "osdsetup.h"
#include "ir.h"
#ifdef BOX_WANT_AUDIO
#include "audio.h"
#endif
#ifdef BOX_WANT_USB
#include "usbfat.h"
#endif

#endif
