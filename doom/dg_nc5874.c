/*
 * doomgeneric platform layer for the NC5874 set-top box (bare metal
 * under U-Boot, no OS).
 *
 *   Screen: Doom's 8-bit 320x200 frame (doomgeneric CMAP256) -> palette
 *           LUT -> ARGB1555, scaled 3x wide / 3.6x tall = 960x720 (4:3)
 *           centred on the 1280x720 OSD layer. Only rows that changed
 *           since the last frame are redrawn.
 *   Input:  stock remote (ir.h) and the serial console. Keys are held
 *           while the remote repeats; released IR_RELEASE_MS after the
 *           last frame. Serial keys are pressed and released at once.
 *   Files:  SDK fopen () loads whole files from the USB stick into RAM
 *           (DOOM.WAD, 12.4 MB, takes ~15 s at U-Boot's USB speed); from
 *           the launcher, relative names are inside the app's folder.
 *           Saving is not supported yet. A red bar shows big loads.
 *   Build:  on the SDK (../sdk): runtime, C library, exit / atexit.
 *   Timer:  U-Boot get_timer (ms).
 *   Sound:  dg_sound.c (effects mixer -> audio.h), topped up from here.
 *
 * Remote:  arrows = move/turn, OK = fire (and menu select), RED = use /
 *          open, GREEN / YELLOW = strafe left / right, BLUE = run on/off,
 *          1-7 = weapons, INFO = map, EXIT / HOME = menu, PLAY = yes,
 *          STOP = no, SETTINGS = debug overlay (dg_debug.c) on/off,
 *          POWER = quit to U-Boot.
 * Serial:  arrows or w a s d, space = use, f / j = fire, Enter, Esc,
 *          , . = strafe, Tab = map, digits, y / n, D (capital) = debug
 *          overlay, Q (capital) = quit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_video.h"

#include "sdk.h"

void dg_sound_pump (void);          /* dg_sound.c */
void dg_sound_stop (void);
void dg_debug_frame (struct fb *fb);    /* dg_debug.c */
void dg_debug_clear (struct fb *fb, int picture_x);
extern int dg_debug_on;

/* Counters for the debug overlay (raw CP0 Count, wrap: deltas only) */
u32 dg_sleep_ticks, dg_draw_ticks, dg_frames, dg_rows_drawn, dg_usb_bytes;
static long files_bytes;            /* bytes of big files loaded (overlay) */

static inline u32 ticks (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

#define OUT_W           960
#define OUT_H           720
#define IR_RELEASE_MS   200         /* NEC repeats every ~110 ms */

static struct fb fb;
static int out_x, out_y;
static u32 lut[256];                /* palette -> two identical ARGB1555 pixels */
static unsigned char prev_frame[SCREENWIDTH * SCREENHEIGHT];
static int full_redraw = 1;
static u32 line_buf[OUT_W / 2];

/* ---- files: progress bar for big loads (sdk_load_progress) ---- */

static void load_progress (u32 done, u32 total) {
    static u32 last;

    if (done < last) {
        last = 0;
    }
    if (done - last < 1024 * 1024 && done != total) {
        return;
    }
    last = done;
    printf ("\r  %u / %u KB ", done / 1024, total / 1024);
    if (done == total) {
        printf ("\n");
        files_bytes += total;
    }
    if (fb.pix && total > 1024 * 1024) {
        fb_rect (&fb, out_x, out_y + OUT_H / 2 - 8,
                 (int) ((unsigned long long) OUT_W * done / total), 16, RGB (200, 40, 40));
    }
}

long dg_files_bytes (void) {
    return files_bytes;
}

static void cleanup (void) {
    dg_sound_stop ();
    if (fb.pix) {
        fb_clear (&fb, TRANSPARENT);
    }
    led_green (1);
}

/* ---- screen ---- */

static void build_lut (void) {
    int i;

    for (i = 0; i < 256; i++) {
        u32 c = RGB (colors[i].r, colors[i].g, colors[i].b);

        if (c == 0x801f) {
            c = 0x801e;             /* pure blue is the OSD colour key */
        }
        lut[i] = (c << 16) | c;
    }
}

void DG_Init (void) {
    if (osd_setup (&fb) < 0) {
        printf ("dg: display not running: source avstart.scr first\n");
        exit (1);
    }
    out_x = ((fb.w - OUT_W) / 2) & ~1;
    out_y = (fb.h - OUT_H) / 2;
    fb_clear (&fb, BLACK);
}

/* 320 source pixels -> 960 output pixels = 480 pixel pairs:
 * source a b -> output a a a b b b -> pairs (a,a) (a,b) (b,b) */
static void expand_line (const unsigned char *src) {
    u32 *out = line_buf;
    int x;

    for (x = 0; x < SCREENWIDTH; x += 2) {
        u32 a = lut[src[x]], b = lut[src[x + 1]];

        out[0] = a;
        out[1] = (a & 0xffff0000u) | (b & 0xffff);
        out[2] = b;
        out += 3;
    }
}

void DG_DrawFrame (void) {
    const unsigned char *frame = (const unsigned char *) DG_ScreenBuffer;
    u32 t0 = ticks ();
    int y;

    if (palette_changed) {
        palette_changed = false;
        build_lut ();
        full_redraw = 1;
    }
    for (y = 0; y < SCREENHEIGHT; y++) {
        const unsigned char *src = frame + y * SCREENWIDTH;
        int oy, oy_end;

        if (!full_redraw && !memcmp (src, prev_frame + y * SCREENWIDTH, SCREENWIDTH)) {
            continue;
        }
        memcpy (prev_frame + y * SCREENWIDTH, src, SCREENWIDTH);
        expand_line (src);
        dg_rows_drawn++;

        /* output rows y*3.6 .. (y+1)*3.6 */
        oy_end = (y + 1) * OUT_H / SCREENHEIGHT;
        for (oy = y * OUT_H / SCREENHEIGHT; oy < oy_end; oy++) {
            volatile u32 *dst = (volatile u32 *) (fb.pix + (out_y + oy) * fb.pitch + out_x);
            int i;

            for (i = 0; i < OUT_W / 2; i++) {
                dst[i] = line_buf[i];
            }
        }
    }
    full_redraw = 0;
    dg_draw_ticks += ticks () - t0;
    dg_frames++;
    dg_usb_bytes = sdk_usb_bytes ();
    dg_sound_pump ();               /* drawing is the slow part: top up audio */
    dg_debug_frame (&fb);
    sdk_overlay_tick ();
}

/* ---- timer ---- */

void DG_SleepMs (uint32_t ms) {
    u32 t0 = ticks ();

    sdk_idle (ms * 1000);                   /* counted idle for the SDK overlay too */
    dg_sleep_ticks += ticks () - t0;
}

static void toggle_debug (void) {
    dg_debug_on = !dg_debug_on;
    if (!dg_debug_on) {
        dg_debug_clear (&fb, out_x);
    }
}

uint32_t DG_GetTicksMs (void) {
    return ub_get_timer (0);
}

void DG_SetWindowTitle (const char *title) {
    (void) title;
}

/* ---- input ---- */

#define QSIZE 64
static struct { unsigned char pressed, key; } keyq[QSIZE];
static int q_head, q_tail;

static void post (int pressed, unsigned char key) {
    int next = (q_tail + 1) % QSIZE;

    if (key && next != q_head) {
        keyq[q_tail].pressed = pressed;
        keyq[q_tail].key = key;
        q_tail = next;
    }
}

/* Remote: up to two Doom keys per button (OK = fire + menu enter) */
static const struct { unsigned char ir, k1, k2; } ir_map[] = {
    { IR_KEY_UP, KEY_UPARROW, 0 },      { IR_KEY_DOWN, KEY_DOWNARROW, 0 },
    { IR_KEY_LEFT, KEY_LEFTARROW, 0 },  { IR_KEY_RIGHT, KEY_RIGHTARROW, 0 },
    { IR_KEY_OK, KEY_FIRE, KEY_ENTER }, { IR_KEY_RED, KEY_USE, 0 },
    { IR_KEY_GREEN, KEY_STRAFE_L, 0 },  { IR_KEY_YELLOW, KEY_STRAFE_R, 0 },
    { IR_KEY_INFO, KEY_TAB, 0 },        { IR_KEY_EXIT, KEY_ESCAPE, 0 },
    { IR_KEY_HOME, KEY_ESCAPE, 0 },     { IR_KEY_PLAY, 'y', 0 },
    { IR_KEY_STOP, 'n', 0 },            { IR_KEY_1, '1', 0 },
    { IR_KEY_2, '2', 0 },               { IR_KEY_3, '3', 0 },
    { IR_KEY_4, '4', 0 },               { IR_KEY_5, '5', 0 },
    { IR_KEY_6, '6', 0 },               { IR_KEY_7, '7', 0 },
    { IR_KEY_8, '8', 0 },               { IR_KEY_9, '9', 0 },
    { IR_KEY_0, '0', 0 },
};

static int ir_ready, run_on;
static int held = -1;               /* ir_map index of the held button */
static u32 held_ms;

static void release_held (void) {
    if (held >= 0) {
        post (0, ir_map[held].k1);
        post (0, ir_map[held].k2);
        held = -1;
    }
}

static void poll_remote (void) {
    struct ir_event ev;
    u32 now = ub_get_timer (0);

    if (!ir_ready) {
        ir_init ();
        ir_ready = 1;
    }
    while (ir_poll (&ev)) {
        unsigned int i;

        if (ev.user != IR_USER_STOCK) {
            continue;
        }
        sdk_saver_kick ();                  /* SDK screen saver: a key, wake the screen */
        if (ev.key == IR_KEY_POWER) {
            exit (0);
        }
        if (ev.key == IR_KEY_MUTE) {
            if (!ev.repeat) {
                sdk_overlay_on = !sdk_overlay_on;   /* SDK performance bar */
            }
            continue;
        }
        if (ev.key == IR_KEY_SETTINGS) {
            if (!ev.repeat) {
                toggle_debug ();
            }
            continue;
        }
        if (ev.key == IR_KEY_BLUE) {
            if (!ev.repeat) {       /* run toggle = hold Shift */
                run_on = !run_on;
                post (run_on, KEY_RSHIFT);
            }
            continue;
        }
        for (i = 0; i < sizeof (ir_map) / sizeof (ir_map[0]); i++) {
            if (ir_map[i].ir == ev.key) {
                break;
            }
        }
        if (i == sizeof (ir_map) / sizeof (ir_map[0])) {
            continue;
        }
        if (held == (int) i) {
            held_ms = now;          /* still held */
            continue;
        }
        release_held ();
        post (1, ir_map[i].k1);
        post (1, ir_map[i].k2);
        held = i;
        held_ms = now;
    }
    if (held >= 0 && now - held_ms > IR_RELEASE_MS) {
        release_held ();
    }
}

/* Serial keys: press now, release on the next poll (Doom sees one tic) */
static unsigned char serial_down;

static unsigned char serial_key (int c) {
    switch (c) {
    case 'w': return KEY_UPARROW;
    case 's': return KEY_DOWNARROW;
    case 'a': return KEY_LEFTARROW;
    case 'd': return KEY_RIGHTARROW;
    case ',': return KEY_STRAFE_L;
    case '.': return KEY_STRAFE_R;
    case ' ': return KEY_USE;
    case 'f':
    case 'j': return KEY_FIRE;
    case '\r':
    case '\n': return KEY_ENTER;
    case 27: return KEY_ESCAPE;
    case '\t': return KEY_TAB;
    case 8:
    case 127: return KEY_BACKSPACE;
    default: return (c >= 32 && c < 127) ? c : 0;
    }
}

static void poll_serial (void) {
    if (serial_down) {
        post (0, serial_down);
        serial_down = 0;
    }
    if (ub_tstc ()) {
        int c = ub_getc ();

        if (c == 'Q') {
            exit (0);
        }
        if (c == 'D') {
            toggle_debug ();
            return;
        }
        if (c == 27) {              /* arrows: ESC [ A/B/C/D */
            u32 t = ub_get_timer (0);

            while (!ub_tstc () && ub_get_timer (t) < 5) {
            }
            if (ub_tstc () && ub_getc () == '[') {
                while (!ub_tstc () && ub_get_timer (t) < 10) {
                }
                c = ub_tstc () ? ub_getc () : 0;
                serial_down = c == 'A' ? KEY_UPARROW : c == 'B' ? KEY_DOWNARROW :
                              c == 'C' ? KEY_RIGHTARROW : c == 'D' ? KEY_LEFTARROW : 0;
            } else {
                serial_down = KEY_ESCAPE;
            }
        } else {
            serial_down = serial_key (c);
        }
        post (1, serial_down);
    }
}

int DG_GetKey (int *pressed, unsigned char *key) {
    dg_sound_pump ();
    if (q_head == q_tail) {
        poll_remote ();
        poll_serial ();
    }
    if (q_head == q_tail) {
        return 0;
    }
    *pressed = keyq[q_head].pressed;
    *key = keyq[q_head].key;
    q_head = (q_head + 1) % QSIZE;
    return 1;
}

/* ---- main ---- */

int main (int argc, char *argv[]) {
    static char *args[32];
    int n = 0, i;

    int have_iwad = 0, have_mb = 0;

    /* Doom uses the first -iwad / -mb it finds: add ours only if the user
     * gave none, e.g. go ${a} -iwad doom2.wad -merge mod.wad -deh mod.deh */
    for (i = 1; i < argc; i++) {
        have_iwad |= !strcmp (argv[i], "-iwad");
        have_mb |= !strcmp (argv[i], "-mb");
    }
    args[n++] = "doom";
    if (!have_iwad) {
        args[n++] = "-iwad";
        args[n++] = "doom.wad";
    }
    if (!have_mb) {
        args[n++] = "-mb";
        args[n++] = "10";
    }
    for (i = 1; i < argc && n < 31; i++) {
        args[n++] = argv[i];
    }
    args[n] = 0;

    printf ("Doom for the NC5874 box (doomgeneric). POWER on the remote or "
            "'Q' on serial quits.\n");
    sdk_load_progress = load_progress;
    atexit (cleanup);               /* Doom leaves through exit () */
    doomgeneric_Create (n, args);
    for (;;) {
        doomgeneric_Tick ();
    }
}
