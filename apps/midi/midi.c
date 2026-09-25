/*
 * MIDI player for the NC5874 box (SDK app, NCAPPS/APPS/MIDI).
 *
 * Plays Standard MIDI files (.mid / .midi / .kar, type 0 and 1) with two
 * synth engines, switchable while playing (BLUE):
 *   OPL  - FM synthesis on the OPL emulator with the GENMIDI instrument set
 *          (GENMIDI.OP2 in the app folder, else the GENMIDI lump of
 *          /NCAPPS/APPS/DOOM/DOOM2.WAD or DOOM.WAD)
 *   SF2  - SoundFont 2 samples: "-sf2 FILE.SF2" argument, else the first
 *          .sf2 in the app folder (the whole file is loaded into RAM, so
 *          keep it below ~28 MB). YELLOW opens a list of the .sf2 files in
 *          the app folder and in /SOUNDFONTS to load another one.
 *
 * Start folder: first argument that is not an option, default /MIDI, else
 * the stick root. Browser: UP / DOWN select, OK open / play, BACK folder
 * up (quit at the start folder), HOME / POWER quit.
 * Playing: OK / PLAY / PAUSE pause, UP previous, DOWN / NEXT next, LEFT /
 * RIGHT volume, RED / GREEN seek (tap = 2 s, hold = faster, jumps on
 * release; programs and controllers are chased), BLUE engine OPL <-> SF2,
 * YELLOW choose SoundFont, STOP / BACK list.
 * The screen shows the 16 MIDI channels with their instruments.
 */
#define BOX_WANT_AUDIO
#include "sdk.h"
#include "synth.h"
#include "smf.h"
#include "gm_names.h"
#include "seekhold.h"

#define BG          RGB (12, 18, 40)
#define PANEL       RGB (24, 34, 72)
#define HILITE      RGB (60, 110, 200)
#define BAR_OFF     RGB (30, 40, 80)

#define MAX_ITEMS   512
#define ROWS        13
#define ROW_H       40
#define TARGET_Q    8192            /* frames queued in the audio ring (170 ms) */

static struct fb fb;

struct item {
    char name[100];
    int is_dir;
    u32 size;
};

static struct item items[MAX_ITEMS];
static int nitems, sel, top;
static char cur_dir[SDK_PATH_MAX], start_dir[SDK_PATH_MAX];

static int have_opl, have_sf2;
static struct synth *engine;
static int vol = 80, paused;
static struct seekhold sk;
static unsigned char *song_data;

/* ---- helpers ---- */

static int has_ext (const char *n, const char *ext) {
    int a = strlen (n), b = strlen (ext);

    return a > b && !strcasecmp (n + a - b, ext);
}

static int is_midi (const char *n) {
    return has_ext (n, ".mid") || has_ext (n, ".midi") || has_ext (n, ".kar");
}

static void text (int x, int y, const char *s, int scale, u16 fg, u16 bg, int max) {
    char buf[128];
    int n = strlen (s);

    if (n > max) {
        n = max;
    }
    if (n > 127) {
        n = 127;
    }
    memcpy (buf, s, n);
    buf[n] = 0;
    fb_text (&fb, x, y, buf, scale, fg, bg);
}

static void message (const char *msg, u16 color) {
    fb_rect (&fb, 0, 300, fb.w, 100, PANEL);
    fb_text (&fb, 60, 334, msg, 2, color, TRANSPARENT);
}

static void progress (u32 done, u32 total) {
    if (total) {
        fb_rect (&fb, 60, 380, (int) (1160ull * done / total), 12, CYAN);
    }
}

/* ---- instrument data ---- */

static u32 le32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

/* GENMIDI lump from a Doom WAD (directory: 16-byte entries) */
static unsigned char *genmidi_from_wad (const char *path, long *len) {
    unsigned char hdr[12], *dir, *lump = 0;
    int h = sdk_open (path);
    u32 n, off, i;

    if (h < 0) {
        return 0;
    }
    if (sdk_read (h, hdr, 12) == 12 && (!memcmp (hdr, "IWAD", 4) || !memcmp (hdr, "PWAD", 4))) {
        n = le32 (hdr + 4);
        off = le32 (hdr + 8);
        dir = malloc (n * 16);
        if (dir) {
            sdk_seek (h, off);
            if (sdk_read (h, dir, n * 16) == (long) (n * 16)) {
                for (i = 0; i < n; i++) {
                    if (!strncmp ((char *) dir + i * 16 + 8, "GENMIDI", 8)) {
                        *len = le32 (dir + i * 16 + 4);
                        lump = malloc (*len);
                        sdk_seek (h, le32 (dir + i * 16));
                        if (lump && sdk_read (h, lump, *len) != *len) {
                            free (lump);
                            lump = 0;
                        }
                        break;
                    }
                }
            }
            free (dir);
        }
    }
    sdk_close (h);
    return lump;
}

/* ---- SoundFonts: list, load, choose ---- */

#define MAX_SF      32
extern size_t heap_total;
#define SF2_MAX     ((u32) heap_total - (4u << 20))     /* leave room for the rest */

struct sfile {
    char path[SDK_PATH_MAX];        /* relative = app folder, /SOUNDFONTS/... */
    char name[64];
    u32 size;
};

static struct sfile sflist[MAX_SF];
static int nsf;
static unsigned char *sf2_data;
static char sf2_path[SDK_PATH_MAX];

static void list_folder (const char *dir, const char *prefix) {
    struct sdk_dirent e;
    int h = sdk_dir_open (dir);

    while (h >= 0 && sdk_dir_read (h, &e) && nsf < MAX_SF) {
        if (!e.is_dir && has_ext (e.name, ".sf2")) {
            snprintf (sflist[nsf].path, sizeof (sflist[0].path), "%s%s", prefix, e.name);
            snprintf (sflist[nsf].name, sizeof (sflist[0].name), "%s", e.name);
            sflist[nsf].size = e.size;
            nsf++;
        }
    }
    sdk_dir_close (h);
}

static void list_sf2 (void) {
    nsf = 0;
    list_folder ("", "");                       /* the app folder */
    list_folder ("/SOUNDFONTS", "/SOUNDFONTS/");
}

/* Replace the loaded SoundFont. Returns 1 when the new one is ready. */
static int load_sf2 (const char *path) {
    char msg[160];
    unsigned char *data;
    long len = sdk_file_size (path);

    if (len < 0) {
        printf ("midi: %s not found\n", path);
        return 0;
    }
    if ((u32) len > SF2_MAX) {
        snprintf (msg, sizeof (msg), "%s is too big (%ld MB, max %d MB)", path, len >> 20,
                  SF2_MAX >> 20);
        message (msg, RED);
        ub_udelay (2000000);
        return 0;
    }
    synth_sf2.reset ();                         /* no voice may point into the old data */
    have_sf2 = 0;
    free (sf2_data);
    sf2_data = 0;
    sf2_path[0] = 0;

    snprintf (msg, sizeof (msg), "Loading SoundFont %s (%ld KB) ...", path, len / 1024);
    message (msg, WHITE);
    sdk_load_progress = progress;
    if (sdk_load_file (path, &data, &len) == 0) {
        if (synth_sf2_init (data, len) == 0) {
            have_sf2 = 1;
            sf2_data = data;
            snprintf (sf2_path, sizeof (sf2_path), "%s", path);
        } else {
            free (data);
        }
    }
    sdk_load_progress = 0;
    if (!have_sf2) {
        message ("That file is not a usable SoundFont", RED);
        ub_udelay (1500000);
    }
    return have_sf2;
}

/* List screen. Returns 1 if another SoundFont was loaded. */
static int choose_sf2 (void) {
    struct sdk_key k;
    int cur = 0, i, redraw = 1;

    list_sf2 ();
    for (i = 0; i < nsf; i++) {
        if (!strcmp (sflist[i].path, sf2_path)) {
            cur = i;
        }
    }
    for (;;) {
        if (redraw) {
            fb_clear (&fb, BG);
            fb_rect (&fb, 0, 0, fb.w, 100, PANEL);
            fb_text (&fb, 40, 16, "SOUNDFONTS", 3, WHITE, TRANSPARENT);
            fb_text (&fb, 40, 64, "app folder + /SOUNDFONTS on the stick", 2, GREY, TRANSPARENT);
            if (nsf == 0) {
                fb_text (&fb, 60, 140, "No .sf2 files found", 2, YELLOW, TRANSPARENT);
            }
            for (i = 0; i < nsf && i < ROWS; i++) {
                int y = 130 + i * ROW_H, big = sflist[i].size > SF2_MAX;
                char line[120];

                if (i == cur) {
                    fb_rect (&fb, 30, y - 6, fb.w - 60, ROW_H - 4, HILITE);
                }
                snprintf (line, sizeof (line), "%s%s", sflist[i].name,
                          !strcmp (sflist[i].path, sf2_path) ? "  (loaded)" : "");
                text (50, y, line, 2, big ? GREY : WHITE, TRANSPARENT, 56);
                snprintf (line, sizeof (line), big ? "%d MB too big" : "%d.%d MB",
                          sflist[i].size >> 20, (sflist[i].size >> 16) * 10 / 16 % 10);
                fb_text (&fb, fb.w - 260, y, line, 2, big ? RED : GREY, TRANSPARENT);
            }
            fb_rect (&fb, 0, 680, fb.w, 40, PANEL);
            fb_text (&fb, 40, 688, "OK load   UP/DOWN select   BACK cancel", 2, GREY, TRANSPARENT);
            redraw = 0;
        }
        if (!sdk_key_poll (&k) || k.btn == BTN_NONE) {
            continue;
        }
        if (k.btn == BTN_UP && nsf) {
            cur = (cur + nsf - 1) % nsf;
            redraw = 1;
        } else if (k.btn == BTN_DOWN && nsf) {
            cur = (cur + 1) % nsf;
            redraw = 1;
        } else if (k.btn == BTN_OK && nsf && !k.repeat) {
            return load_sf2 (sflist[cur].path);
        } else if ((k.btn == BTN_BACK || k.btn == BTN_YELLOW) && !k.repeat) {
            return 0;
        }
    }
}

static void load_instruments (const char *sf2_arg) {
    unsigned char *data;
    long len;

    /* OPL: GENMIDI.OP2 here, else from the Doom WADs */
    if (sdk_load_file ("GENMIDI.OP2", &data, &len) == 0 ||
        (data = genmidi_from_wad ("/NCAPPS/APPS/DOOM/DOOM2.WAD", &len)) ||
        (data = genmidi_from_wad ("/NCAPPS/APPS/DOOM/DOOM.WAD", &len))) {
        have_opl = synth_opl_init (data, len) == 0;
    }

    /* SF2: argument, else the first .sf2 in the app folder */
    if (sf2_arg) {
        load_sf2 (sf2_arg);
    } else {
        list_sf2 ();
        if (nsf) {
            load_sf2 (sflist[0].path);
        }
    }
    engine = have_sf2 ? &synth_sf2 : have_opl ? &synth_opl : 0;
    printf ("midi: OPL %s, SF2 %s\n", have_opl ? "ready" : "missing (no GENMIDI)",
            have_sf2 ? synth_sf2_name () : "missing");
}

/* ---- browser ---- */

static int item_cmp (const struct item *a, const struct item *b) {
    if (a->is_dir != b->is_dir) {
        return b->is_dir - a->is_dir;
    }
    return strcasecmp (a->name, b->name);
}

static void scan (void) {
    struct sdk_dirent e;
    char path[SDK_PATH_MAX];
    int h, i, j;

    snprintf (path, sizeof (path), "/%s", cur_dir);     /* from the stick root */
    h = sdk_dir_open (path);

    nitems = sel = top = 0;
    if (h < 0) {
        return;
    }
    while (sdk_dir_read (h, &e) && nitems < MAX_ITEMS) {
        if (e.name[0] == '.' || (!e.is_dir && !is_midi (e.name))) {
            continue;
        }
        snprintf (items[nitems].name, sizeof (items[0].name), "%s", e.name);
        items[nitems].is_dir = e.is_dir;
        items[nitems].size = e.size;
        nitems++;
    }
    sdk_dir_close (h);
    for (i = 1; i < nitems; i++) {
        for (j = i; j > 0 && item_cmp (&items[j - 1], &items[j]) > 0; j--) {
            struct item t = items[j];

            items[j] = items[j - 1];
            items[j - 1] = t;
        }
    }
}

static void draw_browser (void) {
    int i;

    fb_clear (&fb, BG);
    fb_rect (&fb, 0, 0, fb.w, 100, PANEL);
    fb_text (&fb, 40, 16, "MIDI", 3, WHITE, TRANSPARENT);
    {
        char line[160];

        snprintf (line, sizeof (line), "%s   OPL %s   SF2 %s", cur_dir[0] ? cur_dir : "/",
                  have_opl ? "yes" : "no", have_sf2 ? synth_sf2_name () : "no");
        text (40, 64, line, 2, GREY, TRANSPARENT, 74);
    }
    if (nitems == 0) {
        fb_text (&fb, 60, 140, "No folders or MIDI files here", 2, YELLOW, TRANSPARENT);
    }
    for (i = 0; i < ROWS && top + i < nitems; i++) {
        const struct item *it = &items[top + i];
        int y = 130 + i * ROW_H, on = top + i == sel;
        char line[120];

        if (on) {
            fb_rect (&fb, 30, y - 6, fb.w - 60, ROW_H - 4, HILITE);
        }
        snprintf (line, sizeof (line), it->is_dir ? "[%s]" : "%s", it->name);
        text (50, y, line, 2, it->is_dir ? YELLOW : (on ? WHITE : RGB (200, 210, 230)),
              TRANSPARENT, 64);
        if (!it->is_dir) {
            char sz[24];

            snprintf (sz, sizeof (sz), "%d KB", (it->size + 1023) / 1024);
            fb_text (&fb, fb.w - 180, y, sz, 2, GREY, TRANSPARENT);
        }
    }
    fb_rect (&fb, 0, 680, fb.w, 40, PANEL);
    fb_text (&fb, 40, 688, "OK open/play   YELLOW SoundFont   BACK folder up   HOME quit", 2, GREY,
             TRANSPARENT);
}

/* ---- playback ---- */

static int acc_l[512], acc_r[512];
static short mix[1024];

/*
 * SoundFont voice limit from the CPU time the synth needs: CP0 Count runs
 * at 324 MHz = 6750 ticks per 48 kHz sample. Every 100 ms of audio: above
 * 70 % (or the audio ring running low) fewer voices are allowed, so notes
 * get stolen instead of the sound stuttering; below 45 % the limit grows
 * back to the maximum.
 */
#define TICKS_PER_SAMPLE    6750u
static u32 load_ticks, load_samples;
int synth_load_pct;                             /* last measured, for the screen */

static inline u32 cp0_count (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

static void voice_budget (u32 ticks, u32 n, u32 queued) {
    int active, lim = synth_sf2_limit;

    load_ticks += ticks;
    load_samples += n;
    if (queued < 1024 && engine == &synth_sf2) {    /* nearly empty: act now */
        active = synth_sf2.voices ();
        synth_sf2_limit = active > 24 ? active - active / 4 : 16;
    }
    if (load_samples < SYNTH_RATE / 10) {
        return;
    }
    synth_load_pct = (int) ((unsigned long long) load_ticks * 100 /
                            ((unsigned long long) load_samples * TICKS_PER_SAMPLE));
    load_ticks = load_samples = 0;
    if (engine != &synth_sf2) {
        return;
    }
    active = synth_sf2.voices ();
    if (synth_load_pct > 70 && active > 16) {
        lim = active * 60 / synth_load_pct;     /* aim for ~60 % */
        lim = lim < 16 ? 16 : lim;
    } else if (synth_load_pct < 45 && lim < synth_sf2.max_voices) {
        lim += 4;
    }
    synth_sf2_limit = lim > synth_sf2.max_voices ? synth_sf2.max_voices : lim;
}

static void pump (void) {
    u32 queued = ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME, n, i;

    if (queued >= TARGET_Q) {
        return;
    }
    n = TARGET_Q - queued;
    n = n > 512 ? 512 : n;
    memset (acc_l, 0, n * sizeof (int));
    memset (acc_r, 0, n * sizeof (int));
    if (!paused) {                              /* paused: zeros, not a held sample */
        u32 t0 = cp0_count ();

        smf_render (acc_l, acc_r, n);
        voice_budget (cp0_count () - t0, n, queued);
    }
    for (i = 0; i < n; i++) {
        int l = acc_l[i] * vol / 100, r = acc_r[i] * vol / 100;

        l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
        mix[2 * i] = r;                         /* audio.h: R = hi, L = lo */
        mix[2 * i + 1] = l;
    }
    audio_write (mix, n);
}

static void draw_player (int idx) {
    char line[160];

    fb_clear (&fb, BG);
    fb_rect (&fb, 0, 0, fb.w, 100, PANEL);
    fb_text (&fb, 40, 16, "MIDI PLAYER", 2, GREY, TRANSPARENT);
    snprintf (line, sizeof (line), "%d / %d  %s", idx + 1, nitems, cur_dir[0] ? cur_dir : "/");
    text (40, 60, line, 2, GREY, TRANSPARENT, 74);
    snprintf (line, sizeof (line), "%s", items[idx].name);
    if (strrchr (line, '.')) {
        *strrchr (line, '.') = 0;
    }
    text (40, 120, line, 3, WHITE, TRANSPARENT, 30);
    text (40, 176, smf_title ()[0] ? smf_title () : "", 2, YELLOW, TRANSPARENT, 36);
    snprintf (line, sizeof (line), "Engine: %s", engine->name);
    text (40, 230, line, 2, CYAN, TRANSPARENT, 36);
    if (engine == &synth_sf2) {
        text (40, 262, synth_sf2_name (), 2, CYAN, TRANSPARENT, 36);
    }
    fb_rect (&fb, 0, 640, fb.w, 80, PANEL);
    fb_text (&fb, 40, 650, "OK pause  UP/DOWN prev/next  LEFT/RIGHT vol  RED/GREEN seek", 2, GREY,
             TRANSPARENT);
    fb_text (&fb, 40, 684, "BLUE engine  YELLOW SoundFont  BACK list  HOME quit", 2, GREY, TRANSPARENT);
}

static void draw_status (void) {
    char line[96];
    u32 pos = sk.active ? sk.target_ms : smf_position_ms (), len = smf_length_ms ();
    int ch, w = len ? (int) (560ull * (pos < len ? pos : len) / len) : 0;

    fb_rect (&fb, 40, 320, w, 14, CYAN);
    fb_rect (&fb, 40 + w, 320, 560 - w, 14, BAR_OFF);
    snprintf (line, sizeof (line), "%d:%02d / %d:%02d  %s ", pos / 60000, pos / 1000 % 60,
              len / 60000, len / 1000 % 60, sk.active ? "SEEK  " : paused ? "PAUSED" : "      ");
    fb_text (&fb, 40, 350, line, 2, WHITE, BG);
    snprintf (line, sizeof (line), "Tempo %3d bpm   vol %3d%%", smf_tempo_bpm (), vol);
    fb_text (&fb, 40, 390, line, 2, WHITE, BG);
    snprintf (line, sizeof (line), "Voices %3d/%-3d  synth CPU %3d%%", engine->voices (),
              engine == &synth_sf2 ? synth_sf2_limit : engine->max_voices, synth_load_pct);
    fb_text (&fb, 40, 424, line, 2, synth_load_pct > 70 ? YELLOW : WHITE, BG);

    /* 16 channels: instrument + activity */
    for (ch = 0; ch < 16; ch++) {
        int y = 116 + ch * 32, lv = smf_chan_level[ch];
        const char *name = ch == 9 ? "Drums" : gm_names[smf_chan_program[ch] & 127];

        snprintf (line, sizeof (line), "%2d %-14s", ch + 1, smf_chan_used[ch] ? name : "-");
        fb_text (&fb, 660, y, line, 1, smf_chan_used[ch] ? WHITE : GREY, BG);
        fb_rect (&fb, 800, y + 2, lv * 3, 12, lv > 100 ? YELLOW : GREEN);
        fb_rect (&fb, 800 + lv * 3, y + 2, 381 - lv * 3, 12, BAR_OFF);
        smf_chan_level[ch] = lv > 6 ? lv - 6 : 0;
    }
}

static int open_song (int idx) {
    char path[SDK_PATH_MAX];
    long len;

    free (song_data);
    song_data = 0;
    snprintf (path, sizeof (path), "/%s%s%s", cur_dir, cur_dir[0] ? "/" : "", items[idx].name);
    if (sdk_load_file (path, &song_data, &len) < 0 || smf_load (song_data, len, engine) < 0) {
        printf ("midi: cannot play %s\n", path);
        return -1;
    }
    printf ("midi: %s \"%s\", %d:%02d, %s\n", items[idx].name, smf_title (),
            smf_length_ms () / 60000, smf_length_ms () / 1000 % 60, engine->name);
    paused = 0;
    return 0;
}

static void play_from (int idx) {
    struct sdk_key k;
    u32 last = 0, t0 = ub_get_timer (0);

    if (!engine) {
        message ("No instruments: add GENMIDI.OP2 or a .sf2 file", RED);
        ub_udelay (2000000);
        return;
    }
    while (idx < nitems && (items[idx].is_dir || open_song (idx) < 0)) {
        idx++;
    }
    if (idx >= nitems) {
        return;
    }
    audio_start ();
    draw_player (idx);
    for (;;) {
        int next = 0;
        u32 now;

        pump ();
        if (smf_done () && engine->voices () == 0 && !sk.active) {
            next = 1;
        }
        if (sdk_key_poll (&k) && k.btn != BTN_NONE &&
            !(k.repeat && k.btn != BTN_LEFT && k.btn != BTN_RIGHT && k.btn != BTN_RED &&
              k.btn != BTN_GREEN)) {
            if (k.btn == BTN_OK || k.btn == BTN_PLAY || k.btn == BTN_PAUSE) {
                paused = !paused;
            } else if (k.btn == BTN_DOWN || k.btn == BTN_NEXT) {
                next = 1;
            } else if (k.btn == BTN_UP) {
                next = -1;
            } else if (k.btn == BTN_LEFT) {
                vol = vol > 5 ? vol - 5 : 0;
            } else if (k.btn == BTN_RIGHT) {
                vol = vol < 150 ? vol + 5 : 150;
            } else if (k.btn == BTN_RED || k.btn == BTN_GREEN) {
                seekhold_key (&sk, k.btn == BTN_RED ? -1 : 1, k.repeat, smf_position_ms (),
                              smf_length_ms (), ub_get_timer (0));
            } else if (k.btn == BTN_YELLOW && !k.repeat) {
                if (choose_sf2 () || (engine == &synth_sf2 && !have_sf2)) {
                    engine = have_sf2 ? &synth_sf2 : &synth_opl;
                    if (!have_sf2 && !have_opl) {
                        break;
                    }
                    smf_set_synth (engine);         /* song continues on the new sounds */
                }
                draw_player (idx);
            } else if (k.btn == BTN_BLUE && have_opl && have_sf2) {
                engine = engine == &synth_sf2 ? &synth_opl : &synth_sf2;
                smf_set_synth (engine);
                draw_player (idx);
            } else if (k.btn == BTN_STOP || k.btn == BTN_BACK) {
                break;
            } else if (k.btn == BTN_HOME || k.btn == BTN_POWER) {
                engine->reset ();
                audio_stop ();
                fb_clear (&fb, TRANSPARENT);
                exit (0);
            }
            last = 0;
        }
        if (next) {
            int i = idx + next;

            engine->reset ();
            while (i >= 0 && i < nitems && (items[i].is_dir || open_song (i) < 0)) {
                i += next;
            }
            if (i < 0 || i >= nitems) {
                break;
            }
            idx = sel = i;
            sk.active = 0;
            draw_player (idx);
        }
        if (seekhold_done (&sk, ub_get_timer (0))) {
            smf_seek (sk.target_ms);
        }
        now = ub_get_timer (t0);
        if (now - last >= 50 && !sdk_screen_off) {  /* screen saver: nothing to see */
            last = now;
            draw_status ();
        }
        sdk_idle (500);
    }
    sk.active = 0;
    engine->reset ();
    audio_stop ();
}

/* ---- main ---- */

int main (int argc, char *argv[]) {
    const char *sf2_arg = 0, *dir = "/MIDI";
    struct sdk_key k;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "-sf2") && i + 1 < argc) {
            sf2_arg = argv[++i];
        } else {
            dir = argv[i];
        }
    }
    if (osd_setup (&fb) < 0) {
        printf ("midi: display not running\n");
        return 1;
    }
    fb_clear (&fb, BG);
    load_instruments (sf2_arg);

    snprintf (start_dir, sizeof (start_dir), "%s", dir[0] == '/' ? dir + 1 : dir);
    strcpy (cur_dir, start_dir);
    scan ();
    if (nitems == 0 && start_dir[0]) {
        printf ("midi: /%s empty or missing, using the stick root\n", start_dir);
        start_dir[0] = cur_dir[0] = 0;
        scan ();
    }
    draw_browser ();

    for (;;) {
        int redraw = 0;

        if (!sdk_key_poll (&k) || k.btn == BTN_NONE) {
            sdk_idle (2000);
            continue;
        }
        if (k.btn == BTN_UP && nitems) {
            sel = (sel + nitems - 1) % nitems;
            redraw = 1;
        } else if (k.btn == BTN_DOWN && nitems) {
            sel = (sel + 1) % nitems;
            redraw = 1;
        } else if ((k.btn == BTN_OK || k.btn == BTN_RIGHT || k.btn == BTN_PLAY) && nitems &&
                   !k.repeat) {
            if (items[sel].is_dir) {
                char nd[SDK_PATH_MAX];

                snprintf (nd, sizeof (nd), "%s%s%s", cur_dir, cur_dir[0] ? "/" : "", items[sel].name);
                strcpy (cur_dir, nd);
                scan ();
            } else {
                play_from (sel);
            }
            redraw = 1;
        } else if ((k.btn == BTN_BACK || k.btn == BTN_LEFT) && !k.repeat) {
            if (!strcmp (cur_dir, start_dir) || !cur_dir[0]) {
                if (k.btn == BTN_BACK) {
                    break;
                }
            } else {
                char *s = strrchr (cur_dir, '/');

                if (s) {
                    *s = 0;
                } else {
                    cur_dir[0] = 0;
                }
                scan ();
                redraw = 1;
            }
        } else if (k.btn == BTN_YELLOW && !k.repeat) {
            if (choose_sf2 ()) {
                engine = &synth_sf2;
            } else if (!have_sf2) {
                engine = have_opl ? &synth_opl : 0;
            }
            redraw = 1;
        } else if (k.btn == BTN_HOME || k.btn == BTN_POWER) {
            break;
        }
        if (redraw) {
            if (sel < top) {
                top = sel;
            } else if (sel >= top + ROWS) {
                top = sel - ROWS + 1;
            }
            draw_browser ();
        }
    }
    fb_clear (&fb, TRANSPARENT);
    return 0;
}
