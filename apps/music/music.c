/*
 * Music player for the NC5874 box (SDK app, NCAPPS/APPS/MUSIC).
 *
 * Plays MP3 (Helix decoder, mp3stream.h) and WAV (PCM 8/16/24-bit, mono /
 * stereo, any rate) streamed from the USB stick, resampled to 48 kHz.
 * Shows title / artist / album / year (ID3v2.2-2.4, ID3v1, WAV LIST INFO)
 * and the album cover: the ID3 APIC / PIC picture, else cover.jpg,
 * folder.jpg, front.jpg or cover.png in the song's folder (JPEG / PNG via
 * stb_image).
 *
 * Start folder: first argument (INF args), default /MUSIC, else the stick
 * root. Browser: UP / DOWN select, OK open folder / play, BACK folder up
 * (quit at the start folder), HOME / POWER quit.
 * Playing: OK / PLAY / PAUSE pause, UP previous, DOWN / NEXT next track,
 * LEFT / RIGHT volume, RED / GREEN seek back / forward (tap = 2 s, hold =
 * faster and faster, jumps on release; sdk/seekhold.h), STOP / BACK to the
 * list. Plays on through the folder.
 */
#define BOX_WANT_AUDIO
#include "sdk.h"
#include "mp3stream.h"
#include "third_party/stb_image.h"
#include "seekhold.h"

#define BG          RGB(12, 18, 40)
#define PANEL       RGB(24, 34, 72)
#define HILITE      RGB(60, 110, 200)
#define METER_OFF   RGB(30, 40, 80)

#define MAX_ITEMS   512
#define ROWS        13
#define ROW_H       40
#define COVER       320
#define TARGET_Q    8192            /* frames queued in the audio ring (170 ms) */
#define TAG_LEN     80

static struct fb fb;

/* ---- browser ---- */

struct item {
    char name[100];
    int is_dir;
    u32 size;
};

static struct item items[MAX_ITEMS];
static int nitems, sel, top;
static char cur_dir[SDK_PATH_MAX], start_dir[SDK_PATH_MAX];

static int has_ext(const char *n, const char *ext) {
    int a = strlen(n), b = strlen(ext);

    return a > b && !strcasecmp(n + a - b, ext);
}

static int is_song(const char *n) {
    return has_ext(n, ".mp3") || has_ext(n, ".wav");
}

static int item_cmp(const struct item *a, const struct item *b) {
    if (a->is_dir != b->is_dir) {
        return b->is_dir - a->is_dir;           /* folders first */
    }
    return strcasecmp(a->name, b->name);
}

static void scan(void) {
    struct sdk_dirent e;
    char path[SDK_PATH_MAX];
    int h, i, j;

    snprintf(path, sizeof(path), "/%s", cur_dir);     /* from the stick root */
    h = sdk_dir_open(path);

    nitems = 0;
    sel = top = 0;
    if (h < 0) {
        return;
    }
    while (sdk_dir_read(h, &e) && nitems < MAX_ITEMS) {
        if (e.name[0] == '.' || (!e.is_dir && !is_song(e.name))) {
            continue;
        }
        strncpy(items[nitems].name, e.name, sizeof(items[0].name) - 1);
        items[nitems].name[sizeof(items[0].name) - 1] = 0;
        items[nitems].is_dir = e.is_dir;
        items[nitems].size = e.size;
        nitems++;
    }
    sdk_dir_close(h);
    for (i = 1; i < nitems; i++) {
        for (j = i; j > 0 && item_cmp(&items[j - 1], &items[j]) > 0; j--) {
            struct item t = items[j];

            items[j] = items[j - 1];
            items[j - 1] = t;
        }
    }
}

static void text(int x, int y, const char *s, int scale, u16 fg, u16 bg, int max) {
    char buf[128];
    int n = strlen(s);

    if (n > max) {
        n = max;
    }
    if (n > 127) {
        n = 127;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
    if (n == max && (int) strlen(s) > max && n > 3) {
        buf[n - 1] = buf[n - 2] = buf[n - 3] = '.';
    }
    fb_text(&fb, x, y, buf, scale, fg, bg);
}

static void draw_browser(void) {
    int i;

    fb_clear(&fb, BG);
    fb_rect(&fb, 0, 0, fb.w, 100, PANEL);
    fb_text(&fb, 40, 16, "MUSIC", 3, WHITE, TRANSPARENT);
    text(40, 64, cur_dir[0] ? cur_dir : "/", 2, GREY, TRANSPARENT, 74);
    if (nitems == 0) {
        fb_text(&fb, 60, 140, "No folders or .mp3 / .wav files here", 2, YELLOW, TRANSPARENT);
    }
    for (i = 0; i < ROWS && top + i < nitems; i++) {
        const struct item *it = &items[top + i];
        int y = 130 + i * ROW_H, on = top + i == sel;
        char line[120];

        if (on) {
            fb_rect(&fb, 30, y - 6, fb.w - 60, ROW_H - 4, HILITE);
        }
        snprintf(line, sizeof(line), "%s%s", it->is_dir ? "[" : "", it->name);
        if (it->is_dir) {
            strncat(line, "]", sizeof(line) - strlen(line) - 1);
        }
        text(50, y, line, 2, it->is_dir ? YELLOW : (on ? WHITE : RGB(200, 210, 230)),
              TRANSPARENT, 60);
        if (!it->is_dir) {
            char sz[24];

            snprintf(sz, sizeof(sz), "%d.%d MB", it->size >> 20, (it->size >> 16) * 10 / 16 % 10);
            fb_text(&fb, fb.w - 200, y, sz, 2, GREY, TRANSPARENT);
        }
    }
    fb_rect(&fb, 0, 680, fb.w, 40, PANEL);
    fb_text(&fb, 40, 688, "OK open/play   BACK folder up   HOME quit", 2, GREY, TRANSPARENT);
}

/* ---- tags ---- */

struct song {
    char path[SDK_PATH_MAX];
    char title[TAG_LEN], artist[TAG_LEN], album[TAG_LEN], year[8];
    int is_mp3;
    int h;                          /* stream handle */
    u32 data_start, data_end;       /* audio bytes in the file */
    u32 rate, chans, bits, bitrate, duration_s;
    unsigned char *pic;             /* embedded cover (malloc), NULL = none */
    u32 pic_len;
};

static struct song song;

static u32 be32(const unsigned char *p) {
    return ((u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static u32 le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static u32 le16(const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static u32 syncsafe(const unsigned char *p) {
    return ((p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) | ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

/* ID3 text (enc 0 Latin-1, 1 UTF-16 BOM, 2 UTF-16BE, 3 UTF-8) -> ASCII */
static void id3_text(char *dst, int size, const unsigned char *p, u32 len) {
    u32 enc, i = 0;
    int n = 0, be = 1;

    if (len < 1) {
        return;
    }
    enc = *p++;
    len--;
    if (enc == 1 || enc == 2) {
        if (enc == 1 && len >= 2) {
            be = !(p[0] == 0xff && p[1] == 0xfe);
            if ((p[0] == 0xff && p[1] == 0xfe) || (p[0] == 0xfe && p[1] == 0xff)) {
                i = 2;
            }
        }
        for (; i + 1 < len && n < size - 1; i += 2) {
            u32 c = be ? (p[i] << 8) | p[i + 1] : p[i] | (p[i + 1] << 8);

            if (c == 0) {
                break;
            }
            if (c >= 0xdc00 && c < 0xe000) {
                continue;
            }
            dst[n++] = (c >= 32 && c < 127) ? c : '?';
        }
    } else {
        for (; i < len && n < size - 1; i++) {
            u32 c = p[i];

            if (c == 0) {
                break;
            }
            if (enc == 3 && (c & 0xc0) == 0x80) {
                continue;
            }
            dst[n++] = (c >= 32 && c < 127) ? c : '?';
        }
    }
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = 0;
}

/* APIC (v2.3/2.4): enc, mime\0, type, desc (enc-terminated), data
 * PIC (v2.2): enc, 3-char format, type, desc, data */
static void id3_picture(const unsigned char *p, u32 len, int v22) {
    u32 i, enc;

    if (song.pic || len < 4) {
        return;
    }
    enc = p[0];
    i = 1;
    if (v22) {
        i += 3;
    } else {
        while (i < len && p[i]) {
            i++;
        }
        i++;
    }
    i++;                                        /* picture type */
    if (enc == 1 || enc == 2) {                 /* UTF-16 description */
        while (i + 1 < len && (p[i] || p[i + 1])) {
            i += 2;
        }
        i += 2;
    } else {
        while (i < len && p[i]) {
            i++;
        }
        i++;
    }
    if (i >= len) {
        return;
    }
    song.pic = malloc(len - i);
    if (song.pic) {
        memcpy(song.pic, p + i, len - i);
        song.pic_len = len - i;
    }
}

static void id3v2_parse(const unsigned char *p, u32 size) {
    u32 ver = p[3], pos = 10, end = size;

    if (p[5] & 0x80) {
        return;                                 /* unsynchronised: skip */
    }
    if (ver >= 3 && (p[5] & 0x40)) {
        pos += (ver == 4) ? syncsafe(p + 10) : be32(p + 10) + 4;
    }
    while (pos + 10 <= end) {
        const unsigned char *f = p + pos;
        u32 hdr = ver == 2 ? 6 : 10, fs;

        if (f[0] == 0) {
            break;
        }
        fs = ver == 2 ? (u32) ((f[3] << 16) | (f[4] << 8) | f[5]) :
             ver == 4 ? syncsafe(f + 4) : be32(f + 4);
        if (fs > end - pos - hdr) {
            break;
        }
        f += hdr;
        if (ver == 2) {
            const unsigned char *id = p + pos;

            if (!memcmp(id, "TT2", 3) && !song.title[0]) {
                id3_text(song.title, TAG_LEN, f, fs);
            } else if (!memcmp(id, "TP1", 3) && !song.artist[0]) {
                id3_text(song.artist, TAG_LEN, f, fs);
            } else if (!memcmp(id, "TAL", 3) && !song.album[0]) {
                id3_text(song.album, TAG_LEN, f, fs);
            } else if (!memcmp(id, "TYE", 3) && !song.year[0]) {
                id3_text(song.year, 5, f, fs);
            } else if (!memcmp(id, "PIC", 3)) {
                id3_picture(f, fs, 1);
            }
        } else {
            const unsigned char *id = p + pos;

            if (!memcmp(id, "TIT2", 4) && !song.title[0]) {
                id3_text(song.title, TAG_LEN, f, fs);
            } else if (!memcmp(id, "TPE1", 4) && !song.artist[0]) {
                id3_text(song.artist, TAG_LEN, f, fs);
            } else if (!memcmp(id, "TALB", 4) && !song.album[0]) {
                id3_text(song.album, TAG_LEN, f, fs);
            } else if ((!memcmp(id, "TYER", 4) || !memcmp(id, "TDRC", 4)) && !song.year[0]) {
                id3_text(song.year, 5, f, fs);
            } else if (!memcmp(id, "APIC", 4)) {
                id3_picture(f, fs, 0);
            }
        }
        pos += hdr + fs;
    }
}

static void id3v1_parse(const unsigned char *t) {
    static const struct { int off, len, which; } f[] = {
        { 3, 30, 0 }, { 33, 30, 1 }, { 63, 30, 2 }, { 93, 4, 3 },
    };
    unsigned int i;

    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        char *dst = f[i].which == 0 ? song.title : f[i].which == 1 ? song.artist :
                    f[i].which == 2 ? song.album : song.year;
        unsigned char tmp[32];
        int j;

        if (dst[0]) {
            continue;
        }
        tmp[0] = 0;
        for (j = 0; j < f[i].len; j++) {
            tmp[j + 1] = t[f[i].off + j];
        }
        id3_text(dst, f[i].which == 3 ? 5 : TAG_LEN, tmp, f[i].len + 1);
    }
}

/* ---- cover picture ---- */

static u16 *cover;                  /* COVER x COVER ARGB1555 (malloc) */

static void cover_from(const unsigned char *data, u32 len) {
    int w, h, n, x, y;
    unsigned char *rgb;

    if (cover || !data || len < 16) {
        return;
    }
    rgb = stbi_load_from_memory(data, len, &w, &h, &n, 3);
    if (!rgb) {
        printf("music: cover not decoded (%s)\n", stbi_failure_reason());
        return;
    }
    cover = malloc(COVER * COVER * 2);
    if (cover) {
        int size = w > h ? w : h;               /* fit, keep aspect, centre */
        int ox = (size - w) / 2, oy = (size - h) / 2;

        for (y = 0; y < COVER; y++) {
            for (x = 0; x < COVER; x++) {
                /* average the source area of this output pixel */
                int sx0 = x * size / COVER - ox, sx1 = (x + 1) * size / COVER - ox;
                int sy0 = y * size / COVER - oy, sy1 = (y + 1) * size / COVER - oy;
                u32 r = 0, g = 0, b = 0, cnt = 0;
                int sx, sy;
                u16 c;

                if (sx1 <= sx0) {
                    sx1 = sx0 + 1;
                }
                if (sy1 <= sy0) {
                    sy1 = sy0 + 1;
                }
                for (sy = sy0; sy < sy1; sy += 1 + (sy1 - sy0) / 4) {
                    for (sx = sx0; sx < sx1; sx += 1 + (sx1 - sx0) / 4) {
                        if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                            const unsigned char *px = rgb + 3 * (sy * w + sx);

                            r += px[0];
                            g += px[1];
                            b += px[2];
                        }
                        cnt++;
                    }
                }
                c = RGB(r / cnt, g / cnt, b / cnt);
                cover[y * COVER + x] = c == 0x801f ? 0x801e : c;
            }
        }
    }
    stbi_image_free(rgb);
}

static void cover_from_folder(const char *dir) {
    static const char *names[] = { "cover.jpg", "folder.jpg", "front.jpg", "cover.png",
                                   "folder.png", "AlbumArtSmall.jpg" };
    unsigned int i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]) && !cover; i++) {
        char p[SDK_PATH_MAX];
        long len;

        snprintf(p, sizeof(p), "/%s/%s", dir, names[i]);
        len = sdk_file_size(p);
        if (len > 0 && len < (8 << 20)) {
            unsigned char *d = malloc(len);

            if (d && sdk_read_file(p, d, len) == len) {
                cover_from(d, len);
            }
            free(d);
        }
    }
}

/* ---- playback ---- */

static int playing, paused, vol = 80;
static struct seekhold sk;
static u32 out_frames;              /* 48 kHz frames written for this song */

/* WAV: stereo 16-bit frames decoded from the file, resampled like mp3stream */
static short wav_in[2 * (1024 + 2)];
static unsigned char wav_raw[1024 * 6];
static u32 wav_have, wav_pos, wav_frac, wav_step, wav_eof;
static short mix[512 * 2];
static int peak_l, peak_r;

static int mp3_refill(unsigned char *dst, int max) {
    u32 pos = sdk_tell(song.h), left = pos < song.data_end ? song.data_end - pos : 0;

    if ((u32) max > left) {
        max = left;
    }
    return max > 0 ? sdk_read(song.h, dst, max) : 0;
}

static unsigned char mp3_buf[16384] __attribute__((aligned(8)));

/* Decode the next WAV block after keeping the last frame; 0 at the end */
static u32 wav_fill(void) {
    u32 block = song.chans * (song.bits / 8), pos = sdk_tell(song.h), keep, n, i;
    long got;

    keep = wav_have > wav_pos ? wav_have - wav_pos : 0;
    for (i = 0; i < 2 * keep; i++) {
        wav_in[i] = wav_in[2 * wav_pos + i];
    }
    wav_have = keep;
    wav_pos = 0;
    n = sizeof(wav_raw) / block;
    if (pos + n * block > song.data_end) {
        n = pos < song.data_end ? (song.data_end - pos) / block : 0;
    }
    if (n > 1024) {
        n = 1024;
    }
    got = n ? sdk_read(song.h, wav_raw, n * block) : 0;
    n = got > 0 ? (u32) got / block : 0;
    for (i = 0; i < n; i++) {
        const unsigned char *p = wav_raw + i * block;
        int l, r;

        if (song.bits == 8) {
            l = (p[0] - 128) << 8;
            r = song.chans == 2 ? (p[1] - 128) << 8 : l;
        } else if (song.bits == 16) {
            l = (short) le16(p);
            r = song.chans == 2 ? (short) le16(p + 2) : l;
        } else {                                /* 24-bit */
            l = (short) (p[1] | (p[2] << 8));
            r = song.chans == 2 ? (short) (p[4] | (p[5] << 8)) : l;
        }
        wav_in[2 * (wav_have + i)] = l;
        wav_in[2 * (wav_have + i) + 1] = r;
    }
    wav_have += n;
    return n;
}

static void track_peak(int l, int r) {
    if (l < 0) {
        l = -l;
    }
    if (r < 0) {
        r = -r;
    }
    if (l > peak_l) {
        peak_l = l;
    }
    if (r > peak_r) {
        peak_r = r;
    }
}

/* Paused: keep the ring fed with zeros. An empty ring makes the output
 * hold its last sample, a DC offset on HDMI / RCA. */
static void feed_silence(void) {
    u32 queued = ((AUD_REG(0x104) & AUD_MASK) << 3) / AUD_FRAME, n;

    if (queued >= TARGET_Q) {
        return;
    }
    n = TARGET_Q - queued > 512 ? 512 : TARGET_Q - queued;
    memset(mix, 0, n * 4);
    audio_write(mix, n);
}

/* Top the audio ring up to TARGET_Q frames. 0 when the song has ended. */
static int pump(void) {
    u32 queued = ((AUD_REG(0x104) & AUD_MASK) << 3) / AUD_FRAME, want, k = 0;

    if (!playing) {
        return 1;
    }
    if (paused) {
        feed_silence();
        return 1;
    }
    if (queued >= TARGET_Q) {
        return 1;
    }
    want = TARGET_Q - queued;
    if (want > 512) {
        want = 512;
    }
    if (song.is_mp3) {
        u32 before = mp3s_out_frames;
        int more;

        mp3s_volq = vol * 256 / 100;
        more = mp3s_pump();
        out_frames += mp3s_out_frames - before;
        track_peak(mp3s_peak_l, mp3s_peak_r);
        mp3s_peak_l = mp3s_peak_r = 0;
        return more || queued > 64;
    }
    while (k < want) {
        const short *a, *b;
        int l, r;

        if (wav_pos + 1 >= wav_have) {
            if (wav_eof || !wav_fill()) {
                wav_eof = 1;
                break;
            }
            continue;
        }
        a = wav_in + 2 * wav_pos;
        b = a + 2;
        l = a[0] + (((b[0] - a[0]) * (int) wav_frac) >> 16);
        r = a[1] + (((b[1] - a[1]) * (int) wav_frac) >> 16);
        l = l * vol / 100;
        r = r * vol / 100;
        track_peak(l, r);
        mix[2 * k] = r;                         /* audio.h: R = hi, L = lo */
        mix[2 * k + 1] = l;
        k++;
        wav_frac += wav_step;
        wav_pos += wav_frac >> 16;
        wav_frac &= 0xffff;
    }
    if (k) {
        audio_write(mix, k);
        out_frames += k;
    }
    return !wav_eof || queued > 64;
}

static void close_song(void) {
    if (song.h >= 0) {
        sdk_close(song.h);
    }
    song.h = -1;
    free(song.pic);
    song.pic = 0;
    free(cover);
    cover = 0;
    playing = 0;
}

/* Xing / Info header in the first frame: exact length of VBR files */
static u32 xing_frames(const unsigned char *p, u32 len) {
    u32 i;

    for (i = 4; i + 12 < len && i < 200; i++) {
        if (!memcmp(p + i, "Xing", 4) || !memcmp(p + i, "Info", 4)) {
            return (be32(p + i + 4) & 1) ? be32(p + i + 8) : 0;
        }
    }
    return 0;
}

static int open_mp3(void) {
    unsigned char hdr[10];
    u32 size = sdk_size(song.h), start = 0, frames;
    unsigned char probe[512];

    if (sdk_read(song.h, hdr, 10) == 10 && !memcmp(hdr, "ID3", 3)) {
        u32 tag = syncsafe(hdr + 6) + 10 + ((hdr[5] & 0x10) ? 10 : 0);

        if (tag < (8u << 20)) {
            unsigned char *t = malloc(tag);

            if (t) {
                memcpy(t, hdr, 10);
                if (sdk_read(song.h, t + 10, tag - 10) == (long) (tag - 10)) {
                    id3v2_parse(t, tag);
                }
                free(t);
            }
        }
        start = tag;
    }
    song.data_start = start;
    song.data_end = size;
    if (size > 128) {
        unsigned char t[128];

        sdk_seek(song.h, size - 128);
        if (sdk_read(song.h, t, 128) == 128 && !memcmp(t, "TAG", 3)) {
            id3v1_parse(t);
            song.data_end = size - 128;
        }
    }
    sdk_seek(song.h, start);
    frames = sdk_read(song.h, probe, sizeof(probe)) > 0 ? xing_frames(probe, sizeof(probe)) : 0;
    sdk_seek(song.h, start);

    helix_used = 0;                             /* new decoder each song */
    mp3s_frames_ok = mp3s_errors = 0;
    mp3s_out_frames = 0;
    mp3s_have = mp3s_pos = mp3s_frac = 0;
    if (mp3s_open_stream(mp3_buf, sizeof(mp3_buf), mp3_refill) < 0) {
        return -1;
    }
    song.rate = mp3s_info.samprate;
    song.chans = mp3s_info.nChans;
    song.bits = 16;
    song.bitrate = mp3s_info.bitrate;
    if (frames && song.rate) {
        song.duration_s = frames * (mp3s_info.version == 0 ? 1152 : 576) / song.rate;
        song.bitrate = song.duration_s ? (u32) ((unsigned long long) (song.data_end - start) * 8 /
                                                 song.duration_s) : song.bitrate;
    } else if (song.bitrate) {
        song.duration_s = (song.data_end - start) / (song.bitrate / 8);
    }
    return 0;
}

static int open_wav(void) {
    unsigned char c[12];
    u32 pos = 12, size = sdk_size(song.h);
    int fmt = 0;

    if (sdk_read(song.h, c, 12) != 12 || memcmp(c, "RIFF", 4) || memcmp(c + 8, "WAVE", 4)) {
        return -1;
    }
    while (pos + 8 <= size) {
        u32 len;

        sdk_seek(song.h, pos);
        if (sdk_read(song.h, c, 8) != 8) {
            break;
        }
        len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char f[16];

            sdk_read(song.h, f, 16);
            fmt = le16(f);
            song.chans = le16(f + 2);
            song.rate = le32(f + 4);
            song.bits = le16(f + 14);
        } else if (!memcmp(c, "data", 4)) {
            song.data_start = pos + 8;
            song.data_end = pos + 8 + len < size ? pos + 8 + len : size;
        } else if (!memcmp(c, "LIST", 4) && len < 65536) {
            unsigned char *l = malloc(len);
            u32 i;

            if (l && sdk_read(song.h, l, len) == (long) len && !memcmp(l, "INFO", 4)) {
                for (i = 4; i + 8 <= len;) {
                    u32 sl = le32(l + i + 4);
                    char *dst = !memcmp(l + i, "INAM", 4) ? song.title :
                                !memcmp(l + i, "IART", 4) ? song.artist :
                                !memcmp(l + i, "IPRD", 4) ? song.album :
                                !memcmp(l + i, "ICRD", 4) ? song.year : 0;

                    if (i + 8 + sl > len) {
                        break;
                    }
                    if (dst) {
                        unsigned char tmp[TAG_LEN + 1];
                        u32 n = sl < TAG_LEN ? sl : TAG_LEN;

                        tmp[0] = 0;
                        memcpy(tmp + 1, l + i + 8, n);
                        id3_text(dst, dst == song.year ? 5 : TAG_LEN, tmp, n + 1);
                    }
                    i += 8 + ((sl + 1) & ~1u);
                }
            }
            free(l);
        }
        pos += 8 + ((len + 1) & ~1u);
    }
    if ((fmt != 1 && fmt != 0xfffe) || !song.data_start || song.chans < 1 || song.chans > 2 ||
        (song.bits != 8 && song.bits != 16 && song.bits != 24) || song.rate < 4000 ||
        song.rate > 192000) {
        printf("music: unsupported WAV (format %d, %d ch, %d bit, %d Hz)\n", fmt, song.chans,
                song.bits, song.rate);
        return -1;
    }
    song.bitrate = song.rate * song.chans * song.bits;
    song.duration_s = (song.data_end - song.data_start) / (song.rate * song.chans * song.bits / 8);
    sdk_seek(song.h, song.data_start);
    wav_have = wav_pos = wav_frac = wav_eof = 0;
    wav_step = (u32) (((unsigned long long) song.rate << 16) / AUD_RATE);
    return 0;
}

static int open_song(const char *name) {
    const char *base;

    close_song();
    memset(song.title, 0, sizeof(song.title));
    memset(song.artist, 0, sizeof(song.artist));
    memset(song.album, 0, sizeof(song.album));
    memset(song.year, 0, sizeof(song.year));
    song.duration_s = song.bitrate = 0;
    snprintf(song.path, sizeof(song.path), "/%s%s%s", cur_dir, cur_dir[0] ? "/" : "", name);
    song.is_mp3 = has_ext(name, ".mp3");
    song.h = sdk_open(song.path);
    if (song.h < 0) {
        printf("music: cannot open %s\n", song.path);
        return -1;
    }
    if ((song.is_mp3 ? open_mp3() : open_wav()) < 0) {
        close_song();
        return -1;
    }
    if (!song.title[0]) {
        base = name;
        snprintf(song.title, sizeof(song.title), "%s", base);
        if (strrchr(song.title, '.')) {
            *strrchr(song.title, '.') = 0;
        }
    }
    cover_from(song.pic, song.pic_len);
    cover_from_folder(cur_dir);
    out_frames = 0;
    paused = 0;
    playing = 1;
    printf("music: %s - %s (%s, %d Hz, %d ch, %d kbit/s, %d:%02d)\n", song.artist, song.title,
            song.is_mp3 ? "MP3" : "WAV", song.rate, song.chans, song.bitrate / 1000,
            song.duration_s / 60, song.duration_s % 60);
    return 0;
}

static void seek_to(int secs) {
    u32 pos;

    if (secs < 0) {
        secs = 0;
    }
    if (song.duration_s && (u32) secs >= song.duration_s) {
        secs = song.duration_s - 1;
    }
    if (song.is_mp3) {
        pos = song.data_start + (u32) ((unsigned long long) (song.data_end - song.data_start) *
                                        secs / (song.duration_s ? song.duration_s : 1));
        sdk_seek(song.h, pos);
        mp3s_restart();
    } else {
        u32 block = song.chans * song.bits / 8;

        pos = song.data_start + (u32) secs * song.rate * block;
        sdk_seek(song.h, pos);
        wav_have = wav_pos = wav_frac = wav_eof = 0;
    }
    out_frames = (u32) secs * AUD_RATE;
}

/* ---- player screen ---- */

static void draw_cover(void) {
    int x0 = 60, y0 = 130, x, y;

    if (!cover) {
        fb_rect(&fb, x0, y0, COVER, COVER, PANEL);
        fb_text(&fb, x0 + 120, y0 + 110, "~", 8, GREY, TRANSPARENT);
        return;
    }
    for (y = 0; y < COVER; y++) {
        volatile u16 *row = fb.pix + (y0 + y) * fb.pitch + x0;

        for (x = 0; x < COVER; x++) {
            row[x] = cover[y * COVER + x];
        }
    }
}

static void draw_player(int idx) {
    char line[160];
    int x = 420;

    fb_clear(&fb, BG);
    fb_rect(&fb, 0, 0, fb.w, 100, PANEL);
    fb_text(&fb, 40, 16, "NOW PLAYING", 2, GREY, TRANSPARENT);
    snprintf(line, sizeof(line), "%d / %d  %s", idx + 1, nitems, cur_dir[0] ? cur_dir : "/");
    text(40, 60, line, 2, GREY, TRANSPARENT, 74);
    draw_cover();
    text(x, 130, song.title, 3, WHITE, TRANSPARENT, 34);
    text(x, 190, song.artist[0] ? song.artist : "Unknown artist", 2, YELLOW, TRANSPARENT, 50);
    snprintf(line, sizeof(line), "%s%s%s%s", song.album[0] ? song.album : "Unknown album",
              song.year[0] ? "  (" : "", song.year, song.year[0] ? ")" : "");
    text(x, 230, line, 2, CYAN, TRANSPARENT, 50);
    snprintf(line, sizeof(line), "%s  %d Hz  %s  %d kbit/s%s", song.is_mp3 ? "MP3" : "WAV",
              song.rate, song.chans == 2 ? "stereo" : "mono", song.bitrate / 1000,
              song.is_mp3 ? "" : (song.bits == 8 ? "  8-bit" : song.bits == 24 ? "  24-bit" : "  16-bit"));
    text(x, 290, line, 2, WHITE, TRANSPARENT, 50);
    fb_rect(&fb, 0, 640, fb.w, 80, PANEL);
    fb_text(&fb, 40, 650, "OK pause  UP/DOWN prev/next  LEFT/RIGHT volume", 2, GREY, TRANSPARENT);
    fb_text(&fb, 40, 684, "RED/GREEN seek (hold = faster)  BACK list  HOME quit", 2, GREY,
             TRANSPARENT);
}

static void draw_meter(int y, int peak) {
    int w = 0, v = peak, steps = 0, x, full = 760;

    if (v > 0) {
        while (v < 16384 && steps < 12) {
            v <<= 1;
            steps++;
        }
        if (steps < 12) {
            w = full * (11 - steps) / 12 + (full / 12) * (v - 16384) / 16384;
        }
    }
    for (x = 0; x < full; x += 10) {
        u16 c = x >= w ? METER_OFF : x > full * 11 / 12 ? RED : x > full * 9 / 12 ? YELLOW : GREEN;

        fb_rect(&fb, 420 + x, y, 8, 18, c);
    }
}

static void draw_status(void) {
    char line[96];
    u32 t = (sk.active ? sk.target_ms / 1000 : out_frames / AUD_RATE), d = song.duration_s;
    int w = d ? (int) (760ull * (t < d ? t : d) / d) : 0;

    fb_rect(&fb, 420, 360, w, 16, CYAN);
    fb_rect(&fb, 420 + w, 360, 760 - w, 16, METER_OFF);
    snprintf(line, sizeof(line), "%d:%02d / %d:%02d   %s   vol %d%%   ", t / 60, t % 60,
              d / 60, d % 60, sk.active ? "SEEK   " : paused ? "PAUSED " : "playing", vol);
    fb_text(&fb, 420, 390, line, 2, WHITE, BG);
    draw_meter(450, peak_l);
    draw_meter(476, peak_r);
    peak_l = peak_r = 0;
}

/* Front display (satellite box): playing time ("123" = 1:23; from 10 min
 * on it alternates each second between the minutes "12-" and the seconds
 * "-34"), "PAU" when paused, the volume ("U80", "100") for 1.5 s after a
 * change. The front LED blinks with the seconds and stays on while paused. */
static u32 panel_vol_until;

static void panel_update(void) {
    u32 t = sk.active ? sk.target_ms / 1000 : out_frames / AUD_RATE;
    char s[8];

    if (ub_get_timer(0) < panel_vol_until) {
        snprintf(s, sizeof(s), vol < 100 ? "U%2d" : "%3d", vol);
    } else if (paused && !sk.active) {
        strcpy(s, "PAU");
    } else if (t < 600) {
        snprintf(s, sizeof(s), "%u%02u", (unsigned) (t / 60), (unsigned) (t % 60));
    } else if (t & 1) {
        snprintf(s, sizeof(s), "-%02u", (unsigned) (t % 60));
    } else {
        snprintf(s, sizeof(s), "%2u-", (unsigned) (t / 60 % 100));
    }
    sdk_panel_show(s);
}

static void led_update(void) {
    sdk_panel_led(paused || out_frames % AUD_RATE < AUD_RATE / 2);
}

/* Play from items[idx] on. Returns when the user goes back to the list. */
static void play_from(int idx) {
    struct sdk_key k;
    u32 last = 0, panel_last = 0, t0 = ub_get_timer(0);

    while (idx < nitems && (items[idx].is_dir || open_song(items[idx].name) < 0)) {
        idx++;
    }
    if (idx >= nitems) {
        return;
    }
    audio_start();
    draw_player(idx);
    for (;;) {
        u32 now;
        int next = 0;

        if (!pump()) {
            next = 1;                           /* song finished */
        }
        if (sdk_key_poll(&k) && k.btn != BTN_NONE &&
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
                panel_vol_until = ub_get_timer(0) + 1500;
            } else if (k.btn == BTN_RIGHT) {
                vol = vol < 95 ? vol + 5 : 100;
                panel_vol_until = ub_get_timer(0) + 1500;
            } else if (k.btn == BTN_RED || k.btn == BTN_GREEN) {
                seekhold_key(&sk, k.btn == BTN_RED ? -1 : 1, k.repeat,
                              (u32) ((unsigned long long) out_frames * 1000 / AUD_RATE),
                              song.duration_s * 1000, ub_get_timer(0));
            } else if (k.btn == BTN_STOP || k.btn == BTN_BACK) {
                break;
            } else if (k.btn == BTN_HOME || k.btn == BTN_POWER) {
                sdk_panel_led(0);
                close_song();
                audio_stop();
                fb_clear(&fb, TRANSPARENT);
                exit(0);
            }
            last = 0;
        }
        if (next) {
            int i = idx + next;

            while (i >= 0 && i < nitems && (items[i].is_dir || open_song(items[i].name) < 0)) {
                i += next;
            }
            if (i < 0 || i >= nitems) {
                break;                          /* start / end of the folder */
            }
            idx = sel = i;
            sk.active = 0;
            draw_player(idx);
            last = 0;
        }
        if (seekhold_done(&sk, ub_get_timer(0))) {
            seek_to(sk.target_ms / 1000);
        }
        now = ub_get_timer(t0);
        if (now - last >= 100 && !sdk_screen_off) { /* screen saver: nothing to see */
            last = now;
            draw_status();
        }
        if (now - panel_last >= 200) {          /* the front display stays on */
            panel_last = now;
            panel_update();
        }
        led_update();
        sdk_idle(500);                         /* the ring holds 170 ms */
    }
    sk.active = 0;
    close_song();
    audio_stop();
    sdk_panel_show("---");
    sdk_panel_led(0);
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    struct sdk_key k;

    song.h = -1;
    if (osd_setup(&fb) < 0) {
        printf("music: display not running\n");
        return 1;
    }
    snprintf(start_dir, sizeof(start_dir), "%s", argc > 1 ? argv[1] : "/MUSIC");
    while (start_dir[0] == '/') {
        memmove(start_dir, start_dir + 1, strlen(start_dir));
    }
    strcpy(cur_dir, start_dir);
    scan();
    if (nitems == 0 && start_dir[0]) {
        printf("music: /%s empty or missing, using the stick root\n", start_dir);
        start_dir[0] = cur_dir[0] = 0;
        scan();
    }
    draw_browser();

    for (;;) {
        int redraw = 0;

        if (!sdk_key_poll(&k) || k.btn == BTN_NONE) {
            sdk_idle(2000);
            continue;
        }
        if (k.btn == BTN_UP && nitems) {
            sel = (sel + nitems - 1) % nitems;
            redraw = 1;
        } else if (k.btn == BTN_DOWN && nitems) {
            sel = (sel + 1) % nitems;
            redraw = 1;
        } else if ((k.btn == BTN_OK || k.btn == BTN_RIGHT || k.btn == BTN_PLAY) && nitems && !k.repeat) {
            if (items[sel].is_dir) {
                char nd[SDK_PATH_MAX];

                snprintf(nd, sizeof(nd), "%s%s%s", cur_dir, cur_dir[0] ? "/" : "", items[sel].name);
                strcpy(cur_dir, nd);
                scan();
            } else {
                play_from(sel);
            }
            redraw = 1;
        } else if ((k.btn == BTN_BACK || k.btn == BTN_LEFT) && !k.repeat) {
            if (!strcmp(cur_dir, start_dir) || !cur_dir[0]) {
                if (k.btn == BTN_BACK) {
                    break;
                }
            } else {
                char *s = strrchr(cur_dir, '/');

                if (s) {
                    *s = 0;
                } else {
                    cur_dir[0] = 0;
                }
                scan();
                redraw = 1;
            }
        } else if (k.btn == BTN_HOME || k.btn == BTN_POWER) {
            break;
        }
        if (redraw) {
            if (sel < top) {
                top = sel;
            } else if (sel >= top + ROWS) {
                top = sel - ROWS + 1;
            }
            draw_browser();
        }
    }
    fb_clear(&fb, TRANSPARENT);
    return 0;
}
