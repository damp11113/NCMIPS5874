/*
 * mp3play: decode an .mp3 file from RAM with the Helix fixed-point decoder
 * and play it through audio.h (HDMI + RCA), with a "now playing" screen.
 *
 *   source avstart.scr                        (HDMI video + audio; else RCA only)
 *   fatload usb 0 ${a} mp3play.bin
 *   fatload usb 0 81600000 test.mp3           (last, so ${filesize} is its size)
 *   go ${a} 81600000 ${filesize} [vol=NN] [name.mp3]
 *
 * Build (WSL): ./buildmp3.sh mp3play.c mp3play (Helix sources in helix/mp3/).
 *
 * Arguments: load address (hex, default 81600000); file size (hex, needed:
 * an mp3 has no length field); vol=NN software volume in percent (default
 * 100); an argument with a '.' is the file name, shown when the file has
 * no title tag. MPEG-1/2/2.5 layer 3, mono or stereo, any rate (resampled
 * to the hardware's 48 kHz with linear interpolation). STANDBY, a serial
 * key or any remote key stops.
 *
 * Screen (OSD, if the display runs): title / artist / album / year from
 * ID3v2.2-2.4 (else ID3v1), progress bar, format, bitrate, decode CPU load,
 * audio buffer fill, frame counts, L/R level meters. The font is ASCII
 * only: other characters show as '?'.
 *
 * Memory: the file sits at 0x81600000 (phys 0x01600000) and must end
 * below the OSD plane at phys 0x03000000, so files up to ~26 MB fit.
 * Audio buffers at phys 0x04000000.
 */
#define AUD_BUF_PHYS    0x04000000      /* clear of the file and the OSD */

#include "board.h"
#include "audio.h"
#include "ir.h"
#include "osdsetup.h"
#include "mp3stream.h"
#include "sdk/strbuf.h"

#define MP3_MAX_END     0x83000000      /* OSD header + pixels start here */
#define TAG_LEN         64

int memcmp (const void *a, const void *b, unsigned int n);    /* libc.c */

static char tag_title[TAG_LEN], tag_artist[TAG_LEN], tag_album[TAG_LEN], tag_year[8];

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

static int has_char (const char *s, char c) {
    for (; *s; s++) {
        if (*s == c) {
            return 1;
        }
    }
    return 0;
}

/* ---- ID3 tags ---- */

static u32 syncsafe (const unsigned char *p) {
    return ((p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) | ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static u32 be32 (const unsigned char *p) {
    return ((u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* Size of an ID3v2 tag at p (0 if none) */
static u32 id3v2_size (const unsigned char *p, u32 len) {
    u32 size;

    if (len < 10 || memcmp (p, "ID3", 3) != 0) {
        return 0;
    }
    size = syncsafe (p + 6) + 10;
    if (p[5] & 0x10) {
        size += 10;                     /* footer */
    }
    return size;
}

/* Copy an ID3 text frame body to ASCII: enc 0 Latin-1, 1 UTF-16 + BOM,
 * 2 UTF-16BE, 3 UTF-8. Non-ASCII characters become '?'. */
static void id3_text (char *dst, const unsigned char *p, u32 len) {
    u32 enc, i = 0, n = 0, be = 1;

    if (len < 1) {
        return;
    }
    enc = p[0];
    p++;
    len--;
    if (enc == 1 || enc == 2) {
        if (enc == 1 && len >= 2) {
            be = !(p[0] == 0xff && p[1] == 0xfe);
            if ((p[0] == 0xff && p[1] == 0xfe) || (p[0] == 0xfe && p[1] == 0xff)) {
                i = 2;
            }
        }
        for (; i + 1 < len && n < TAG_LEN - 1; i += 2) {
            u32 c = be ? (p[i] << 8) | p[i + 1] : p[i] | (p[i + 1] << 8);

            if (c == 0) {
                break;
            }
            if (c >= 0xdc00 && c < 0xe000) {
                continue;               /* low half of a surrogate pair */
            }
            dst[n++] = (c >= 32 && c < 127) ? c : '?';
        }
    } else {
        for (; i < len && n < TAG_LEN - 1; i++) {
            u32 c = p[i];

            if (c == 0) {
                break;
            }
            if (enc == 3 && (c & 0xc0) == 0x80) {
                continue;               /* UTF-8 continuation byte */
            }
            dst[n++] = (c >= 32 && c < 127) ? c : '?';
        }
    }
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = 0;
}

static void id3v2_parse (const unsigned char *p, u32 tag_size) {
    u32 ver = p[3], pos = 10, end = tag_size;

    if (p[5] & 0x10) {
        end -= 10;
    }
    if (p[5] & 0x80) {
        return;                         /* unsynchronised tag: rare, skip */
    }
    if (ver >= 3 && (p[5] & 0x40)) {
        pos += (ver == 4) ? syncsafe (p + 10) : be32 (p + 10) + 4;
    }

    while (pos + 10 <= end) {
        const unsigned char *f = p + pos;
        u32 hdr, size;
        char *dst = 0;

        if (f[0] == 0) {
            break;                      /* padding */
        }
        if (ver == 2) {
            hdr = 6;
            size = (f[3] << 16) | (f[4] << 8) | f[5];
            if (!memcmp (f, "TT2", 3)) {
                dst = tag_title;
            } else if (!memcmp (f, "TP1", 3)) {
                dst = tag_artist;
            } else if (!memcmp (f, "TAL", 3)) {
                dst = tag_album;
            } else if (!memcmp (f, "TYE", 3)) {
                dst = tag_year;
            }
        } else {
            hdr = 10;
            size = (ver == 4) ? syncsafe (f + 4) : be32 (f + 4);
            if (!memcmp (f, "TIT2", 4)) {
                dst = tag_title;
            } else if (!memcmp (f, "TPE1", 4)) {
                dst = tag_artist;
            } else if (!memcmp (f, "TALB", 4)) {
                dst = tag_album;
            } else if (!memcmp (f, "TYER", 4) || !memcmp (f, "TDRC", 4)) {
                dst = tag_year;
            }
        }
        if (size > end - pos - hdr) {
            break;
        }
        if (dst && !dst[0]) {
            char tmp[TAG_LEN];
            int i;

            tmp[0] = 0;
            id3_text (tmp, f + hdr, size);
            for (i = 0; tmp[i] && i < (dst == tag_year ? 4 : TAG_LEN - 1); i++) {
                dst[i] = tmp[i];
            }
            dst[i] = 0;
        }
        pos += hdr + size;
    }
}

/* ID3v1: last 128 bytes "TAG" title[30] artist[30] album[30] year[4] */
static int id3v1_parse (const unsigned char *p) {
    static const struct { char *dst; int off, len; } fld[] = {
        { tag_title, 3, 30 }, { tag_artist, 33, 30 }, { tag_album, 63, 30 }, { tag_year, 93, 4 },
    };
    int i, j;

    if (memcmp (p, "TAG", 3) != 0) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        unsigned char tmp[32];

        if (fld[i].dst[0]) {
            continue;                   /* ID3v2 value wins */
        }
        tmp[0] = 0;                     /* Latin-1 */
        for (j = 0; j < fld[i].len; j++) {
            tmp[j + 1] = p[fld[i].off + j];
        }
        id3_text (fld[i].dst, tmp, fld[i].len + 1);
    }
    return 1;
}

/* ---- screen ---- */

#define BG          RGB (10, 20, 60)
#define PANEL       RGB (20, 35, 90)
#define MARGIN      60
#define BAR_W       (1280 - 2 * MARGIN)
#define METER_X     (MARGIN + 40)
#define METER_W     (BAR_W - 40)

static struct fb fb;
static int have_screen;

/* Text padded with spaces to 'cols' characters (erases older, longer text) */
static void draw_field (int x, int y, const char *s, int scale, u16 fg, u16 bg, int cols) {
    char line[100];
    int i;

    for (i = 0; i < cols && i < (int) sizeof (line) - 1; i++) {
        line[i] = *s ? *s++ : ' ';
    }
    if (*s && i > 3) {
        line[i - 1] = line[i - 2] = line[i - 3] = '.';     /* cut */
    }
    line[i] = 0;
    fb_text (&fb, x, y, line, scale, fg, bg);
}

static void draw_static (const char *fallback_name) {
    struct str s;

    fb_clear (&fb, BG);
    draw_field (MARGIN, 40, "NOW PLAYING", 2, GREY, BG, 20);
    draw_field (MARGIN, 80, tag_title[0] ? tag_title : fallback_name, 4, WHITE, BG, BAR_W / 32);
    draw_field (MARGIN, 160, tag_artist[0] ? tag_artist : "Unknown artist", 3, YELLOW, BG, BAR_W / 24);

    s_reset (&s);
    s_add (&s, tag_album[0] ? tag_album : "Unknown album");
    if (tag_year[0]) {
        s_add (&s, "  (");
        s_add (&s, tag_year);
        s_add (&s, ")");
    }
    draw_field (MARGIN, 220, s.buf, 2, CYAN, BG, BAR_W / 16);

    fb_rect (&fb, MARGIN, 290, BAR_W, 20, PANEL);

    s_reset (&s);
    s_add (&s, "Format : MPEG-");
    s_add (&s, mp3s_info.version == 0 ? "1" : mp3s_info.version == 1 ? "2" : "2.5");
    s_add (&s, " Layer ");
    s_num (&s, mp3s_info.layer, 1);
    s_add (&s, ", ");
    s_num (&s, mp3s_info.samprate, 1);
    s_add (&s, " Hz, ");
    s_add (&s, mp3s_info.nChans == 2 ? "stereo" : "mono");
    s_add (&s, " -> 48000 Hz");
    draw_field (MARGIN, 390, s.buf, 2, WHITE, BG, 70);

    fb_text (&fb, MARGIN, 580, "L", 2, WHITE, BG);
    fb_text (&fb, MARGIN, 615, "R", 2, WHITE, BG);
    fb_rect (&fb, METER_X, 580, METER_W, 24, PANEL);
    fb_rect (&fb, METER_X, 615, METER_W, 24, PANEL);

    draw_field (MARGIN, 670, "STANDBY or any remote key: stop", 2, GREY, BG, 70);
}

/* Level meter: peak 0..32767, log-ish scale (green / yellow / red) */
static void draw_meter (int y, int peak) {
    int w = 0, v = peak, x;

    /* ~ dB: each halving of the level takes 1/12 of the width (72 dB) */
    if (v > 0) {
        int steps = 0;

        while (v < 16384 && steps < 12) {
            v <<= 1;
            steps++;
        }
        if (steps < 12) {
            w = METER_W * (11 - steps) / 12 + (METER_W / 12) * (v - 16384) / 16384;
        }
    }
    for (x = 0; x < METER_W; x += 8) {
        u16 c = x >= w ? PANEL : x > METER_W * 11 / 12 ? RED :
                x > METER_W * 9 / 12 ? YELLOW : GREEN;

        fb_rect (&fb, METER_X + x, y, 6, 24, c);
    }
}

static void draw_status (u32 elapsed_s, u32 total_s, u32 cpu10, u32 buf_pct) {
    struct str s;
    int w;

    if (total_s) {
        w = BAR_W * (elapsed_s < total_s ? elapsed_s : total_s) / total_s;
        fb_rect (&fb, MARGIN, 290, w, 20, CYAN);
        fb_rect (&fb, MARGIN + w, 290, BAR_W - w, 20, PANEL);
    }
    s_reset (&s);
    s_time (&s, elapsed_s);
    s_add (&s, " / ");
    if (total_s) {
        s_time (&s, total_s);
    } else {
        s_add (&s, "?:??");
    }
    draw_field (MARGIN, 320, s.buf, 2, WHITE, BG, 20);

    s_reset (&s);
    s_add (&s, "Bitrate: ");
    s_num (&s, mp3s_info.bitrate / 1000, 1);
    s_add (&s, " kbit/s");
    draw_field (MARGIN, 425, s.buf, 2, WHITE, BG, 40);

    s_reset (&s);
    s_add (&s, "CPU    : ");
    s_num (&s, cpu10 / 10, 1);
    s_add (&s, ".");
    s_num (&s, cpu10 % 10, 1);
    s_add (&s, " % decode (MIPS 24KEc ~648 MHz)");
    draw_field (MARGIN, 460, s.buf, 2, WHITE, BG, 60);

    s_reset (&s);
    s_add (&s, "Buffer : ");
    s_num (&s, buf_pct, 1);
    s_add (&s, " %");
    draw_field (MARGIN, 495, s.buf, 2, WHITE, BG, 40);

    s_reset (&s);
    s_add (&s, "Frames : ");
    s_num (&s, mp3s_frames_ok, 1);
    s_add (&s, "   bad: ");
    s_num (&s, mp3s_errors, 1);
    draw_field (MARGIN, 530, s.buf, 2, WHITE, BG, 40);
}


int main (int argc, char *argv[]) {
    u32 load = (argc > 1) ? parse_hex (argv[1]) : 0x81600000;
    u32 fsize = 0, vol = 100, skip, data_len;
    u32 last = 0, start, total_s = 0;
    u32 last_dec_ms = 0, last_out = 0, cpu10 = 0;
    unsigned char *file = (unsigned char *) load;
    const char *name = "Unknown title";
    struct ir_event ev;
    int i, more = 1;

    for (i = 2; i < argc; i++) {
        if (argv[i][0] == 'v' && argv[i][1] == 'o' && argv[i][2] == 'l') {
            vol = parse_dec (after_eq (argv[i]));
        } else if (has_char (argv[i], '.')) {
            name = argv[i];
        } else {
            fsize = parse_hex (argv[i]);
        }
    }
    if (!fsize) {
        printf ("mp3play: file size needed\n");
        printf ("  fatload usb 0 81600000 test.mp3; go ${a} 81600000 ${filesize}\n");
        return 1;
    }
    if (load + fsize > MP3_MAX_END) {
        fsize = MP3_MAX_END - load;
        printf ("mp3play: file too big, playing the first %d bytes\n", fsize);
    }
    if (vol > 100) {
        vol = 100;
    }
    mp3s_volq = vol * 256 / 100;

    skip = id3v2_size (file, fsize);
    if (skip >= fsize) {
        printf ("mp3play: nothing after the ID3 tag\n");
        return 1;
    }
    if (skip) {
        id3v2_parse (file, skip);
    }
    data_len = fsize - skip;
    if (data_len >= 128 && id3v1_parse (file + fsize - 128)) {
        data_len -= 128;
    }
    printf ("mp3play: title  \"%s\"\n", tag_title[0] ? tag_title : name);
    printf ("mp3play: artist \"%s\", album \"%s\", year \"%s\"\n",
            tag_artist, tag_album, tag_year);

    if (mp3s_open (file + skip, data_len) < 0) {
        printf ("mp3play: no MPEG audio frame found at 0x%08x\n", load);
        return 1;
    }
    if (mp3s_info.bitrate > 0) {
        total_s = data_len / (mp3s_info.bitrate / 8);
    }
    printf ("mp3play: MPEG%s layer %d, %d ch, %d Hz, %d kbit/s, ~%d:%02d (CBR estimate)\n",
            mp3s_info.version == 0 ? "1" : mp3s_info.version == 1 ? "2" : "2.5",
            mp3s_info.layer, mp3s_info.nChans, mp3s_info.samprate,
            mp3s_info.bitrate / 1000, total_s / 60, total_s % 60);
    printf ("mp3play: volume %d%%. Stop: STANDBY, serial key or remote key.\n", vol);

    have_screen = osd_setup (&fb) == 0;
    if (have_screen) {
        draw_static (name);
        draw_status (0, total_s, 0, 0);
    } else {
        printf ("mp3play: display not running (source avstart.scr), console only\n");
    }

    ir_init ();
    audio_start ();
    while (tstc ()) {
        getc ();
    }

    start = get_timer (0);
    while (more && !standby_pressed () && !tstc () && !ir_poll (&ev)) {
        u32 now;

        more = mp3s_pump ();

        /* Screen every 100 ms (meters), text + console every second.
         * The audio ring holds ~340 ms, so short drawing is safe. */
        now = get_timer (start);
        if (now - last >= 100) {
            u32 t = mp3s_out_frames / AUD_RATE;
            int full_update = now / 1000 != last / 1000;

            last = now;
            if (have_screen) {
                draw_meter (580, mp3s_peak_l);
                draw_meter (615, mp3s_peak_r);
            }
            mp3s_peak_l = mp3s_peak_r = 0;

            if (full_update) {
                u32 audio_ms = (mp3s_out_frames - last_out) / (AUD_RATE / 1000);
                u32 buf_pct = ((AUD_REG (0x104) & AUD_MASK) << 3) * 100 / AUD_BUF_SIZE;

                if (audio_ms) {
                    cpu10 = (mp3s_dec_ms - last_dec_ms) * 1000 / audio_ms;
                }
                last_dec_ms = mp3s_dec_ms;
                last_out = mp3s_out_frames;
                if (have_screen) {
                    draw_status (t, total_s, cpu10, buf_pct);
                }
                printf ("\r  %d:%02d / ~%d:%02d  %3d kbit/s  CPU %d.%d %% ", t / 60, t % 60,
                        total_s / 60, total_s % 60, mp3s_info.bitrate / 1000,
                        cpu10 / 10, cpu10 % 10);
            }
        }
    }

    mp3s_drain ();
    if (tstc ()) {
        getc ();
    }
    while (standby_pressed ()) {
        udelay (10000);
    }

    audio_stop ();
    if (have_screen) {
        fb_clear (&fb, TRANSPARENT);
    }
    printf ("\n");
    mp3s_report ("mp3play done");
    return 0;
}
