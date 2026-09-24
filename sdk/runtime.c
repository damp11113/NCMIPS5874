/*
 * SDK runtime for NC5874 box apps: program entry, exit, app folders,
 * files and folders on the USB stick, input, launcher helpers.
 *
 * Entry: _start is the first byte of every app (link.ld puts .text.start
 * first). It clears .bss, runs C++ constructors, reads the launcher's
 * arguments, calls main (), runs atexit () handlers and returns main's
 * value (or exit ()'s code) to whoever started the app: U-Boot's 'go' or
 * the launcher.
 *
 * Arguments: U-Boot's go passes argv[0] = the load address, then the
 * user's words. The launcher passes argv[0] = "@app=<app folder>",
 * argv[1] = "@data=<data folder>", then the APP.INF arguments. main ()
 * always gets argv[0] = "app" followed by the real arguments.
 *
 * Files: sdk_load_file (), and fopen () on top of it, read whole files
 * from the stick (usbfat.h through U-Boot's USB driver; 'usb start' must
 * have run). Paths starting with '/' are from the stick's root, other
 * paths are inside the app's folder (sdk_app_dir; the root when started
 * from U-Boot).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOX_WANT_USB
#define BOX_WANT_AUDIO              /* audio_stop () when the stick is gone */
#include "sdk.h"

int main (int argc, char *argv[]);

char sdk_app_dir[SDK_PATH_MAX];
char sdk_data_dir[SDK_PATH_MAX];
void (*sdk_load_progress) (u32 done, u32 total);

static void *exit_buf[5];
static int exit_code;
static void (*atexit_fn[8]) (void);
static int atexit_count;

/* ---- entry / exit ---- */

extern unsigned int __bss_start[], __bss_end[];
int _start (int argc, char *argv[]);
void sdk_heap_region (size_t start, size_t end);        /* libc.c */
char *ub_getenv (const char *name);                     /* ub_exports.S */

/* Big memory: the boot script can start the AV core with less video
 * memory (avenv_VIDEO_FW_CFG_SIZE) and set nc_bigmem=1; the rest of its
 * original 50 MB video area (up to 0x07d04000) is then ours. */
#define AV_VIDEO_END 0x87d04000u
u32 sdk_bigmem_bytes;

static void heap_regions (void) {
    u32 limit = (u32) _start >= SDK_LAUNCHER_ADDR ? SDK_LAUNCHER_ADDR + 0x200000 : SDK_APP_END;
    const char *big = ub_getenv ("nc_bigmem");

    sdk_heap_region (SDK_HEAP_START, SDK_HEAP_END);
    /* the rest of our own load area (the image ends at __bss_end) */
    sdk_heap_region ((u32) __bss_end + 0x10000, limit - 0x10000);
    if (big && big[0] == '1') {
        const char *addr = ub_getenv ("avenv_VIDEO_FW_CFG_ADDR");
        const char *size = ub_getenv ("avenv_VIDEO_FW_CFG_SIZE");

        if (addr && size) {
            u32 start = 0x80000000u | ((strtoul (addr, 0, 16) + strtoul (size, 0, 16)) & 0x1fffffffu);

            if (start < AV_VIDEO_END) {
                sdk_bigmem_bytes = AV_VIDEO_END - start;
                sdk_heap_region (start + 0x10000, AV_VIDEO_END);
            }
        }
    }
}
extern void (*__init_array_start[]) (void);
extern void (*__init_array_end[]) (void);

static void run_atexit (void) {
    while (atexit_count > 0) {
        atexit_fn[--atexit_count] ();
    }
}

static void copy_arg (char *dst, const char *src) {
    strncpy (dst, src, SDK_PATH_MAX - 1);
    dst[SDK_PATH_MAX - 1] = 0;
}

__attribute__ ((section (".text.start")))
int _start (int argc, char *argv[]) {
    static char *args[32];
    volatile unsigned int *p;
    void (**ctor) (void);
    int n = 0, i = 1;

    for (p = __bss_start; p < __bss_end; p++) {
        *p = 0;
    }
    heap_regions ();
    for (ctor = __init_array_start; ctor < __init_array_end; ctor++) {
        (*ctor) ();
    }

    args[n++] = "app";
    if (argc > 0 && !strncmp (argv[0], "@app=", 5)) {
        copy_arg (sdk_app_dir, argv[0] + 5);
        if (argc > 1 && !strncmp (argv[1], "@data=", 6)) {
            copy_arg (sdk_data_dir, argv[1] + 6);
            i = 2;
        }
        if (i < argc && !strncmp (argv[i], "@ovl=", 5)) {
            sdk_overlay_on = argv[i][5] == '1';
            i++;
        }
    }
    for (; i < argc && n < 31; i++) {
        args[n++] = argv[i];
    }
    args[n] = 0;

    if (__builtin_setjmp (exit_buf) == 0) {
        exit_code = main (n, args);
    }
    run_atexit ();
    return exit_code;
}

void sdk_exit (int code) {
    exit_code = code;
    __builtin_longjmp (exit_buf, 1);
}

int atexit (void (*fn) (void)) {
    if (atexit_count == (int) (sizeof (atexit_fn) / sizeof (atexit_fn[0]))) {
        return -1;
    }
    atexit_fn[atexit_count++] = fn;
    return 0;
}

/* ---- files ---- */

static int mounted;

/*
 * The stick was pulled out (or stopped answering): nothing can be read any
 * more, so every SDK app ends up here instead of hanging. Put it back and
 * press OK (or wait: re-inserting is detected) to restart the box.
 */
static void usb_lost (void) {
    static int shown;
    struct fb f;
    struct sdk_key k;
    int back = 0;

    if (shown++) {
        return;
    }
    memset (&f, 0, sizeof (f));
    audio_stop ();                  /* else the output holds the last sample (DC) */
    printf ("\nsdk: USB stick removed or not answering\n");
    if (osd_setup (&f) == 0) {
        fb_clear (&f, RGB (60, 10, 10));
        fb_text (&f, 80, 200, "USB stick removed", 4, WHITE, TRANSPARENT);
        fb_text (&f, 80, 300, "Put the stick back in, then press OK", 2, WHITE, TRANSPARENT);
        fb_text (&f, 80, 340, "(or POWER) to restart the box.", 2, WHITE, TRANSPARENT);
    }
    for (;;) {
        if (!back && ufs_stick_present ()) {
            back = 1;
            printf ("sdk: stick is back\n");
            if (f.pix) {
                fb_text (&f, 80, 420, "Stick found - press OK to restart", 2, YELLOW, TRANSPARENT);
            }
        }
        if (sdk_key_poll (&k) && (k.btn == BTN_OK || k.btn == BTN_POWER) && !k.repeat) {
            sdk_reboot ();
        }
        if (standby_pressed ()) {
            sdk_reboot ();
        }
        ub_udelay (20000);
    }
}

/* After any USB access: stop here if the stick is gone */
static inline void lost_check (void) {
    if (ufs_lost) {
        usb_lost ();
    }
}

static int mount (void) {
    lost_check ();
    if (!mounted) {
        if (ufs_mount () < 0) {
            lost_check ();
            return -1;
        }
        mounted = 1;
    }
    return 0;
}

/* Full path on the stick for an app path */
void sdk_resolve (const char *path, char *out) {
    if (path[0] == '/' || !sdk_app_dir[0]) {
        copy_arg (out, path[0] == '/' ? path + 1 : path);
    } else {
        snprintf (out, SDK_PATH_MAX, "%s/%s", sdk_app_dir, path);
    }
}

long sdk_file_size (const char *path) {
    static struct ufile f;
    char full[SDK_PATH_MAX];

    if (mount () < 0) {
        return -1;
    }
    sdk_resolve (path, full);
    if (ufs_open (&f, full) < 0) {
        lost_check ();
        return -1;
    }
    return f.size;
}

/* Read up to max bytes of a file to dst. Returns the size read, or -1. */
long sdk_read_file (const char *path, void *dst, long max) {
    static struct ufile f;
    char full[SDK_PATH_MAX];
    u32 done = 0, total;

    if (mount () < 0) {
        return -1;
    }
    sdk_resolve (path, full);
    if (ufs_open (&f, full) < 0) {
        return -1;
    }
    total = f.size < (u32) max ? f.size : (u32) max;
    while (done < total) {
        u32 n = ufs_read (&f, (unsigned char *) dst + done,
                          total - done < 256 * 1024 ? total - done : 256 * 1024);

        if (n == 0) {
            lost_check ();
            return -1;
        }
        done += n;
        if (sdk_load_progress) {
            sdk_load_progress (done, total);
        }
    }
    return done;
}

/* Whole file into malloc'd memory (for fopen). Returns 0, or -1. */
int sdk_load_file (const char *path, unsigned char **data, long *size) {
    static struct ufile f;
    char full[SDK_PATH_MAX];
    unsigned char *buf;
    u32 done = 0;

    if (mount () < 0) {
        return -1;
    }
    sdk_resolve (path, full);
    if (ufs_open (&f, full) < 0) {                  /* one lookup, not two */
        lost_check ();
        return -1;
    }
    buf = malloc (f.size + 1);
    if (!buf) {
        printf ("sdk: no memory for %s (%u bytes)\n", path, f.size);
        return -1;
    }
    if (f.size > 1024 * 1024) {
        printf ("sdk: loading %s (%u KB) from USB\n", path, f.size / 1024);
    }
    while (done < f.size) {
        u32 n = ufs_read (&f, buf + done, f.size - done < 256 * 1024 ? f.size - done : 256 * 1024);

        if (n == 0) {
            free (buf);
            lost_check ();
            return -1;
        }
        done += n;
        if (sdk_load_progress) {
            sdk_load_progress (done, f.size);
        }
    }
    *data = buf;
    *size = f.size;
    return 0;
}

u32 sdk_usb_bytes (void) {
    return ufs_bytes;
}

/* ---- streamed files (songs, big data: read in pieces, seek) ---- */

#define MAX_STREAMS 3
static struct ufile streams[MAX_STREAMS];
static int stream_used[MAX_STREAMS];

int sdk_open (const char *path) {
    char full[SDK_PATH_MAX];
    int h;

    if (mount () < 0) {
        return -1;
    }
    for (h = 0; h < MAX_STREAMS && stream_used[h]; h++) {
    }
    if (h == MAX_STREAMS) {
        return -1;
    }
    sdk_resolve (path, full);
    if (ufs_open (&streams[h], full) < 0) {
        lost_check ();
        return -1;
    }
    stream_used[h] = 1;
    return h;
}

long sdk_read (int h, void *dst, long len) {
    long n;

    if (h < 0 || h >= MAX_STREAMS || !stream_used[h] || len <= 0) {
        return 0;
    }
    n = ufs_read (&streams[h], dst, len);
    lost_check ();
    return n;
}

void sdk_seek (int h, u32 pos) {
    if (h >= 0 && h < MAX_STREAMS && stream_used[h]) {
        ufs_seek (&streams[h], pos);
    }
}

u32 sdk_tell (int h) {
    return (h >= 0 && h < MAX_STREAMS && stream_used[h]) ? streams[h].pos : 0;
}

u32 sdk_size (int h) {
    return (h >= 0 && h < MAX_STREAMS && stream_used[h]) ? streams[h].size : 0;
}

void sdk_close (int h) {
    if (h >= 0 && h < MAX_STREAMS) {
        stream_used[h] = 0;
    }
}

u32 sdk_usb_ticks (void) {
    return ufs_ticks;
}

/* USB stick details (for info screens). Returns 0, or -1. */
int sdk_storage_info (struct sdk_storage *st) {
    const char *d;

    if (mount () < 0) {
        return -1;
    }
    d = (const char *) ufs_dev;
    memcpy (st->vendor, d + 24, 40);
    st->vendor[40] = 0;
    memcpy (st->product, d + 65, 20);
    st->product[20] = 0;
    memcpy (st->revision, d + 86, 8);
    st->revision[8] = 0;
    st->blocks = *(const u32 *) (d + 16);
    st->block_size = *(const u32 *) (d + 20);
    st->fat_bits = ufs_fat32 ? 32 : 16;
    st->cluster_bytes = ufs_clus_bytes;
    return 0;
}

/* ---- folders ---- */

#define MAX_DIRS 4
static struct udir dirs[MAX_DIRS];
static int dir_used[MAX_DIRS];

int sdk_dir_open (const char *path) {
    char full[SDK_PATH_MAX];
    int h;

    if (mount () < 0) {
        return -1;
    }
    for (h = 0; h < MAX_DIRS && dir_used[h]; h++) {
    }
    if (h == MAX_DIRS) {
        return -1;
    }
    sdk_resolve (path, full);
    if (ufs_opendir (&dirs[h], full) < 0) {
        lost_check ();
        return -1;
    }
    dir_used[h] = 1;
    return h;
}

int sdk_dir_read (int h, struct sdk_dirent *e) {
    struct udirent u;

    if (h < 0 || h >= MAX_DIRS || !dir_used[h] || !ufs_readdir (&dirs[h], &u)) {
        lost_check ();
        return 0;
    }
    copy_arg (e->name, u.name);
    e->is_dir = u.is_dir;
    e->size = u.size;
    return 1;
}

void sdk_dir_close (int h) {
    if (h >= 0 && h < MAX_DIRS) {
        dir_used[h] = 0;
    }
}

/* ---- reboot ---- */

/* Hardware reset through the watchdog block at 0xbf100100, the same
 * register sequence as U-Boot's reset command (its _machine_restart
 * helper at link 0x80104a64). The box then boots from flash as at
 * power-on. */
void sdk_reboot (void) {
    REG32 (0xbf100104) = 0x12345678;
    REG32 (0xbf100108) = 0;
    REG32 (0xbf100100) = 0;
    REG32 (0xbf100108) = 1;
    REG32 (0xbf100104) = 0;
    for (;;) {
        ub_udelay (1000);
    }
}

/* ---- launcher helpers ---- */

/* Make freshly loaded code visible to instruction fetch */
void sdk_cache_sync (u32 start, u32 len) {
    u32 a;

    for (a = start & ~31u; a < start + len; a += 32) {
        __asm__ volatile (
            "cache 0x15, 0(%0)\n\t"     /* D: hit writeback invalidate */
            "cache 0x10, 0(%0)"         /* I: hit invalidate */
            : : "r" (a) : "memory");
    }
    __asm__ volatile ("sync" : : : "memory");
}

/* ---- input: remote + serial as buttons ---- */

static const struct { unsigned char ir; short btn; } ir_btn[] = {
    { IR_KEY_UP, BTN_UP }, { IR_KEY_DOWN, BTN_DOWN }, { IR_KEY_LEFT, BTN_LEFT },
    { IR_KEY_RIGHT, BTN_RIGHT }, { IR_KEY_OK, BTN_OK }, { IR_KEY_EXIT, BTN_BACK },
    { IR_KEY_HOME, BTN_HOME }, { IR_KEY_SETTINGS, BTN_MENU }, { IR_KEY_INFO, BTN_INFO },
    { IR_KEY_POWER, BTN_POWER }, { IR_KEY_RED, BTN_RED }, { IR_KEY_GREEN, BTN_GREEN },
    { IR_KEY_YELLOW, BTN_YELLOW }, { IR_KEY_BLUE, BTN_BLUE }, { IR_KEY_PLAY, BTN_PLAY },
    { IR_KEY_PAUSE, BTN_PAUSE }, { IR_KEY_STOP, BTN_STOP }, { IR_KEY_NEXT, BTN_NEXT },
    { IR_KEY_MUTE, BTN_MUTE }, { IR_KEY_0, BTN_0 }, { IR_KEY_1, BTN_1 }, { IR_KEY_2, BTN_2 },
    { IR_KEY_3, BTN_3 }, { IR_KEY_4, BTN_4 }, { IR_KEY_5, BTN_5 }, { IR_KEY_6, BTN_6 },
    { IR_KEY_7, BTN_7 }, { IR_KEY_8, BTN_8 }, { IR_KEY_9, BTN_9 },
};

/* ---- idle time + performance overlay ---- */

int sdk_overlay_on;
static u32 idle_ticks;

static inline u32 cp0_count (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

void sdk_idle (u32 us) {
    u32 t0 = cp0_count ();

    ub_udelay (us);
    idle_ticks += cp0_count () - t0;
    sdk_overlay_tick ();
}

extern size_t heap_in_use, heap_total;

#define OVL_H       18
#define OVL_BG      RGB (8, 8, 16)
#define OVL_TICKS   (324000u * 500)             /* 0.5 s of CP0 Count */

static void ovl_bar (struct fb *f, int x, int w, int pct, u16 color) {
    int fill = w * (pct > 100 ? 100 : pct) / 100;

    fb_rect (f, x, 4, fill, OVL_H - 8, color);
    fb_rect (f, x + fill, 4, w - fill, OVL_H - 8, RGB (40, 40, 60));
}

void sdk_overlay_tick (void) {
    static u32 last, last_idle, last_usb;
    static int was_on;
    struct fb f;
    u32 now = cp0_count (), wall, idle, usb, heap_kb;
    int cpu, mem;
    char line[64];

    if (!sdk_overlay_on && !was_on) {
        return;
    }
    wall = now - last;
    if (wall < OVL_TICKS && sdk_overlay_on == was_on) {
        return;
    }
    /* Only draw if the OSD layer is ours (osd_setup ran) */
    if (REG32 (0xbf441028) != OSD_HDR_PHYS >> 3) {
        return;
    }
    f.pix = (volatile u16 *) (0xa0000000u | OSD_PIX_PHYS);
    f.w = 1280;
    f.h = 720;
    f.pitch = 1280;
    if (!sdk_overlay_on) {                      /* switched off: clear the strip once */
        fb_rect (&f, 0, 0, f.w, OVL_H, TRANSPARENT);
        was_on = 0;
        return;
    }
    idle = idle_ticks - last_idle;
    usb = ufs_bytes - last_usb;
    cpu = wall ? 100 - (int) ((unsigned long long) idle * 100 / wall) : 0;
    cpu = cpu < 0 ? 0 : cpu;
    heap_kb = heap_in_use / 1024;
    mem = heap_total ? (int) ((unsigned long long) heap_in_use * 100 / heap_total) : 0;
    last = now;
    last_idle = idle_ticks;
    last_usb = ufs_bytes;
    was_on = 1;

    fb_rect (&f, 0, 0, f.w, OVL_H, OVL_BG);
    snprintf (line, sizeof (line), "CPU %3d%%", cpu);
    fb_text (&f, 8, 1, line, 1, WHITE, TRANSPARENT);
    ovl_bar (&f, 80, 160, cpu, cpu > 85 ? RED : cpu > 60 ? YELLOW : GREEN);
    snprintf (line, sizeof (line), "MEM %d.%d/%dM", heap_kb / 1024, heap_kb * 10 / 1024 % 10,
              (int) (heap_total >> 20));
    fb_text (&f, 256, 1, line, 1, WHITE, TRANSPARENT);
    ovl_bar (&f, 360, 160, mem, mem > 85 ? RED : CYAN);
    snprintf (line, sizeof (line), "USB %4dK/s",
              (int) ((unsigned long long) usb * 324000000u / (wall ? wall : 1) / 1024));
    fb_text (&f, 536, 1, line, 1, GREY, TRANSPARENT);
    if ((AUD_REG (0x00) & 0xfff) == 0x305) {     /* audio playing: how much is queued */
        u32 q = ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME;

        snprintf (line, sizeof (line), "AUDIO %3dms", q / 48);
        fb_text (&f, 640, 1, line, 1, q < 2400 ? YELLOW : GREY, TRANSPARENT);
    }
    {
        u32 up = ub_get_timer (0) / 1000;

        snprintf (line, sizeof (line), "UP %d:%02d:%02d", up / 3600, up / 60 % 60, up % 60);
        fb_text (&f, 752, 1, line, 1, GREY, TRANSPARENT);
    }
    snprintf (line, sizeof (line), "%.30s  MUTE hides",
              sdk_app_dir[0] ? (strrchr (sdk_app_dir, '/') ? strrchr (sdk_app_dir, '/') + 1 :
                                sdk_app_dir) : "(U-Boot go)");
    fb_text (&f, 872, 1, line, 1, WHITE, TRANSPARENT);
}

static int input_ready;
static u32 press_ms;                /* when the held remote button went down */

/* A short press on the stock remote sends a frame and, ~110 ms later, a
 * repeat code, which lists took as a second press ("bounce"). Repeats only
 * count once the button has been held this long; then they auto-repeat. */
#define KEY_REPEAT_DELAY_MS 400

int sdk_key_poll (struct sdk_key *k) {
    struct ir_event ev;

    sdk_overlay_tick ();

    if (!input_ready) {
        ir_init ();
        input_ready = 1;
    }
    k->btn = BTN_NONE;
    k->ch = 0;
    k->repeat = 0;
    k->remote = 0;

    if (ir_poll (&ev) && ev.user == IR_USER_STOCK) {
        unsigned int i;

        for (i = 0; i < sizeof (ir_btn) / sizeof (ir_btn[0]); i++) {
            if (ir_btn[i].ir == ev.key) {
                u32 now = ub_get_timer (0);

                if (!ev.repeat) {
                    press_ms = now;
                } else if (now - press_ms < KEY_REPEAT_DELAY_MS) {
                    return 0;               /* the repeat right after a tap */
                }
                if (ir_btn[i].btn == BTN_MUTE) {
                    if (!ev.repeat) {
                        sdk_overlay_on = !sdk_overlay_on;    /* MUTE: overlay on / off */
                        sdk_overlay_tick ();
                    }
                    return 0;
                }
                k->btn = ir_btn[i].btn;
                k->repeat = ev.repeat;
                k->remote = 1;
                return 1;
            }
        }
        return 0;
    }
    if (ub_tstc ()) {
        int c = ub_getc ();

        k->ch = c;
        if (c == 27) {                  /* ESC [ A-D = arrows, ESC alone = back */
            u32 t = ub_get_timer (0);

            while (!ub_tstc () && ub_get_timer (t) < 5) {
            }
            if (ub_tstc () && ub_getc () == '[') {
                while (!ub_tstc () && ub_get_timer (t) < 10) {
                }
                c = ub_tstc () ? ub_getc () : 0;
                k->btn = c == 'A' ? BTN_UP : c == 'B' ? BTN_DOWN :
                         c == 'C' ? BTN_RIGHT : c == 'D' ? BTN_LEFT : BTN_NONE;
                k->ch = 0;
            } else {
                k->btn = BTN_BACK;
            }
        } else if (c == '\r' || c == '\n' || c == ' ') {
            k->btn = BTN_OK;
        } else if (c == 8 || c == 127) {
            k->btn = BTN_BACK;
        } else if (c >= '0' && c <= '9') {
            k->btn = BTN_0 + (c - '0');
        } else if (c == 'w' || c == 'k') {
            k->btn = BTN_UP;
        } else if (c == 's' || c == 'j') {
            k->btn = BTN_DOWN;
        } else if (c == 'a' || c == 'h') {
            k->btn = BTN_LEFT;
        } else if (c == 'd' || c == 'l') {
            k->btn = BTN_RIGHT;
        } else if (c == 'm') {
            k->btn = BTN_MENU;
        }
        return 1;
    }
    return 0;
}

const char *sdk_btn_name (int btn) {
    static const char *names[] = {
        "NONE", "UP", "DOWN", "LEFT", "RIGHT", "OK", "BACK", "HOME", "MENU", "INFO", "POWER",
        "RED", "GREEN", "YELLOW", "BLUE", "PLAY", "PAUSE", "STOP", "NEXT", "MUTE",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    };

    return (btn >= 0 && btn < (int) (sizeof (names) / sizeof (names[0]))) ? names[btn] : "?";
}
