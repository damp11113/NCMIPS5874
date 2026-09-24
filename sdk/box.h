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
 * audio.h buffers above it, in the gap below the AV core's buffers. */
#define OSD_HDR_PHYS    0x04400000u
#ifndef AUD_BUF_PHYS
#define AUD_BUF_PHYS    0x045d0000u         /* 0x90000 bytes */
#endif

#include "board.h"
#include "osdsetup.h"
#include "ir.h"
#ifdef BOX_WANT_AUDIO
#include "audio.h"
#endif
#ifdef BOX_WANT_USB
#include "usbfat.h"
#endif

#endif
