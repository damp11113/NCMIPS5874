/*
 * badapple: play Bad Apple!! (or any .bav from mkbadapple.py) on the OSD
 * with its soundtrack through audio.h, streamed from the USB stick or
 * from RAM, with a debug overlay.
 *
 *   PC:     python mkbadapple.py badapple.mp4 badapple.bav
 *   Build:  wsl sh ./buildmp3.sh badapple.c badapple
 *   U-Boot: usb start
 *           source avstart.scr                     (HDMI on, sets ${a})
 *           fatload usb 0 ${a} badapple.bin
 *           go ${a} [badapple.bav] [vol=NN] [nodebug]     stream from USB
 *     or:   fatload usb 0 81600000 badapple.bav; go ${a} 81600000   from RAM
 *
 * First argument: a hex address = file already in RAM there, else the file
 * name on the stick's root directory (default badapple.bav). Streaming
 * reads through usbfat.h (U-Boot's USB storage driver, blocking reads).
 *
 * Video: 1-bit frames, delta + run-length coded (see mkbadapple.py),
 * drawn 2x (480x360 -> 960x720) centred on the 1280x720 OSD layer. Only
 * pixels that change are drawn. Audio: MP3 via mp3stream.h. The video
 * follows the audio clock: frame = samples actually played * fps / 48000.
 *
 * Debug overlay in the left black bar (on by default, 'nodebug' = off,
 * serial key 'd' toggles): frame, time, fps, A/V lag, CPU split
 * (audio decode / video draw / USB read / overlay), USB rates, audio
 * buffer. STANDBY, any other serial key or any remote key stops.
 *
 * Memory (RAM mode): the .bav sits at 0x81600000 and must end below the
 * OSD at phys 0x03000000 (~26 MB). Audio buffers at phys 0x04000000.
 */
#define AUD_BUF_PHYS    0x04000000      /* clear of the file and the OSD */

#include "board.h"
#include "audio.h"
#include "ir.h"
#include "osdsetup.h"
#include "mp3stream.h"
#include "usbfat.h"
#include "sdk/strbuf.h"

#define BAV_MAX_END     0x83000000      /* OSD header + pixels start here */
#define MAX_W           640
#define MAX_H           360
#define VBUF_SIZE       16384           /* video stream buffer (USB mode) */
#define ABUF_SIZE       16384           /* MP3 input buffer (USB mode) */
#define TICKS_PER_MS    324000          /* CP0 Count (cpuinfo) */
#define OVL_X           8
#define OVL_COLS        18
#define OVL_LINES       16

struct bav_header {
    char magic[4];
    u32 width, height, fps, frames;
    u32 audio_off, audio_len;
    u32 video_off, video_len;
};

static unsigned char bitmap[MAX_W * MAX_H];    /* current frame, 1 = white */
static struct fb fb;
static int vid_w, vid_h, x0, y0;

/* Video stream: [vptr, vend) holds the next bytes; USB mode refills it */
static const unsigned char *vptr, *vend;
static int from_usb;
static struct ufile vfile, afile;
static u32 vleft, aleft;                /* stream bytes not yet read (USB) */
static unsigned char vbuf[VBUF_SIZE] __attribute__ ((aligned (8)));
static unsigned char abuf[ABUF_SIZE] __attribute__ ((aligned (8)));
static u32 runs_total;

static inline u32 ticks (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

static u32 parse_dec (const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

static const char *after_eq (const char *s) {
    while (*s && *s != '=') {
        s++;
    }
    return *s ? s + 1 : s;
}

static int is_hex_addr (const char *s) {
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
              (*s >= 'A' && *s <= 'F') || *s == 'x' || *s == 'X')) {
            return 0;
        }
    }
    return 1;
}

static int video_refill (void) {
    u32 n;

    if (!from_usb || vleft == 0) {
        return 0;
    }
    n = ufs_read (&vfile, vbuf, vleft < VBUF_SIZE ? vleft : VBUF_SIZE);
    vleft -= n;
    vptr = vbuf;
    vend = vbuf + n;
    return n > 0;
}

static int audio_refill (unsigned char *dst, int max) {
    u32 n = (u32) max < aleft ? (u32) max : aleft;

    n = ufs_read (&afile, dst, n);
    aleft -= n;
    return n;
}

static inline u32 vbyte (void) {
    if (vptr >= vend && !video_refill ()) {
        return 0;
    }
    return *vptr++;
}

static u32 varint (void) {
    u32 v = 0;
    int shift = 0;

    for (;;) {
        u32 b = vbyte ();

        v |= (b & 0x7f) << shift;
        if (!(b & 0x80) || shift > 28) {
            return v;
        }
        shift += 7;
    }
}

/* Flip pixels [pos, pos + len) and draw them as 2x2 blocks */
static void toggle_run (u32 pos, u32 len) {
    u32 end = pos + len;
    int y, x;

    if (end > (u32) (vid_w * vid_h)) {
        end = vid_w * vid_h;
    }
    y = pos / vid_w;
    x = pos - y * vid_w;
    while (pos < end) {
        volatile u32 *row0 = (volatile u32 *) (fb.pix + (y0 + 2 * y) * fb.pitch + x0);
        volatile u32 *row1 = row0 + fb.pitch / 2;
        unsigned char *bm = bitmap + y * vid_w;

        for (; x < vid_w && pos < end; x++, pos++) {
            u32 c;

            bm[x] ^= 1;
            c = bm[x] ? 0xffffffffu : 0x80008000u;     /* white / opaque black */
            row0[x] = c;
            row1[x] = c;
        }
        x = 0;
        y++;
    }
}

/* Decode the next video frame into bitmap + screen. 0 at end of stream. */
static int next_video_frame (void) {
    u32 runs, i, pos = 0;

    if (vptr >= vend && !video_refill ()) {
        return 0;
    }
    runs = varint ();
    runs_total += runs;
    for (i = 0; i < runs; i++) {
        u32 len = varint ();

        if (i & 1) {
            toggle_run (pos, len);
        }
        pos += len;
    }
    return 1;
}

/* ---- debug overlay ---- */

struct stats {
    u32 t, audio, video, usb, osd, usb_bytes, shown, runs;
};

static void ovl_line (int line, const char *text, u16 fg) {
    char buf[OVL_COLS + 1];
    int i;

    for (i = 0; i < OVL_COLS; i++) {
        buf[i] = *text ? *text++ : ' ';
    }
    buf[i] = 0;
    fb_text (&fb, OVL_X, 16 + 20 * line, buf, 1, fg, BLACK);
}

/* Tenths of a percent of 'part' in 'whole' (both CP0 ticks) */
static u32 pct10 (u32 part, u32 whole) {
    whole >>= 10;
    return whole ? (part >> 10) * 1000 / whole : 0;
}

static void s_pct (struct str *s, u32 p10) {
    s_num (s, p10 / 10, 1);
    s_add (s, ".");
    s_num (s, p10 % 10, 1);
    s_add (s, " %");
}

static void ovl_draw (const struct stats *now, const struct stats *prev, u32 frames, u32 fps,
                      int behind, u32 lag_max) {
    u32 wall = now->t - prev->t;
    u32 ms = wall / TICKS_PER_MS;
    u32 df = now->shown - prev->shown;
    u32 busy = (now->audio - prev->audio) + (now->video - prev->video) +
               (now->usb - prev->usb) + (now->osd - prev->osd);
    u32 ub = now->usb_bytes - prev->usb_bytes;
    u32 uus = (now->usb - prev->usb) / (TICKS_PER_MS / 1000);
    u32 sec = now->shown / fps, total = frames / fps;
    struct str s;
    int l = 0;

    if (!ms) {
        return;
    }
    ovl_line (l++, "DEBUG  (d: hide)", YELLOW);
    ovl_line (l++, from_usb ? "SRC   USB stream" : "SRC   RAM", WHITE);

    s_reset (&s);
    s_add (&s, "FRAME ");
    s_num (&s, now->shown, 1);
    s_add (&s, "/");
    s_num (&s, frames, 1);
    ovl_line (l++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "TIME  ");
    s_time (&s, sec);
    s_add (&s, "/");
    s_time (&s, total);
    ovl_line (l++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "FPS   ");
    s_num (&s, df * 1000 / ms, 1);
    s_add (&s, ".");
    s_num (&s, (df * 10000 / ms) % 10, 1);
    ovl_line (l++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "A/V   ");
    if (behind < 0) {
        s_add (&s, "-");
        behind = -behind;
    }
    s_num (&s, behind, 1);
    s_add (&s, " fr late");
    ovl_line (l++, s.buf, behind > 2 ? RED : WHITE);

    s_reset (&s);
    s_add (&s, "LAG   max ");
    s_num (&s, lag_max, 1);
    s_add (&s, " fr");
    ovl_line (l++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "CPU   ");
    s_pct (&s, pct10 (busy, wall));
    ovl_line (l++, s.buf, CYAN);

    s_reset (&s);
    s_add (&s, " audio ");
    s_pct (&s, pct10 (now->audio - prev->audio, wall));
    ovl_line (l++, s.buf, CYAN);

    s_reset (&s);
    s_add (&s, " video ");
    s_pct (&s, pct10 (now->video - prev->video, wall));
    ovl_line (l++, s.buf, CYAN);

    s_reset (&s);
    s_add (&s, " usb   ");
    s_pct (&s, pct10 (now->usb - prev->usb, wall));
    ovl_line (l++, s.buf, CYAN);

    s_reset (&s);
    s_add (&s, " osd   ");
    s_pct (&s, pct10 (now->osd - prev->osd, wall));
    ovl_line (l++, s.buf, CYAN);

    s_reset (&s);
    s_add (&s, "RUNS  ");
    s_num (&s, df ? (now->runs - prev->runs) / df : 0, 1);
    s_add (&s, "/fr");
    ovl_line (l++, s.buf, WHITE);

    s_reset (&s);
    s_add (&s, "USB   ");
    s_num (&s, (ub / 1024) * 1000 / ms, 1);
    s_add (&s, " KB/s");
    ovl_line (l++, s.buf, GREEN);

    s_reset (&s);
    s_add (&s, "USBRD ");
    if (uus) {
        s_num (&s, (ub / 1024) * 1000 / (uus / 1000 ? uus / 1000 : 1), 1);
        s_add (&s, " KB/s");
    } else {
        s_add (&s, "-");
    }
    ovl_line (l++, s.buf, GREEN);

    s_reset (&s);
    s_add (&s, "ABUF  ");
    s_num (&s, ((AUD_REG (0x104) & AUD_MASK) << 3) * 100 / AUD_BUF_SIZE, 1);
    s_add (&s, " %");
    ovl_line (l++, s.buf, GREEN);
}

static void ovl_clear (void) {
    fb_rect (&fb, 0, 0, x0, fb.h, BLACK);
}

static void snap (struct stats *s, u32 audio, u32 video, u32 osd, u32 shown) {
    s->t = ticks ();
    s->audio = audio;
    s->video = video;
    s->usb = ufs_ticks;
    s->osd = osd;
    s->usb_bytes = ufs_bytes;
    s->shown = shown;
    s->runs = runs_total;
}

int main (int argc, char *argv[]) {
    const char *name = "badapple.bav";
    struct bav_header hdr;
    const struct bav_header *h = &hdr;
    u32 load = 0, vol = 100, shown = 0, lag_max = 0, last_print = 0, last_ovl = 0, start;
    u32 audio_t = 0, video_t = 0, osd_t = 0;        /* CP0 ticks, wrap: deltas only */
    u32 audio_ms = 0, video_ms = 0, usb_ms = 0;     /* totals for the summary */
    struct stats prev, now;
    struct ir_event ev;
    int i, audio_more = 1, debug = 1, stop = 0, behind = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == 'v' && argv[i][1] == 'o' && argv[i][2] == 'l') {
            vol = parse_dec (after_eq (argv[i]));
        } else if (!strcmp (argv[i], "nodebug")) {
            debug = 0;
        } else if (i == 1 && is_hex_addr (argv[i])) {
            load = parse_hex (argv[i]);
        } else {
            name = argv[i];
        }
    }
    if (vol > 100) {
        vol = 100;
    }
    mp3s_volq = vol * 256 / 100;

    if (load) {
        memcpy (&hdr, (void *) load, sizeof (hdr));
    } else {
        from_usb = 1;
        if (ufs_mount () < 0) {
            return 1;
        }
        if (ufs_open (&vfile, name) < 0 || ufs_open (&afile, name) < 0) {
            printf ("badapple: %s not found in the stick's root directory\n", name);
            return 1;
        }
        if (ufs_read (&vfile, &hdr, sizeof (hdr)) != sizeof (hdr)) {
            printf ("badapple: cannot read %s\n", name);
            return 1;
        }
        printf ("badapple: streaming %s (%d bytes) from USB\n", name, vfile.size);
    }

    if (memcmp (h->magic, "BAV1", 4) != 0) {
        printf ("badapple: no BAV1 header (%s)\n", load ? "RAM" : name);
        return 1;
    }
    if (h->width > MAX_W || h->height > MAX_H || h->width == 0 || h->height == 0 ||
        h->fps == 0 || (load && load + h->video_off + h->video_len > BAV_MAX_END)) {
        printf ("badapple: bad header (%dx%d, %d fps, end 0x%08x)\n", h->width, h->height,
                h->fps, load + h->video_off + h->video_len);
        return 1;
    }
    vid_w = h->width;
    vid_h = h->height;
    printf ("badapple: %dx%d, %d fps, %d frames (%d:%02d), video %d B, audio %d B\n",
            vid_w, vid_h, h->fps, h->frames, h->frames / h->fps / 60, h->frames / h->fps % 60,
            h->video_len, h->audio_len);

    if (osd_setup (&fb) < 0) {
        printf ("badapple: display not running: source avstart.scr first\n");
        return 1;
    }
    x0 = (fb.w - 2 * vid_w) / 2;
    y0 = (fb.h - 2 * vid_h) / 2;
    if (x0 < 0 || y0 < 0) {
        printf ("badapple: %dx%d does not fit 2x on %dx%d\n", vid_w, vid_h, fb.w, fb.h);
        return 1;
    }
    x0 &= ~1;                           /* 32-bit pixel pair stores */
    if (x0 < OVL_X + 8 * OVL_COLS) {
        debug = 0;                      /* no room for the overlay */
    }

    if (load) {
        vptr = (const unsigned char *) load + h->video_off;
        vend = vptr + h->video_len;
        i = mp3s_open ((unsigned char *) load + h->audio_off, h->audio_len);
    } else {
        ufs_seek (&vfile, h->video_off);
        vleft = h->video_len;
        vptr = vend = vbuf;
        ufs_seek (&afile, h->audio_off);
        aleft = h->audio_len;
        i = mp3s_open_stream (abuf, ABUF_SIZE, audio_refill);
    }
    if (i < 0) {
        printf ("badapple: no MP3 audio in the file\n");
        return 1;
    }
    printf ("badapple: audio %d Hz, %d ch, %d kbit/s. Stop: STANDBY, remote key, "
            "serial key ('d' = debug overlay).\n",
            mp3s_info.samprate, mp3s_info.nChans, mp3s_info.bitrate / 1000);

    fb_clear (&fb, BLACK);
    ir_init ();
    audio_start ();
    while (tstc ()) {
        getc ();
    }

    start = get_timer (0);
    snap (&prev, 0, 0, 0, 0);
    while (!stop) {
        u32 queued, played, target, t0, u0, n = 0, now_ms;

        if (standby_pressed () || ir_poll (&ev)) {
            break;
        }
        while (tstc ()) {
            if (getc () == 'd') {
                debug = !debug;
                if (!debug) {
                    ovl_clear ();
                }
            } else {
                stop = 1;
            }
        }

        if (audio_more) {
            u32 o0 = mp3s_out_frames;

            t0 = ticks ();
            u0 = ufs_ticks;
            audio_more = mp3s_pump ();
            if (mp3s_out_frames != o0) {        /* ring full = idle polling */
                audio_t += (ticks () - t0) - (ufs_ticks - u0);
            }
        }

        /* Samples actually played = written - still queued in the ring */
        queued = ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME;
        played = mp3s_out_frames > queued ? mp3s_out_frames - queued : 0;
        target = played / (AUD_RATE / h->fps);
        if (!audio_more && queued < 64) {
            target = h->frames;         /* audio over: finish the video */
        }

        while (shown < target && shown < h->frames) {
            int ok;

            t0 = ticks ();
            u0 = ufs_ticks;
            ok = next_video_frame ();
            video_t += (ticks () - t0) - (ufs_ticks - u0);
            if (!ok) {
                target = shown = h->frames;
                break;
            }
            shown++;
            n++;
            if ((n & 3) == 0 && audio_more) {
                u32 o0 = mp3s_out_frames;

                t0 = ticks ();
                u0 = ufs_ticks;
                audio_more = mp3s_pump ();     /* keep audio fed when catching up */
                if (mp3s_out_frames != o0) {
                    audio_t += (ticks () - t0) - (ufs_ticks - u0);
                }
            }
        }
        if (n > lag_max) {
            lag_max = n;
        }
        behind = (int) target - (int) shown;
        if (shown >= h->frames && !audio_more) {
            break;
        }

        now_ms = get_timer (start);
        if (now_ms - last_ovl >= 500) {
            last_ovl = now_ms;
            snap (&now, audio_t, video_t, osd_t, shown);
            audio_ms += (now.audio - prev.audio) / TICKS_PER_MS;
            video_ms += (now.video - prev.video) / TICKS_PER_MS;
            usb_ms += (now.usb - prev.usb) / TICKS_PER_MS;
            if (debug) {
                t0 = ticks ();
                ovl_draw (&now, &prev, h->frames, h->fps, behind, lag_max);
                osd_t += ticks () - t0;
            }
            prev = now;
        }
        if (now_ms - last_print >= 1000) {
            u32 s = shown / h->fps;

            last_print = now_ms;
            printf ("\r  %d:%02d  frame %d / %d ", s / 60, s % 60, shown, h->frames);
        }
    }

    mp3s_drain ();
    while (standby_pressed ()) {
        udelay (10000);
    }
    audio_stop ();
    fb_clear (&fb, TRANSPARENT);

    printf ("\nbadapple done: %d / %d frames, video %d ms, audio %d ms, USB %d ms "
            "(%d KB), max %d frames in one step\n", shown, h->frames,
            video_ms, audio_ms, usb_ms, ufs_bytes / 1024, lag_max);
    mp3s_report ("badapple");
    return 0;
}
