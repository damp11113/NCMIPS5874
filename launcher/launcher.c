/*
 * NCAPPS launcher: app menu for the NC5874 box, started from flash by the
 * boot script (fatload /NCAPPS/LAUNCHER.BIN to 0x80800000, go). Returning
 * from main () lets the script boot the stock firmware.
 *
 * Stick layout:
 *   NCAPPS/LAUNCHER.BIN        this program (runs at 0x80800000)
 *   NCAPPS/LAUNCHER.INI        optional: autostart=<entry name>, timeout=<s>
 *   NCAPPS/APPS/<folder>/      one folder per app: APP.BIN, its files, and
 *                              one or more *.INF menu entries
 *   NCAPPS/APPSDATA/<folder>/  the app's writable space (sdk_data_dir)
 *
 * *.INF (key=value lines, # comments):
 *   name=Doom                  menu name (default: the file name)
 *   desc=...                   description, word-wrapped on screen
 *   bin=APP.BIN                binary in the app folder (default APP.BIN)
 *   args=-iwad doom.wad        arguments, split at spaces
 *   version=1.9
 *   icon=ICON.RAW              64x64 ARGB1555 little-endian (8192 bytes)
 *   abi=uboot                  a plain U-Boot 'go' program (not built with the
 *                              SDK): argv = "launcher", then the arguments
 *
 * SETTINGS: about page (credits, project page) with the performance
 * overlay switch, which is passed to SDK apps ("@ovl=1"); MUTE toggles the
 * overlay in any SDK app.
 *
 * Keys: UP / DOWN select, OK start, INFO details on serial, EXIT = leave
 * to the stock firmware, POWER = standby (screen off, red LED; POWER or
 * the STANDBY button again restarts the box). STANDBY held at start-up =
 * leave to the stock firmware at once.
 * Serial: arrows / w s, Enter, Esc.
 *
 * Starting an app: the binary is read to 0x80008000, the caches are
 * synced, and it is called like U-Boot's go does, with argv =
 * "@app=<folder>", "@data=<data folder>", then the INF arguments (the SDK
 * runtime turns these into sdk_app_dir / sdk_data_dir). Apps use the same
 * heap RAM as the launcher, so everything the menu needs lives in static
 * arrays and the C library is reset when the app returns.
 *
 * Crash screen: while the launcher runs, CP0 EBase points at its own
 * vectors (crash_entry.S). An app that crashes (bad pointer, bad
 * instruction, address error, ...) gets a screen with the exception, PC,
 * address and registers instead of freezing the box; OK goes back to the
 * menu, POWER restarts. Interrupts are passed on to U-Boot's vectors, and
 * U-Boot's EBase is put back before the stock firmware is started.
 *
 * Build: LOAD=0x80800000 MAX_END=0x80a00000 sh sdk/build.sh LAUNCHER.BIN
 *        launcher/launcher.c launcher/crash_entry.S
 */
#define BOX_WANT_AUDIO
#include "sdk.h"

#define APPS_DIR        "/NCAPPS/APPS"
#define DATA_DIR        "/NCAPPS/APPSDATA"
#define INI_PATH        "/NCAPPS/LAUNCHER.INI"
#define MAX_ENTRIES     64
#define ICON_W          64
#define ICON_BYTES      (ICON_W * ICON_W * 2)

/* Layout (1280x720 OSD) */
#define BG              RGB (12, 18, 40)
#define PANEL           RGB (24, 34, 72)
#define HILITE          RGB (60, 110, 200)
#define LIST_X          60
#define LIST_Y          130
#define LIST_W          460
#define ROW_H           40
#define ROWS            12
#define INFO_X          560
#define INFO_W          660

struct entry {
    char name[48];
    char folder[48];                /* folder name inside APPS */
    char desc[400];
    char bin[32];
    char args[160];
    char version[24];
    char icon[32];
    char inf[32];
    int uboot_abi;
};

static struct entry entries[MAX_ENTRIES];
static int count, sel, top;
static struct fb fb;
static char autostart[48];
static int autostart_s = 3;
static unsigned short icon_buf[ICON_W * ICON_W];

/* ---- INF parsing ---- */

static void trim (char *s) {
    char *e = s + strlen (s);

    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
}

static void set_field (char *dst, int size, const char *val) {
    strncpy (dst, val, size - 1);
    dst[size - 1] = 0;
}

/* Calls fn (key, value) for each "key=value" line of a text file */
static int read_kv (const char *path, void (*fn) (void *ctx, const char *k, const char *v),
                    void *ctx) {
    char line[512];
    FILE *f = fopen (path, "r");

    if (!f) {
        return -1;
    }
    while (fgets (line, sizeof (line), f)) {
        char *eq, *k = line, *v;

        while (*k == ' ' || *k == '\t') {
            k++;
        }
        if (*k == '#' || *k == ';' || !(eq = strchr (k, '='))) {
            continue;
        }
        *eq = 0;
        v = eq + 1;
        trim (k);
        trim (v);
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        fn (ctx, k, v);
    }
    fclose (f);
    return 0;
}

static void inf_field (void *ctx, const char *k, const char *v) {
    struct entry *e = ctx;

    if (!strcasecmp (k, "name")) {
        set_field (e->name, sizeof (e->name), v);
    } else if (!strcasecmp (k, "desc")) {
        set_field (e->desc, sizeof (e->desc), v);
    } else if (!strcasecmp (k, "bin")) {
        set_field (e->bin, sizeof (e->bin), v);
    } else if (!strcasecmp (k, "args")) {
        set_field (e->args, sizeof (e->args), v);
    } else if (!strcasecmp (k, "version")) {
        set_field (e->version, sizeof (e->version), v);
    } else if (!strcasecmp (k, "abi")) {
        e->uboot_abi = !strcasecmp (v, "uboot");
    } else if (!strcasecmp (k, "icon")) {
        set_field (e->icon, sizeof (e->icon), v);
    }
}

static void ini_field (void *ctx, const char *k, const char *v) {
    (void) ctx;
    if (!strcasecmp (k, "autostart")) {
        set_field (autostart, sizeof (autostart), v);
    } else if (!strcasecmp (k, "timeout")) {
        autostart_s = atoi (v);
    }
}

static int ends_with_inf (const char *n) {
    int len = strlen (n);

    return len > 4 && !strcasecmp (n + len - 4, ".inf");
}

/* ---- start screen (shown while the INF files are read) ---- */

static int splash_on;

static void splash (void) {
    fb_clear (&fb, BG);
    fb_rect (&fb, 0, 250, fb.w, 220, PANEL);
    fb_text (&fb, (fb.w - 6 * 64) / 2, 280, "NCAPPS", 8, WHITE, TRANSPARENT);
    fb_text (&fb, (fb.w - 11 * 16) / 2, 420, "Starting...", 2, GREY, TRANSPARENT);
    splash_on = 1;
}

static void splash_status (const char *msg) {
    char line[64];
    int n;

    if (!splash_on) {
        return;
    }
    n = snprintf (line, sizeof (line), "%-30s", msg);
    fb_text (&fb, (fb.w - n * 16) / 2, 520, line, 2, CYAN, BG);
}

static void scan (void) {
    struct sdk_dirent d, f;
    int h, i, j;

    count = 0;
    h = sdk_dir_open (APPS_DIR);
    if (h < 0) {
        printf ("launcher: no %s folder\n", APPS_DIR);
        return;
    }
    while (sdk_dir_read (h, &d) && count < MAX_ENTRIES) {
        char path[SDK_PATH_MAX];
        int h2;

        if (!d.is_dir) {
            continue;
        }
        snprintf (path, sizeof (path), "%s/%s", APPS_DIR, d.name);
        h2 = sdk_dir_open (path);
        if (h2 < 0) {
            continue;
        }
        while (sdk_dir_read (h2, &f) && count < MAX_ENTRIES) {
            struct entry *e = &entries[count];
            char inf[SDK_PATH_MAX];

            if (f.is_dir || !ends_with_inf (f.name)) {
                continue;
            }
            memset (e, 0, sizeof (*e));
            set_field (e->folder, sizeof (e->folder), d.name);
            set_field (e->inf, sizeof (e->inf), f.name);
            set_field (e->bin, sizeof (e->bin), "APP.BIN");
            snprintf (e->name, sizeof (e->name), "%s", f.name);
            e->name[strlen (e->name) - 4] = 0;
            snprintf (inf, sizeof (inf), "%s/%s", path, f.name);
            if (read_kv (inf, inf_field, e) == 0) {
                char msg[64];

                count++;
                snprintf (msg, sizeof (msg), "Loading apps... %d", count);
                splash_status (msg);
            }
        }
        sdk_dir_close (h2);
    }
    sdk_dir_close (h);

    for (i = 1; i < count; i++) {                   /* sort by name */
        for (j = i; j > 0 && strcasecmp (entries[j - 1].name, entries[j].name) > 0; j--) {
            struct entry t = entries[j];

            entries[j] = entries[j - 1];
            entries[j - 1] = t;
        }
    }
    printf ("launcher: %d app entries\n", count);
}

/* ---- drawing ---- */

static void text (int x, int y, const char *s, int scale, u16 fg, u16 bg, int max_chars) {
    char buf[128];
    int n = strlen (s);

    if (n > max_chars) {
        n = max_chars;
    }
    if (n > (int) sizeof (buf) - 1) {
        n = sizeof (buf) - 1;
    }
    memcpy (buf, s, n);
    buf[n] = 0;
    fb_text (&fb, x, y, buf, scale, fg, bg);
}

/* Word-wrapped text; returns the y below it */
static int wrap (int x, int y, const char *s, int scale, u16 fg, int cols, int max_lines) {
    char line[128];

    while (*s && max_lines-- > 0) {
        int n = strlen (s), cut;

        if (n > cols) {
            for (cut = cols; cut > 0 && s[cut] != ' '; cut--) {
            }
            n = cut > 0 ? cut : cols;
        }
        memcpy (line, s, n);
        line[n] = 0;
        fb_text (&fb, x, y, line, scale, fg, TRANSPARENT);
        y += 18 * scale;
        s += n;
        while (*s == ' ') {
            s++;
        }
    }
    return y;
}

static void draw_icon (const struct entry *e, int x, int y) {
    char path[SDK_PATH_MAX];
    int i, j;

    fb_rect (&fb, x, y, ICON_W * 2, ICON_W * 2, PANEL);
    if (!e->icon[0]) {
        text (x + 40, y + 44, e->name, 5, GREY, TRANSPARENT, 1);    /* initial */
        return;
    }
    snprintf (path, sizeof (path), "%s/%s/%s", APPS_DIR, e->folder, e->icon);
    if (sdk_read_file (path, icon_buf, ICON_BYTES) != ICON_BYTES) {
        return;
    }
    for (j = 0; j < ICON_W; j++) {                  /* 2x */
        for (i = 0; i < ICON_W; i++) {
            u16 c = icon_buf[j * ICON_W + i];

            if (c & 0x8000) {
                fb_rect (&fb, x + 2 * i, y + 2 * j, 2, 2, c);
            }
        }
    }
}

static void draw_header (void) {
    fb_rect (&fb, 0, 0, fb.w, 100, PANEL);
    fb_text (&fb, LIST_X, 30, "NCAPPS", 3, WHITE, TRANSPARENT);
    fb_text (&fb, LIST_X + 170, 44, "app launcher", 2, GREY, TRANSPARENT);
    fb_rect (&fb, 0, 660, fb.w, 60, PANEL);
    fb_text (&fb, LIST_X, 680, "OK start  UP/DOWN select  SETTINGS about  EXIT firmware  POWER off",
             2, GREY, TRANSPARENT);
}

static void draw_list (void) {
    int i;

    fb_rect (&fb, LIST_X - 10, LIST_Y - 10, LIST_W + 20, ROWS * ROW_H + 20, BG);
    if (count == 0) {
        fb_text (&fb, LIST_X, LIST_Y, "No apps in /NCAPPS/APPS", 2, YELLOW, TRANSPARENT);
        return;
    }
    for (i = 0; i < ROWS && top + i < count; i++) {
        int y = LIST_Y + i * ROW_H;
        int on = top + i == sel;

        if (on) {
            fb_rect (&fb, LIST_X - 10, y - 6, LIST_W + 20, ROW_H - 4, HILITE);
        }
        text (LIST_X, y, entries[top + i].name, 2, on ? WHITE : RGB (200, 210, 230),
              TRANSPARENT, LIST_W / 16);
    }
    if (top > 0) {
        fb_text (&fb, LIST_X + LIST_W - 20, LIST_Y - 30, "^", 2, GREY, TRANSPARENT);
    }
    if (top + ROWS < count) {
        fb_text (&fb, LIST_X + LIST_W - 20, LIST_Y + ROWS * ROW_H, "v", 2, GREY, TRANSPARENT);
    }
}

static void draw_info (void) {
    const struct entry *e;
    char line[160];
    int y;

    fb_rect (&fb, INFO_X - 20, LIST_Y - 10, INFO_W + 40, 530, BG);
    if (count == 0) {
        return;
    }
    e = &entries[sel];
    draw_icon (e, INFO_X, LIST_Y);
    y = LIST_Y + 10;
    text (INFO_X + 150, y, e->name, 3, WHITE, TRANSPARENT, (INFO_W - 150) / 24);
    if (e->version[0]) {
        snprintf (line, sizeof (line), "version %s", e->version);
        text (INFO_X + 150, y + 60, line, 2, GREY, TRANSPARENT, 40);
    }
    y = wrap (INFO_X, LIST_Y + 160, e->desc[0] ? e->desc : "(no description)", 2,
              RGB (220, 225, 235), INFO_W / 16, 12);
    snprintf (line, sizeof (line), "%s/%s", e->folder, e->bin);
    text (INFO_X, 610, line, 1, GREY, TRANSPARENT, 80);
    if (e->args[0]) {
        snprintf (line, sizeof (line), "args: %s", e->args);
        text (INFO_X, 628, line, 1, GREY, TRANSPARENT, 80);
    }
}

static void draw_all (void) {
    fb_clear (&fb, BG);
    draw_header ();
    draw_list ();
    draw_info ();
}

static void message (const char *msg, u16 color) {
    fb_rect (&fb, 0, 300, fb.w, 120, PANEL);
    fb_text (&fb, (fb.w - (int) strlen (msg) * 24) / 2, 340, msg, 3, color, TRANSPARENT);
}

/* ---- crash screen ---- */

struct frame {
    u32 r[32];
    u32 hi, lo, epc, status, cause, badvaddr;
};

extern char crash_stack_top[];
extern char crash_stub_refill[], crash_stub_refill_end[];
extern char crash_stub_general[], crash_stub_general_end[];
static u32 vec_page[1024] __attribute__ ((aligned (4096)));     /* our EBase */
u32 uboot_ebase;                    /* U-Boot's vectors: interrupts go there */
static u32 uboot_status, gd_value;
static void *launch_jb[5];
static volatile int in_app, in_crash;
static struct frame crash;
static char crash_app[48];

#define mfc0(reg, sel) ({ u32 __v; \
    __asm__ volatile ("mfc0 %0, $" #reg ", " #sel : "=r" (__v)); __v; })
#define mtc0(reg, sel, v) \
    __asm__ volatile ("mtc0 %0, $" #reg ", " #sel "\n\tehb" : : "r" ((u32) (v)) : "memory")

#define ST_IE   (1u << 0)
#define ST_EXL  (1u << 1)
#define ST_ERL  (1u << 2)
#define ST_KSU  (3u << 3)
#define ST_TS   (1u << 21)
#define ST_BEV  (1u << 22)

static const char *exc_name (u32 code) {
    static const char *names[32] = {
        "Interrupt", "TLB modified (write to read-only page)", "TLB miss on load / fetch",
        "TLB miss on store", "Address error on load / fetch", "Address error on store",
        "Bus error on fetch", "Bus error on load / store", "Syscall", "Breakpoint",
        "Reserved instruction", "Coprocessor unusable (FPU code?)", "Integer overflow",
        "Trap", "?", "Floating point", "?", "?", "Coprocessor 2", "?", "?", "?", "MDMX",
        "Watch", "Machine check (TLB)", "Thread", "DSP disabled", "?", "?", "?", "Cache error",
        "?",
    };

    return names[code & 31];
}

static const char *where (u32 a, u32 *off) {
    if (a >= SDK_APP_ADDR && a < SDK_APP_END) {
        *off = a - SDK_APP_ADDR;
        return "app";
    }
    if (a >= SDK_LAUNCHER_ADDR && a < SDK_LAUNCHER_ADDR + 0x200000) {
        *off = a - SDK_LAUNCHER_ADDR;
        return "launcher";
    }
    if (a >= 0x81300000 && a < 0x81600000) {
        *off = a - 0x81300000;
        return "U-Boot";
    }
    *off = 0;
    return "?";
}

/* TLB: U-Boot leaves power-on garbage (random, even duplicate entries), so
 * a NULL pointer might hit a random mapping instead of faulting. Rewrite
 * all 64 as invalid entries with kseg0 tags no other entry covers (see
 * mmutest.c). U-Boot does not use the TLB. */
static void tlb_clean (void) {
    static u32 tag[64], span[64];
    int i, j;

    for (i = 0; i < 64; i++) {
        mtc0 (0, 0, i);
        __asm__ volatile ("tlbr\n\tehb" : : : "memory");
        tag[i] = mfc0 (10, 0) & ~0x1fffu;
        span[i] = ~(mfc0 (5, 0) | 0x1fffu);
    }
    for (i = 0; i < 64; i++) {
        u32 t = 0x80000000u + (u32) i * 0x2000u;

        for (j = 0; j < 64; j++) {
            if (j != i && ((t ^ tag[j]) & span[j]) == 0) {
                t += 0x80000u;
                j = -1;
            }
        }
        mtc0 (0, 0, i);
        mtc0 (10, 0, t);
        mtc0 (2, 0, 0);
        mtc0 (3, 0, 0);
        mtc0 (5, 0, 0);
        __asm__ volatile ("tlbwi\n\tehb" : : : "memory");
        tag[i] = t;
        span[i] = ~0x1fffu;
    }
    mtc0 (6, 0, 0);
    mtc0 (10, 0, 0);
}

/* EBase may only change while Status.BEV = 1 (24K manual) */
static void set_ebase (u32 base) {
    u32 st = mfc0 (12, 0);

    mtc0 (12, 0, st & ~ST_IE);
    mtc0 (12, 0, (st & ~ST_IE) | ST_BEV);
    mtc0 (15, 1, base);
    mtc0 (12, 0, st & ~ST_BEV);
}

/* A PC-relative branch in U-Boot's vector page that leaves the page would
 * go astray in a copy. Returns the offset of the first one, or -1. */
static int page_branches_out (const u32 *page) {
    int i;

    for (i = 0; i < 1024; i++) {
        u32 w = page[i], op = w >> 26;
        int target;

        if (!(op == 1 || (op >= 4 && op <= 7) || (op >= 20 && op <= 23)) || w == 0xffffffffu) {
            continue;
        }
        target = i * 4 + 4 + ((int) (short) (w & 0xffff) << 2);
        if (target < 0 || target >= 4096) {
            return i * 4;
        }
    }
    return -1;
}

/* Copy U-Boot's vector page, put our stubs at +0x000 and +0x180 */
static void crash_install (void) {
    const u32 *ub;
    int bad;

    uboot_ebase = mfc0 (15, 1);
    uboot_status = mfc0 (12, 0);
    __asm__ volatile ("move %0, $26" : "=r" (gd_value));
    ub = (const u32 *) uboot_ebase;
    printf ("launcher: U-Boot EBase 0x%08x, Status 0x%08x, Cause 0x%08x, IntCtl 0x%08x\n",
            uboot_ebase, uboot_status, mfc0 (13, 0), mfc0 (12, 1));
    printf ("  +180: %08x %08x %08x %08x  +200: %08x %08x %08x %08x\n", ub[0x60], ub[0x61],
            ub[0x62], ub[0x63], ub[0x80], ub[0x81], ub[0x82], ub[0x83]);
    bad = page_branches_out (ub);
    if (bad >= 0) {
        printf ("launcher: crash handler OFF: U-Boot vector page branches out at +0x%03x\n", bad);
        return;
    }
    memcpy (vec_page, (const void *) uboot_ebase, sizeof (vec_page));
    memcpy ((char *) vec_page + 0x000, crash_stub_refill,
            crash_stub_refill_end - crash_stub_refill);
    memcpy ((char *) vec_page + 0x180, crash_stub_general,
            crash_stub_general_end - crash_stub_general);
    sdk_cache_sync ((u32) vec_page, sizeof (vec_page));
    printf ("launcher: crash handler on (U-Boot vectors 0x%08x copied to 0x%08x)\n",
            uboot_ebase, (u32) vec_page);
    tlb_clean ();
    set_ebase ((u32) vec_page);
}

static void crash_uninstall (void) {
    set_ebase (uboot_ebase);
    mtc0 (12, 0, uboot_status);
}

static void crash_resume (void);

/* Exception context (EXL = 1): keep a copy, continue in crash_resume () */
void crash_handler (struct frame *f) {
    if (in_crash) {
        printf ("\nlauncher: exception while showing a crash (%s at 0x%08x), halted\n",
                exc_name ((f->cause >> 2) & 31), f->epc);
        for (;;) {
        }
    }
    in_crash = 1;
    crash = *f;
    f->epc = (u32) crash_resume;
    f->status &= ~(ST_KSU | ST_TS);
    f->r[29] = (u32) crash_stack_top - 64;          /* the app's stack may be the problem */
    f->r[26] = gd_value;                            /* U-Boot's gd for its calls */
}

static void crash_line (int x, int y, u16 color, const char *fmt, ...) __attribute__ ((format (printf, 4, 5)));

static void crash_line (int x, int y, u16 color, const char *fmt, ...) {
    char line[120];
    va_list ap;

    va_start (ap, fmt);
    vsnprintf (line, sizeof (line), fmt, ap);
    va_end (ap);
    printf ("%s\n", line);
    if (fb.pix) {
        fb_text (&fb, x, y, line, x < 100 ? 2 : 1, color, TRANSPARENT);
    }
}

static void crash_resume (void) {
    static const char *rn[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5",
        "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1",
        "gp", "sp", "s8", "ra",
    };
    u32 code = (crash.cause >> 2) & 31, off, roff;
    const char *w = where (crash.epc, &off), *rw = where (crash.r[31], &roff);
    struct sdk_key k;
    int i;

    audio_stop ();
    if (osd_setup (&fb) == 0) {
        fb_clear (&fb, RGB (70, 10, 10));
        fb_rect (&fb, 0, 0, fb.w, 100, RGB (110, 20, 20));
    }
    printf ("\n");
    crash_line (60, 24, WHITE, "%s crashed", in_app ? crash_app : "The launcher");
    crash_line (60, 120, YELLOW, "%s (exception %d)", exc_name (code), code);
    crash_line (60, 160, WHITE, "at 0x%08x = %s + 0x%x%s", crash.epc, w, off,
                (crash.cause & 0x80000000u) ? " (branch delay slot)" : "");
    if (code >= 1 && code <= 7) {
        crash_line (60, 200, WHITE, "address 0x%08x%s", crash.badvaddr,
                    crash.badvaddr < 0x1000 ? " (NULL pointer?)" : "");
    }
    crash_line (60, 240, WHITE, "called from 0x%08x = %s + 0x%x", crash.r[31], rw, roff);
    for (i = 0; i < 32; i++) {
        crash_line (60 + (i % 4) * 300, 300 + (i / 4) * 24, RGB (230, 200, 200), "%-4s %08x",
                    rn[i], i == 0 ? 0 : crash.r[i]);
    }
    crash_line (60, 500, RGB (230, 200, 200), "Status %08x  Cause %08x  HI %08x  LO %08x",
                crash.status, crash.cause, crash.hi, crash.lo);
    crash_line (60, 620, WHITE, "OK: back to the menu    POWER: restart the box");

    while (sdk_key_poll (&k)) {
    }
    for (;;) {
        if (sdk_key_poll (&k) && !k.repeat) {
            if (k.btn == BTN_POWER) {
                sdk_reboot ();
            }
            if (k.btn == BTN_OK || k.btn == BTN_BACK) {
                break;
            }
        }
        if (standby_pressed ()) {
            sdk_reboot ();
        }
    }
    in_crash = 0;
    if (in_app) {
        __builtin_longjmp (launch_jb, 1);           /* back into launch () */
    }
    sdk_reboot ();                                  /* the launcher itself is broken */
}

/* ---- launching ---- */

typedef int (*app_entry_t) (int argc, char *argv[]);

/* Split at spaces, in place: strtok_simple (buf), then strtok_simple (0) */
static char *strtok_simple (char *s) {
    static char *next;
    char *start;

    if (s) {
        next = s;
    }
    while (next && *next == ' ') {
        next++;
    }
    if (!next || !*next) {
        return 0;
    }
    start = next;
    while (*next && *next != ' ') {
        next++;
    }
    if (*next) {
        *next++ = 0;
    }
    return start;
}

static int launch (const struct entry *e) {
    static char app_arg[SDK_PATH_MAX + 8], data_arg[SDK_PATH_MAX + 8], args[160];
    static char *argv[24];                          /* static: survive the longjmp */
    static int rc;
    char path[SDK_PATH_MAX];
    long size, max = SDK_APP_END - SDK_APP_ADDR;
    int argc = 0;
    char *p;

    snprintf (path, sizeof (path), "%s/%s/%s", APPS_DIR, e->folder, e->bin);
    size = sdk_file_size (path);
    if (size <= 0 || size > max) {
        printf ("launcher: %s: %s\n", path, size <= 0 ? "not found" : "too big");
        message (size <= 0 ? "APP.BIN not found" : "App too big", RED);
        ub_udelay (1500000);
        return -1;
    }
    {
        char msg[80];

        snprintf (msg, sizeof (msg), "Starting %s...", e->name);
        message (msg, WHITE);
    }
    printf ("launcher: starting %s (%s, %ld bytes)\n", e->name, path, size);
    if (sdk_read_file (path, (void *) SDK_APP_ADDR, size) != size) {
        message ("Read error", RED);
        ub_udelay (1500000);
        return -1;
    }
    sdk_cache_sync (SDK_APP_ADDR, size);

    snprintf (app_arg, sizeof (app_arg), "@app=%s/%s", APPS_DIR + 1, e->folder);
    snprintf (data_arg, sizeof (data_arg), "@data=%s/%s", DATA_DIR + 1, e->folder);
    if (e->uboot_abi) {
        argv[argc++] = "launcher";          /* go's argv[0] is the address */
    } else {
        argv[argc++] = app_arg;
        argv[argc++] = data_arg;
        argv[argc++] = sdk_overlay_on ? "@ovl=1" : "@ovl=0";
    }
    set_field (args, sizeof (args), e->args);
    for (p = strtok_simple (args); p && argc < 23; p = strtok_simple (0)) {
        argv[argc++] = p;
    }
    argv[argc] = 0;

    snprintf (crash_app, sizeof (crash_app), "%s", e->name);
    in_app = 1;
    if (__builtin_setjmp (launch_jb) == 0) {
        rc = ((app_entry_t) SDK_APP_ADDR) (argc, argv);
    } else {
        rc = -1;                                    /* crashed: see crash_resume () */
    }
    in_app = 0;

    /* The app used the same heap RAM and may have changed the screen */
    sdk_libc_reset ();
    printf ("launcher: %s returned %d\n", e->name, rc);
    return rc;
}

/* ---- about / settings ---- */

static void about (void) {
    struct sdk_key k;
    int redraw = 1;

    for (;;) {
        if (redraw) {
            char line[96];

            fb_clear (&fb, BG);
            fb_rect (&fb, 0, 0, fb.w, 100, PANEL);
            fb_text (&fb, LIST_X, 30, "ABOUT", 3, WHITE, TRANSPARENT);
            fb_text (&fb, LIST_X, 140, "NCAPPS", 5, WHITE, TRANSPARENT);
            fb_text (&fb, LIST_X, 230, "App launcher and SDK for the NC5874 set-top box", 2,
                     RGB (220, 225, 235), TRANSPARENT);
            fb_text (&fb, LIST_X, 264, "Montage M88CS8051B, MIPS 24KEc ~648 MHz, 128 MB, no OS",
                     2, GREY, TRANSPARENT);
            fb_text (&fb, LIST_X, 320, "Made by damp11113", 3, YELLOW, TRANSPARENT);
            fb_text (&fb, LIST_X, 380, "github.com/damp11113/NCMIPS5874", 3, CYAN, TRANSPARENT);
            snprintf (line, sizeof (line), "%d app entries    built %s", count, __DATE__);
            fb_text (&fb, LIST_X, 450, line, 2, GREY, TRANSPARENT);

            fb_rect (&fb, LIST_X - 10, 510, 900, 50, HILITE);
            snprintf (line, sizeof (line), "Performance overlay:  %s",
                      sdk_overlay_on ? "ON " : "OFF");
            fb_text (&fb, LIST_X, 520, line, 2, WHITE, TRANSPARENT);
            fb_text (&fb, LIST_X, 580, "CPU / memory / USB / audio bar at the top of the screen.",
                     2, GREY, TRANSPARENT);
            fb_text (&fb, LIST_X, 612, "MUTE on the remote switches it in any app.", 2, GREY,
                     TRANSPARENT);
            fb_rect (&fb, 0, 660, fb.w, 60, PANEL);
            fb_text (&fb, LIST_X, 680, "OK toggle overlay   BACK / SETTINGS close", 2, GREY,
                     TRANSPARENT);
            redraw = 0;
        }
        if (!sdk_key_poll (&k)) {
            sdk_idle (2000);
            continue;
        }
        if (k.repeat) {
            continue;
        }
        if (k.btn == BTN_OK) {
            sdk_overlay_on = !sdk_overlay_on;
            redraw = 1;
        } else if (k.btn == BTN_BACK || k.btn == BTN_MENU || k.btn == BTN_HOME) {
            return;
        }
    }
}

/* ---- standby ---- */

/*
 * Soft power-off: the box has no software power switch (the stock
 * firmware's standby hands over to the always-on MCU with its own
 * firmware), so: OSD off, sound already off, LED red, HDMI PHY held in
 * reset so the TV sees no signal, then wait for POWER on the remote or the
 * STANDBY button and restart the whole box (sdk_reboot).
 */
static void standby (void) {
    struct sdk_key k;

    printf ("launcher: standby (POWER or STANDBY to wake)\n");
    message ("Power off...", WHITE);
    ub_udelay (500000);
    fb_clear (&fb, TRANSPARENT);
    led_green (0);
    led_red (1);
    REG32 (0xbf157000) |= 0x80;             /* HDMI analog reset: no signal */
    while (sdk_key_poll (&k)) {             /* drop the key that got us here */
    }
    while (standby_pressed ()) {
        ub_udelay (10000);
    }
    ub_udelay (300000);
    for (;;) {
        if ((sdk_key_poll (&k) && k.btn == BTN_POWER && !k.repeat) || standby_pressed ()) {
            printf ("launcher: waking up, restarting the box\n");
            sdk_reboot ();
        }
        ub_udelay (20000);
    }
}

/* ---- main ---- */

static int find_entry (const char *name) {
    int i;

    for (i = 0; i < count; i++) {
        if (!strcasecmp (entries[i].name, name) || !strcasecmp (entries[i].folder, name)) {
            return i;
        }
    }
    return -1;
}

/* Autostart countdown: 1 = start it, 0 = a key was pressed */
static int countdown (int idx) {
    struct sdk_key k;
    int s;

    for (s = autostart_s; s > 0; s--) {
        char msg[80];
        u32 t0 = ub_get_timer (0);

        snprintf (msg, sizeof (msg), "%s in %d s - any key: menu", entries[idx].name, s);
        message (msg, WHITE);
        while (ub_get_timer (t0) < 1000) {
            if (sdk_key_poll (&k)) {
                return 0;
            }
        }
    }
    return 1;
}

int main (int argc, char *argv[]) {
    struct sdk_key k;
    int redraw = 1;

    (void) argc;
    (void) argv;
    printf ("NCAPPS launcher\n");
    if (standby_pressed ()) {
        printf ("launcher: STANDBY held, booting the stock firmware\n");
        while (standby_pressed ()) {
            ub_udelay (10000);
        }
        return 0;
    }
    if (osd_setup (&fb) < 0) {
        printf ("launcher: display not running (the boot script must init AV/HDMI)\n");
        return 1;
    }
    crash_install ();
    splash ();
    splash_status ("Reading settings...");

    read_kv (INI_PATH, ini_field, 0);
    scan ();
    splash_on = 0;
    draw_all ();

    if (autostart[0]) {
        int idx = find_entry (autostart);

        if (idx >= 0) {
            sel = idx;
            if (countdown (idx)) {
                launch (&entries[idx]);
                osd_setup (&fb);
            }
        } else {
            printf ("launcher: autostart '%s' not found\n", autostart);
        }
    }

    for (;;) {
        if (redraw) {
            if (sel < top) {
                top = sel;
            } else if (sel >= top + ROWS) {
                top = sel - ROWS + 1;
            }
            draw_all ();
            redraw = 0;
        }
        if (!sdk_key_poll (&k)) {
            sdk_idle (2000);
            continue;
        }
        if (k.btn == BTN_UP && count) {
            sel = (sel + count - 1) % count;
            if (sel >= top && sel < top + ROWS && sel != count - 1) {
                draw_list ();
                draw_info ();
            } else {
                redraw = 1;
            }
        } else if (k.btn == BTN_DOWN && count) {
            sel = (sel + 1) % count;
            if (sel >= top && sel < top + ROWS && sel != 0) {
                draw_list ();
                draw_info ();
            } else {
                redraw = 1;
            }
        } else if (k.btn == BTN_OK && count && !k.repeat) {
            launch (&entries[sel]);
            osd_setup (&fb);                    /* apps may clear or move it */
            while (sdk_key_poll (&k)) {         /* drop keys left from the app */
            }
            redraw = 1;
        } else if (k.btn == BTN_INFO && count) {
            const struct entry *e = &entries[sel];

            printf ("%s: folder %s, inf %s, bin %s, args '%s'\n", e->name, e->folder, e->inf,
                    e->bin, e->args);
        } else if (k.btn == BTN_MENU && !k.repeat) {
            about ();
            redraw = 1;
        } else if (k.btn == BTN_POWER && !k.repeat) {
            standby ();
        } else if (k.btn == BTN_BACK && !k.repeat) {
            message ("Booting the stock firmware...", WHITE);
            fb_clear (&fb, TRANSPARENT);
            crash_uninstall ();                     /* U-Boot's vectors for the firmware */
            return 0;
        }
    }
}
