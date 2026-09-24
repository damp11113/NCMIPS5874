/*
 * NC5874 box app SDK: the one header for apps.
 *
 *   #include "sdk.h"
 *   int main (int argc, char *argv[]) { ... return 0; }
 *
 * Build (WSL): sh sdk/build.sh OUT.BIN app.c [more.c ...]
 *
 * You get:
 *   C library (sdk/libc): printf, malloc, string, ctype, stdio (fopen reads
 *     whole files from the USB stick; relative paths = the app's folder)
 *   box hardware (box.h): osd_setup / fb_* drawing (osd.h, 1280x720
 *     ARGB1555), ir.h remote, board.h LEDs + STANDBY, audio.h (only in the
 *     one file that defines BOX_WANT_AUDIO before including sdk.h)
 *   input: sdk_key_poll () = remote + serial as BTN_* buttons
 *   files: sdk_file_size / sdk_read_file / sdk_dir_open / sdk_dir_read
 *   app folders: sdk_app_dir, sdk_data_dir (set when started by the launcher)
 *
 * Memory map (physical; kseg0 = 0x80000000 + phys):
 *   0x00008000-0x007fffff  app image + .bss (loaded at 0x80008000); the
 *                          rest of it is heap (region 2)
 *   0x00800000-0x009fffff  launcher (NCAPPS/LAUNCHER.BIN at 0x80800000)
 *   0x00f00000-0x01580000  U-Boot (stack, gd, code): never touch
 *   0x01600000-0x043effff  heap (malloc), region 1, ~45 MB
 *   0x04400000-0x045c2fff  OSD plane (header + 1280x720x2 pixels)
 *   0x045d0000-0x0465ffff  audio.h buffers
 *   0x0469dc00-            AV core buffers: never touch, except
 *   (big memory) 0x04f58000-0x07d03fff when the boot script set nc_bigmem=1
 *                          and gave the AV core less video memory: heap
 *                          region 3 (sdk_bigmem_bytes)
 *   0x07e10000-            AV core code
 */
#ifndef SDK_H
#define SDK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "box.h"

#define SDK_PATH_MAX    128
#define SDK_APP_ADDR    0x80008000u
#define SDK_APP_END     0x80800000u     /* app image + .bss must end below */
#define SDK_LAUNCHER_ADDR 0x80800000u
#define SDK_HEAP_START  0x81600000u
#define SDK_HEAP_END    0x843f0000u
extern u32 sdk_bigmem_bytes;                /* heap from the AV core's area, 0 = off */

/* ---- app folders ---- */

extern char sdk_app_dir[SDK_PATH_MAX];      /* e.g. "NCAPPS/APPS/DOOM", "" = root */
extern char sdk_data_dir[SDK_PATH_MAX];     /* e.g. "NCAPPS/APPSDATA/DOOM" */

void sdk_resolve (const char *path, char *out);     /* app path -> stick path */

/* ---- files and folders ---- */

struct sdk_dirent {
    char name[SDK_PATH_MAX];
    int is_dir;
    u32 size;
};

long sdk_file_size (const char *path);                  /* -1 = not found */
long sdk_read_file (const char *path, void *dst, long max);
int sdk_load_file (const char *path, unsigned char **data, long *size);
int sdk_dir_open (const char *path);                    /* handle, -1 = error */
int sdk_dir_read (int h, struct sdk_dirent *e);         /* 1 = got one, 0 = end */
void sdk_dir_close (int h);
extern void (*sdk_load_progress) (u32 done, u32 total); /* optional, big reads */
u32 sdk_usb_bytes (void);                               /* read from USB so far */

/* Streamed files: up to 3 open, read in pieces (32 KB USB reads) */
int sdk_open (const char *path);                        /* handle, -1 = error */
long sdk_read (int h, void *dst, long len);             /* bytes, 0 = end */
void sdk_seek (int h, u32 pos);
u32 sdk_tell (int h);
u32 sdk_size (int h);
void sdk_close (int h);
u32 sdk_usb_ticks (void);                               /* CP0 ticks spent in USB reads */

struct sdk_storage {
    char vendor[41], product[21], revision[9];
    u32 blocks, block_size;         /* whole device */
    int fat_bits;                   /* 16 / 32 */
    u32 cluster_bytes;
};

int sdk_storage_info (struct sdk_storage *st);         /* 0 = ok */

/* ---- input ---- */

enum {
    BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_OK, BTN_BACK, BTN_HOME,
    BTN_MENU, BTN_INFO, BTN_POWER, BTN_RED, BTN_GREEN, BTN_YELLOW, BTN_BLUE,
    BTN_PLAY, BTN_PAUSE, BTN_STOP, BTN_NEXT, BTN_MUTE,
    BTN_0, BTN_1, BTN_2, BTN_3, BTN_4, BTN_5, BTN_6, BTN_7, BTN_8, BTN_9,
};

struct sdk_key {
    int btn;        /* BTN_*, BTN_NONE for other serial characters */
    int ch;         /* serial character, 0 for the remote */
    int repeat;     /* remote button held */
    int remote;     /* 1 = from the remote */
};

int sdk_key_poll (struct sdk_key *k);       /* 1 = got a key */
const char *sdk_btn_name (int btn);

/* ---- system ---- */

void sdk_exit (int code) __attribute__ ((noreturn));

/* Nothing to do for a moment: wait us microseconds. The time is counted
 * as idle for the performance overlay's CPU figure (apps that never call
 * it show 100 %). */
void sdk_idle (u32 us);

/* Performance overlay: a thin bar at the top of the screen with CPU load
 * (from sdk_idle), heap use and USB reads, redrawn every 0.5 s from
 * sdk_key_poll / sdk_idle. MUTE on the remote toggles it in any SDK app;
 * the launcher passes its setting to apps ("@ovl=1"). */
extern int sdk_overlay_on;
void sdk_overlay_tick (void);
void sdk_reboot (void) __attribute__ ((noreturn));     /* watchdog reset, boots from flash */
void sdk_cache_sync (u32 start, u32 len);
void sdk_libc_reset (void);

#endif
