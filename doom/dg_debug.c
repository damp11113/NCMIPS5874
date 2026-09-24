/*
 * Debug overlay for Doom on the NC5874 box: two panels in the black bars
 * beside the 960x720 picture, redrawn every 0.5 s.
 *
 *   left  (box):  FPS, CPU split (game logic + rendering / 8->16 bit
 *                 scale+copy to the OSD / effects mixer / OPL music /
 *                 this overlay / idle in DG_SleepMs), OSD rows redrawn,
 *                 memory (heap, WAD, zone, program), audio queue, sound
 *                 channels, OPL voices, USB data loaded
 *   right (game): state, map, skill, level time, kills / items / secrets,
 *                 health / armour, weapon, position, angle, thinkers,
 *                 visplanes / drawsegs / sprites of the last frame
 *
 * Times come from CP0 Count (324000 ticks per ms, see cpuinfo) around
 * each part; "game" is what is left of the wall time.
 * Toggle: SETTINGS on the remote, capital D on serial (dg_nc5874.c).
 */
#include <stdio.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "p_mobj.h"
#include "r_defs.h"
#include "r_bsp.h"
#include "r_things.h"
#include "z_zone.h"

#include "box.h"
#include "strbuf.h"

#define TICKS_PER_MS    324000
#define COLS            18
#define LEFT_X          8
#define LINE_H          20

/* Counters kept by the other files (raw CP0 Count, wrap: deltas only) */
extern u32 dg_sleep_ticks, dg_draw_ticks, dg_sound_ticks, dg_music_ticks, dg_frames;
extern u32 dg_rows_drawn, dg_usb_bytes;
extern size_t heap_in_use;
extern long dg_wad_size;
int dg_sound_active (void);
int dg_music_voices (void);
int dg_music_playing (void);
u32 dg_audio_queued (void);

extern visplane_t *lastvisplane;
extern visplane_t visplanes[];
extern thinker_t thinkercap;
extern char __bss_end[];

int dg_debug_on = 1;
static u32 debug_ticks;             /* time spent drawing the overlay */

struct snap {
    u32 t, sleep, draw, sound, music, osd, frames, rows;
};

static struct snap prev;
static int have_prev;

static inline u32 ticks (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

static void line (struct fb *fb, int x, int row, const char *text, u16 fg) {
    char buf[COLS + 1];
    int i;

    for (i = 0; i < COLS; i++) {
        buf[i] = *text ? *text++ : ' ';
    }
    buf[i] = 0;
    fb_text (fb, x, 12 + LINE_H * row, buf, 1, fg, BLACK);
}

static u32 pct10 (u32 part, u32 whole) {
    whole >>= 10;
    return whole ? (part >> 10) * 1000 / whole : 0;
}

static void s_pct (struct str *s, const char *label, u32 p10) {
    s_add (s, label);
    s_num (s, p10 / 10, 1);
    s_add (s, ".");
    s_num (s, p10 % 10, 1);
    s_add (s, " %");
}

static void s_kb (struct str *s, const char *label, u32 bytes) {
    s_add (s, label);
    if (bytes >= 10 * 1024 * 1024) {
        s_num (s, bytes >> 20, 1);
        s_add (s, " MB");
    } else {
        s_num (s, bytes >> 10, 1);
        s_add (s, " KB");
    }
}

static void s_frac (struct str *s, const char *label, int a, int b) {
    s_add (s, label);
    s_num (s, a < 0 ? 0 : a, 1);
    s_add (s, "/");
    s_num (s, b < 0 ? 0 : b, 1);
}

static const char *weapon_name (int w) {
    static const char *names[] = {
        "FIST", "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKET", "PLASMA", "BFG", "CHAINSAW",
        "SUPER SG"
    };

    return (w >= 0 && w < (int) (sizeof (names) / sizeof (names[0]))) ? names[w] : "-";
}

static void draw_box (struct fb *fb, const struct snap *now) {
    u32 wall = now->t - prev.t;
    u32 ms = wall / TICKS_PER_MS;
    u32 sleep = now->sleep - prev.sleep, draw = now->draw - prev.draw;
    u32 sound = now->sound - prev.sound, music = now->music - prev.music;
    u32 osd = now->osd - prev.osd, frames = now->frames - prev.frames;
    u32 used = sleep + draw + sound + music + osd;
    u32 game = wall > used ? wall - used : 0;
    u32 zone = Z_ZoneSize (), zfree = Z_FreeMemory ();
    struct str s;
    int r = 0;

    line (fb, LEFT_X, r++, "BOX  (SETTINGS)", YELLOW);
    line (fb, LEFT_X, r++, "MIPS 24KEc 648MHz", GREY);

    s_reset (&s);
    s_add (&s, "FPS    ");
    s_num (&s, ms ? frames * 1000 / ms : 0, 1);
    s_add (&s, ".");
    s_num (&s, ms ? frames * 10000 / ms % 10 : 0, 1);
    line (fb, LEFT_X, r++, s.buf, WHITE);

    s_reset (&s);
    s_pct (&s, "CPU    ", 1000 - pct10 (sleep, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);
    s_reset (&s);
    s_pct (&s, " game  ", pct10 (game, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);
    s_reset (&s);
    s_pct (&s, " draw  ", pct10 (draw, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);
    s_reset (&s);
    s_pct (&s, " sfx   ", pct10 (sound, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);
    s_reset (&s);
    s_pct (&s, " music ", pct10 (music, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);
    s_reset (&s);
    s_pct (&s, " osd   ", pct10 (osd, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);
    s_reset (&s);
    s_pct (&s, " idle  ", pct10 (sleep, wall));
    line (fb, LEFT_X, r++, s.buf, CYAN);

    s_reset (&s);
    s_add (&s, "ROWS/F ");
    s_num (&s, frames ? (now->rows - prev.rows) / frames : 0, 1);
    s_add (&s, "/200");
    line (fb, LEFT_X, r++, s.buf, WHITE);

    s_reset (&s);
    s_kb (&s, "HEAP   ", heap_in_use);
    line (fb, LEFT_X, r++, s.buf, GREEN);
    s_reset (&s);
    s_kb (&s, " WAD   ", dg_wad_size);
    line (fb, LEFT_X, r++, s.buf, GREEN);
    s_reset (&s);
    s_kb (&s, "ZONE   ", zone - zfree);
    line (fb, LEFT_X, r++, s.buf, GREEN);
    s_reset (&s);
    s_kb (&s, " of    ", zone);
    line (fb, LEFT_X, r++, s.buf, GREEN);
    s_reset (&s);
    s_kb (&s, "PROG   ", (u32) __bss_end - 0x80008000u);
    line (fb, LEFT_X, r++, s.buf, GREEN);

    s_reset (&s);
    s_add (&s, "AUDIO  ");
    s_num (&s, dg_audio_queued () / 48, 1);
    s_add (&s, " ms");
    line (fb, LEFT_X, r++, s.buf, WHITE);
    s_reset (&s);
    s_frac (&s, "SFX CH ", dg_sound_active (), 16);
    line (fb, LEFT_X, r++, s.buf, WHITE);
    s_reset (&s);
    s_frac (&s, "OPL    ", dg_music_voices (), 9);
    s_add (&s, dg_music_playing () ? " on" : " off");
    line (fb, LEFT_X, r++, s.buf, WHITE);
    s_reset (&s);
    s_kb (&s, "USB    ", dg_usb_bytes);
    line (fb, LEFT_X, r++, s.buf, WHITE);
}

static void draw_game (struct fb *fb) {
    int x = fb->w - 8 * COLS - 8, r = 0;
    player_t *p = &players[consoleplayer];
    struct str s;
    thinker_t *t;
    int n;

    line (fb, x, r++, "GAME", YELLOW);

    s_reset (&s);
    s_add (&s, gamestate == GS_LEVEL ? "LEVEL" : gamestate == GS_INTERMISSION ? "INTERMISSION" :
               gamestate == GS_FINALE ? "FINALE" : "DEMO/TITLE");
    if (menuactive) {
        s_add (&s, " MENU");
    }
    if (paused) {
        s_add (&s, " PAUSE");
    }
    if (automapactive) {
        s_add (&s, " MAP");
    }
    line (fb, x, r++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "E");
    s_num (&s, gameepisode, 1);
    s_add (&s, "M");
    s_num (&s, gamemap, 1);
    s_add (&s, "  SKILL ");
    s_num (&s, gameskill + 1, 1);
    line (fb, x, r++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "TIME   ");
    s_time (&s, leveltime / TICRATE);
    line (fb, x, r++, s.buf, WHITE);

    s_reset (&s);
    s_frac (&s, "KILLS  ", p->killcount, totalkills);
    line (fb, x, r++, s.buf, WHITE);
    s_reset (&s);
    s_frac (&s, "ITEMS  ", p->itemcount, totalitems);
    line (fb, x, r++, s.buf, WHITE);
    s_reset (&s);
    s_frac (&s, "SECRET ", p->secretcount, totalsecret);
    line (fb, x, r++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "HEALTH ");
    s_num (&s, p->health < 0 ? 0 : p->health, 1);
    s_add (&s, " AR ");
    s_num (&s, p->armorpoints, 1);
    line (fb, x, r++, s.buf, WHITE);
    s_reset (&s);
    s_add (&s, "WEAPON ");
    s_add (&s, weapon_name (p->readyweapon));
    line (fb, x, r++, s.buf, WHITE);

    if (gamestate == GS_LEVEL && p->mo) {
        int px = p->mo->x >> FRACBITS, py = p->mo->y >> FRACBITS;

        s_reset (&s);
        s_add (&s, "X ");
        if (px < 0) {
            s_add (&s, "-");
            px = -px;
        }
        s_num (&s, px, 1);
        s_add (&s, " Y ");
        if (py < 0) {
            s_add (&s, "-");
            py = -py;
        }
        s_num (&s, py, 1);
        line (fb, x, r++, s.buf, GREEN);
        s_reset (&s);
        s_add (&s, "ANGLE  ");
        s_num (&s, (u32) ((p->mo->angle >> 16) * 360u) >> 16, 1);
        line (fb, x, r++, s.buf, GREEN);
    } else {
        line (fb, x, r++, "X -  Y -", GREEN);
        line (fb, x, r++, "ANGLE  -", GREEN);
    }

    for (n = 0, t = thinkercap.next; t && t != &thinkercap && n < 100000; t = t->next) {
        n++;
    }
    s_reset (&s);
    s_add (&s, "THINKER ");
    s_num (&s, n, 1);
    line (fb, x, r++, s.buf, CYAN);

    r++;
    line (fb, x, r++, "LAST FRAME", YELLOW);
    s_reset (&s);
    s_frac (&s, "VISPLN ", lastvisplane ? (int) (lastvisplane - visplanes) : 0, 128);
    line (fb, x, r++, s.buf, CYAN);
    s_reset (&s);
    s_frac (&s, "DRAWSG ", ds_p ? (int) (ds_p - drawsegs) : 0, MAXDRAWSEGS);
    line (fb, x, r++, s.buf, CYAN);
    s_reset (&s);
    s_frac (&s, "SPRITE ", vissprite_p ? (int) (vissprite_p - vissprites) : 0, MAXVISSPRITES);
    line (fb, x, r++, s.buf, CYAN);
}

/* Called at the end of every DG_DrawFrame */
void dg_debug_frame (struct fb *fb) {
    static u32 last_draw;
    struct snap now;
    u32 t0 = ticks ();

    if (!have_prev) {
        prev.t = t0;
        prev.sleep = dg_sleep_ticks;
        prev.draw = dg_draw_ticks;
        prev.sound = dg_sound_ticks;
        prev.music = dg_music_ticks;
        prev.osd = debug_ticks;
        prev.frames = dg_frames;
        prev.rows = dg_rows_drawn;
        have_prev = 1;
        last_draw = t0;
        return;
    }
    if (t0 - last_draw < 500 * TICKS_PER_MS) {
        return;
    }
    last_draw = t0;

    now.t = t0;
    now.sleep = dg_sleep_ticks;
    now.draw = dg_draw_ticks;
    now.sound = dg_sound_ticks;
    now.music = dg_music_ticks;
    now.osd = debug_ticks;
    now.frames = dg_frames;
    now.rows = dg_rows_drawn;
    if (dg_debug_on) {
        draw_box (fb, &now);
        draw_game (fb);
    }
    prev = now;
    debug_ticks += ticks () - t0;
}

/* Blank both side panels (overlay switched off) */
void dg_debug_clear (struct fb *fb, int picture_x) {
    fb_rect (fb, 0, 0, picture_x, fb->h, BLACK);
    fb_rect (fb, fb->w - picture_x, 0, picture_x, fb->h, BLACK);
}
