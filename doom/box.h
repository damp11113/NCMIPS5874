/*
 * box.h: use the project's box headers (board.h, osd, ir, usbfat,
 * audio) from Doom code, which is built against doom/libc.
 *
 * Those headers expect ../uboot.h, whose getc/puts/printf/malloc clash
 * with the C library. Here U-Boot's calls come in under ub_ names
 * (libc/ub_exports.S) and the short names the headers use are mapped to
 * them. Include this after the C library headers, in .c files only.
 * usbfat.h / ubusb.h define globals, so only one file may define
 * BOX_WANT_USB; audio.h keeps its ring position in statics, so only the
 * sound file defines BOX_WANT_AUDIO.
 */
#ifndef DOOM_BOX_H
#define DOOM_BOX_H

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

/* Memory (phys): heap 0x01600000-0x037f0000 (libc.c, 34 MB: IWAD + PWAD
 * + zone), audio.h buffers 0x03800000-0x03890000, OSD plane here. */
#define OSD_HDR_PHYS    0x03a00000u

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
