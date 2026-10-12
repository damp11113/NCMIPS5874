/*
 * tjcplay: plays TJC video files (tjc/, "Tiled-JPEG Codec", QOA audio)
 * streamed from the USB stick.
 *
 *   tjcplay                  file list: *.TJC in the app folder and /VIDEO
 *   tjcplay /VIDEO/A.TJC     play that file (more words: options)
 *   options: zoom=0 (1:1, centred), bt601 (colour matrix, default BT.709)
 *
 * Make a file on the PC (tjc/README.md), audio at 48 kHz = no resampling:
 *   ffmpeg -i in.mp4 -vn -ar 48000 -ac 2 -c:a pcm_s16le a.wav
 *   ffmpeg -i in.mp4 -f rawvideo -pix_fmt yuv420p -s 854x480 - |
 *       tjc encode --size 854x480 --fps 24 --audio a.wav -o OUT.TJC
 *
 * Pipeline (one loop, no threads):
 *   read ahead: frame records into a queue of slots (video bytes kept
 *     compressed), each frame's QOA audio decoded at once into a PCM FIFO
 *     (stereo, resampled to 48 kHz if needed)
 *   audio: the FIFO tops up the hardware ring; samples played = the clock
 *     (CP0 Count when the file has no audio)
 *   video: when its time comes a frame's dirty tiles are decoded into the
 *     YUV planes and marked; marked tiles are converted to ARGB1555 (BT.709
 *     or BT.601, 4x4 ordered dither) straight into the OSD. When the next
 *     frame is already due, all due frames go in one step and only the
 *     newest copy of each tile is decoded (tiles are self-contained), so
 *     the video drops frames instead of holding up the audio.
 *   zoom: the OSD region header gets the video size and the OSD scaler
 *     stretches it to the screen (no CPU cost).
 *
 * Remote: OK / PLAY / PAUSE pause, LEFT / RIGHT volume, BLUE zoom on / off,
 * YELLOW BT.709 / BT.601, INFO statistics, BACK / STOP / HOME / POWER stop.
 */
#define BOX_WANT_AUDIO
#include "sdk.h"
#include "tjcdec.h"

typedef unsigned short u16;
typedef unsigned char u8;

#define QN          32                  /* frame slots read ahead */
#define FIFO_N      262144u             /* PCM FIFO, stereo frames (5.4 s) */
#define FIFO_KEEP   20000u              /* kept behind the read point (pause rewind) */
#define RBUF_SIZE   (256 * 1024)
#define RING_TARGET 12000u              /* frames kept in the hardware ring (250 ms) */
#define MAX_FILES   64

static struct fb fb;

static inline u32 c0_count(void) {
    u32 v;

    __asm__ volatile("mfc0 %0, $9" : "=r" (v));
    return v;
}

/* ---- buffered reader over sdk_read ---- */

static int fh = -1;
static u8 *rbuf;
static u32 rpos, rlen;
static int reof;
static u32 t_read;                      /* Count ticks spent in USB reads */

static u32 rd(void *dst, u32 n) {
    u8 *d = dst;
    u32 got = 0;

    while (got < n) {
        u32 k;

        if (rpos == rlen) {
            long r;
            u32 t0 = c0_count();

            if (reof) {
                break;
            }
            r = sdk_read(fh, rbuf, RBUF_SIZE);
            t_read += c0_count() - t0;
            if (r <= 0) {
                reof = 1;
                break;
            }
            rpos = 0;
            rlen = (u32) r;
        }
        k = rlen - rpos < n - got ? rlen - rpos : n - got;
        memcpy(d + got, rbuf + rpos, k);
        rpos += k;
        got += k;
    }
    return got;
}

/* ---- stream state ---- */

static struct tjc_dec dec;
static int bitmap_len;

struct slot {
    u8 *buf;
    u32 cap, len;
    u32 frame;                          /* index in the file (0-based) */
    int force;                          /* 1 = every tile follows, no bitmap */
};

static struct slot slots[QN];
static int q_head, q_count;
static u32 frames_read;
static int read_done;                   /* end of file or error */
static const char *read_error;

static u8 *abuf;                        /* one frame's QOA bytes */
static u32 abuf_cap;
static int16_t *qoa_pcm;                /* one decoded QOA frame */

/* PCM FIFO: u32 = R << 16 | L, 48 kHz */
static u32 *fifo;
static u32 fifo_w, fifo_r;              /* running counters */

/* resampler state (source rate != 48 kHz) */
static u32 rs_step, rs_pos;
static int16_t rs_prev_l, rs_prev_r;

static int ensure(u8 **buf, u32 *cap, u32 need) {
    u8 *p;
    u32 n;

    if (need <= *cap) {
        return 0;
    }
    n = *cap ? *cap : 65536;
    while (n < need) {
        n *= 2;
    }
    p = realloc(*buf, n);
    if (!p) {
        return -1;
    }
    *buf = p;
    *cap = n;
    return 0;
}

static inline void fifo_put(int l, int r) {
    fifo[fifo_w & (FIFO_N - 1)] = ((u32) (u16) r << 16) | (u16) l;
    fifo_w++;
}

/* One decoded QOA frame (n samples per channel, interleaved) into the FIFO */
static void audio_push(const int16_t *s, int n) {
    int ch = dec.h.audio_channels, i;

    if (dec.h.audio_rate == 48000) {
        for (i = 0; i < n; i++, s += ch) {
            fifo_put(s[0], ch > 1 ? s[1] : s[0]);
        }
        return;
    }
    /* linear interpolation, x[-1] = previous chunk's last sample */
    while ((int) (rs_pos >> 16) < n) {
        int i1 = (int) (rs_pos >> 16), f = (int) (rs_pos & 0xffff);
        const int16_t *b = s + i1 * ch;
        int al = i1 ? b[-ch] : rs_prev_l;
        int ar = i1 ? (ch > 1 ? b[-ch + 1] : b[-ch]) : rs_prev_r;
        int bl = b[0], br = ch > 1 ? b[1] : b[0];

        fifo_put(al + (((bl - al) * f) >> 16), ar + (((br - ar) * f) >> 16));
        rs_pos += rs_step;
    }
    rs_pos -= (u32) n << 16;
    rs_prev_l = s[(n - 1) * ch];
    rs_prev_r = ch > 1 ? s[(n - 1) * ch + 1] : rs_prev_l;
}

/* Reads one frame record into the next slot. 0 = ok, 1 = end, -1 = error. */
static int read_frame(void) {
    struct slot *sl = &slots[(q_head + q_count) % QN];
    u8 fhdr[TJC_FRAME_HDR], lb[4];
    u32 got, i, dirty = 0;

    got = rd(fhdr, TJC_FRAME_HDR);
    if (got == 0) {
        return 1;
    }
    if (got < TJC_FRAME_HDR || fhdr[4] > 1) {
        read_error = "truncated or corrupt frame header";
        return -1;
    }
    sl->force = fhdr[4];
    sl->frame = frames_read;
    sl->len = 0;
    if (sl->force) {
        dirty = (u32) dec.tiles;
    } else {
        if (ensure(&sl->buf, &sl->cap, (u32) bitmap_len) < 0 ||
                rd(sl->buf, (u32) bitmap_len) < (u32) bitmap_len) {
            read_error = "truncated bitmap / out of memory";
            return -1;
        }
        sl->len = (u32) bitmap_len;
        for (i = 0; i < (u32) bitmap_len; i++) {
            dirty += (u32) __builtin_popcount(sl->buf[i]);
        }
    }
    for (i = 0; i < dirty; i++) {
        u32 len;

        if (rd(lb, 2) < 2) {
            read_error = "truncated tile";
            return -1;
        }
        len = tjc_u16(lb);
        if (len == 0 || ensure(&sl->buf, &sl->cap, sl->len + 2 + len) < 0) {
            read_error = "corrupt tile length / out of memory";
            return -1;
        }
        sl->buf[sl->len] = lb[0];
        sl->buf[sl->len + 1] = lb[1];
        if (rd(sl->buf + sl->len + 2, len) < len) {
            read_error = "truncated tile";
            return -1;
        }
        sl->len += 2 + len;
    }
    if (dec.h.audio_codec) {
        u32 alen, pos = 0;

        if (rd(lb, 4) < 4) {
            read_error = "truncated audio";
            return -1;
        }
        alen = tjc_u32(lb);
        if (alen > 4 * 1024 * 1024 || ensure(&abuf, &abuf_cap, alen + 1) < 0 || rd(abuf, alen) < alen) {
            read_error = "truncated / too big audio";
            return -1;
        }
        while (pos < alen) {
            u32 used;
            int n = tjc_qoa_decode(abuf + pos, alen - pos, dec.h.audio_channels, dec.h.audio_rate, qoa_pcm,
                                   &used);

            if (n < 0) {
                read_error = "corrupt QOA audio";
                return -1;
            }
            audio_push(qoa_pcm, n);
            pos += used;
        }
    }
    frames_read++;
    q_count++;
    return 0;
}

/* ---- display ---- */

static int zoom = 1, matrix709 = 1;
static volatile u16 *dst_base;          /* pixel (0, 0) of the video */
static int dst_pitch;
static u8 *pending;                     /* tiles decoded but not converted */
static int pending_count;

/* YUV -> RGB in 8.8 fixed point, then 5-bit with dither through clamp5 */
static int ty[256], trv[256], tgu[256], tgv[256], tbu[256];
static u8 clamp5[1280];
#define CL_OFF 512

static const u8 bayer[4][4] = {
    {0, 4, 1, 5}, {6, 2, 7, 3}, {1, 5, 0, 4}, {7, 3, 6, 2},
};

static void build_tables(void) {
    /* coefficients x 65536: BT.709 / BT.601, limited range */
    int kr = matrix709 ? 117489 : 104597, kgu = matrix709 ? 13954 : 25675;
    int kgv = matrix709 ? 34928 : 53279, kbu = matrix709 ? 138438 : 132201;
    int i;

    for (i = 0; i < 256; i++) {
        ty[i] = (76309 * (i - 16)) >> 8;            /* 1.164 x 256 */
        trv[i] = (kr * (i - 128)) >> 8;
        tgu[i] = (kgu * (i - 128)) >> 8;
        tgv[i] = (kgv * (i - 128)) >> 8;
        tbu[i] = (kbu * (i - 128)) >> 8;
    }
    for (i = 0; i < 1280; i++) {
        int v = i - CL_OFF;

        clamp5[i] = (u8) ((v < 0 ? 0 : (v > 255 ? 255 : v)) >> 3);
    }
}

static inline u32 pix(int y, int u, int v, int d) {
    int l = ty[y] + 128 + (d << 8);
    u32 r = clamp5[((l + trv[v]) >> 8) + CL_OFF];
    u32 g = clamp5[((l - tgu[u] - tgv[v]) >> 8) + CL_OFF];
    u32 b = clamp5[((l + tbu[u]) >> 8) + CL_OFF];
    u32 c = 0x8000 | (r << 10) | (g << 5) | b;

    return c == 0x801f ? 0x801e : c;            /* 0x801f = the layer's colour key */
}

static void convert_tile(int t) {
    int x0, y0, w, h, y;
    const u8 *py = dec.plane[0], *pu = dec.plane[1], *pv = dec.plane[2];
    int ys = dec.stride[0], cs = dec.stride[1];

    tjc_tile_rect(&dec, t, 0, &x0, &y0, &w, &h);
    if (x0 + w > dec.h.width) {
        w = dec.h.width - x0;
    }
    if (y0 + h > dec.h.height) {
        h = dec.h.height - y0;
    }
    for (y = y0; y < y0 + h; y++) {
        const u8 *ry = py + y * ys, *ru = pu + (y >> 1) * cs, *rv = pv + (y >> 1) * cs;
        const u8 *dr = bayer[y & 3];
        volatile u16 *o = dst_base + y * dst_pitch;
        int x = x0, end = x0 + w;

        if (x & 1) {
            o[x] = (u16) pix(ry[x], ru[x >> 1], rv[x >> 1], dr[x & 3]);
            x++;
        }
        /* pairs share one chroma sample: one 32-bit store (dst_base and
         * dst_pitch are even, so the address is aligned) */
        for (; x + 1 < end; x += 2) {
            int u = ru[x >> 1], v = rv[x >> 1];

            *(volatile u32 *) (o + x) = pix(ry[x], u, v, dr[x & 3]) | (pix(ry[x + 1], u, v, dr[(x + 1) & 3]) << 16);
        }
        if (x < end) {
            o[x] = (u16) pix(ry[x], ru[x >> 1], rv[x >> 1], dr[x & 3]);
        }
    }
}

static void convert_pending(void) {
    int t;

    for (t = 0; t < dec.tiles && pending_count; t++) {
        if (pending[t]) {
            convert_tile(t);
            pending[t] = 0;
            pending_count--;
        }
    }
}

static u32 *taken;                      /* tile -> gen of the step that decoded it */
static u32 gen;

/* Decodes the tiles of one frame that a newer due frame did not already
 * decode in this step (frames go newest first). 0 = ok, -1 = corrupt. */
static int decode_slot(const struct slot *sl) {
    u32 p = sl->force ? 0 : (u32) bitmap_len;
    int t;

    for (t = 0; t < dec.tiles; t++) {
        u32 len;

        if (!sl->force && !((sl->buf[t >> 3] >> (t & 7)) & 1)) {
            continue;
        }
        len = tjc_u16(sl->buf + p);
        if (p + 2 + len > sl->len) {
            return -1;
        }
        if (taken[t] != gen) {
            if (tjc_decode_tile(&dec, sl->buf + p + 2, len, t) < 0) {
                return -1;
            }
            taken[t] = gen;
            if (!pending[t]) {
                pending[t] = 1;
                pending_count++;
            }
        }
        p += 2 + len;
    }
    return 0;
}

static void mark_all(void) {
    memset(pending, 1, dec.tiles);
    pending_count = dec.tiles;
}

/* OSD region size + scaler for a w x h source (osd_setup does 1280x720) */
static void osd_source(int w, int h, int pitch) {
    volatile u32 *hdr = (volatile u32 *) (0xa0000000u | OSD_HDR_PHYS);
    u32 out = REG32(0xbf4400b8), dst_w = out & 0xffff, dst_h = out >> 16;

    hdr[2] = ((u32) h << 16) | (u32) w;
    hdr[3] = ((u32) pitch << 16) | 0xff;
    REG32(0xbf440110) = ((u32) w << 16) | dst_w;
    REG32(0xbf440128) = ((u32) h << 16) | dst_h;
    if (dst_h >= 720) {
        REG32(0xbf440114) = ((u32) w << 12) / dst_w;
        REG32(0xbf44012c) = ((u32) h << 12) / dst_h;
    } else {
        REG32(0xbf440114) = ((u32) w << 16) / dst_w;
        REG32(0xbf44012c) = ((u32) h << 16) / dst_h;
    }
}

static int overlay_saved;

static void display_setup(void) {
    int vw = dec.h.width, vh = dec.h.height;

    osd_setup(&fb);
    fb_clear(&fb, BLACK);
    if (zoom && vw <= fb.w && vh <= fb.h) {
        int pitch = (vw + 31) & ~31;         /* 854 (1708 bytes) sheared the picture */

        sdk_overlay_on = 0;             /* its bar assumes the 1280x720 layout */
        osd_source(vw, vh, pitch);
        dst_base = fb.pix;
        dst_pitch = pitch;
    } else {
        int ox = vw < fb.w ? ((fb.w - vw) / 2) & ~1 : 0, oy = vh < fb.h ? (fb.h - vh) / 2 : 0;

        sdk_overlay_on = overlay_saved;
        dst_base = fb.pix + oy * fb.pitch + ox;
        dst_pitch = fb.pitch;
    }
    mark_all();
}

/* ---- audio output + clock ---- */

static int volume = 100;                /* percent */
static u32 hw_written;                  /* frames into the ring since audio_start */
static u32 clock_base;                  /* media position (48 kHz) at audio_start */
static int paused;
static u32 cnt_per_sample;              /* CP0 Count ticks per 48 kHz sample */
static u32 cnt_last, cnt_frac;          /* no-audio clock */
static u32 noaudio_clock;
static u32 t_audio;
static short mix[2 * 1024];

static inline u32 ring_queued(void) {
    return ((AUD_REG(0x104) & AUD_MASK) << 3) / AUD_FRAME;
}

static u32 media_clock(void) {
    if (dec.h.audio_codec) {
        u32 q = ring_queued();

        return clock_base + (hw_written > q ? hw_written - q : 0);
    }
    if (!paused) {
        u32 now = c0_count();

        cnt_frac += now - cnt_last;
        cnt_last = now;
        noaudio_clock += cnt_frac / cnt_per_sample;
        cnt_frac %= cnt_per_sample;
    }
    return noaudio_clock;
}

/* Fill the hardware ring from the FIFO (zeros while paused) */
static void pump_audio(void) {
    u32 q, want, n, i, t0 = c0_count();
    int vol = volume * 256 / 100;

    if (!dec.h.audio_codec) {
        return;
    }
    q = ring_queued();
    if (q >= RING_TARGET) {
        return;
    }
    want = RING_TARGET - q;
    while (want) {
        n = want > 1024 ? 1024 : want;
        if (!paused) {
            u32 avail = fifo_w - fifo_r;

            if (n > avail) {
                n = avail;
            }
            if (!n) {
                break;
            }
            for (i = 0; i < n; i++) {
                u32 s = fifo[(fifo_r + i) & (FIFO_N - 1)];

                mix[2 * i] = (short) (((int) (short) (s >> 16) * vol) >> 8);      /* R = hi */
                mix[2 * i + 1] = (short) (((int) (short) s * vol) >> 8);          /* L = lo */
            }
            fifo_r += n;
            hw_written += n;
        } else {
            memset(mix, 0, n * 4);
        }
        audio_write(mix, n);
        want -= n;
    }
    t_audio += c0_count() - t0;
}

static void set_pause(int on) {
    if (on == paused) {
        return;
    }
    if (dec.h.audio_codec) {
        u32 q = ring_queued(), pos = media_clock();

        /* media still in the ring goes back into the FIFO (kept there) */
        if (on) {
            u32 back = hw_written > q ? q : hw_written;

            fifo_r -= back;
            clock_base = pos;
        }
        paused = on;
        audio_start();                  /* clears the ring */
        hw_written = 0;
    } else {
        media_clock();
        paused = on;
        cnt_last = c0_count();
    }
}

/* 48 kHz sample at which frame k starts */
static u32 frame_due(u32 k) {
    return (u32) ((unsigned long long) k * 48000u * dec.h.fps_den / dec.h.fps_num);
}

/* ---- statistics ---- */

static int stats_on;
static u32 st_shown, st_decoded, st_merged, st_late_max;
static u32 t_video, t_conv, t_total0;
static u32 sw_t0, sw_video, sw_conv, sw_audio, sw_read;     /* window start values */
static u32 sw_frames;
static char st_line[160];

static void stats_update(void) {
    u32 now = c0_count(), total = now - sw_t0;

    if (total < cnt_per_sample * 24000u) {          /* every 0.5 s */
        return;
    }
    snprintf(st_line, sizeof(st_line),
             "%ux%u %u fps | frame %u/%u | CPU video %u%% conv %u%% audio %u%% usb %u%% | dropped %u, max late %u ms | vol %d%%%s",
             dec.h.width, dec.h.height, (st_shown - sw_frames) * 2, st_decoded, frames_read,
             (u32) ((unsigned long long) (t_video - sw_video) * 100 / total),
             (u32) ((unsigned long long) (t_conv - sw_conv) * 100 / total),
             (u32) ((unsigned long long) (t_audio - sw_audio) * 100 / total),
             (u32) ((unsigned long long) (t_read - sw_read) * 100 / total),
             st_merged, st_late_max / 48, volume, matrix709 ? "" : " BT.601");
    sw_t0 = now;
    sw_video = t_video;
    sw_conv = t_conv;
    sw_audio = t_audio;
    sw_read = t_read;
    sw_frames = st_shown;
    if (stats_on && !zoom) {
        fb_rect(&fb, 0, 0, fb.w, 18, BLACK);
        fb_text(&fb, 4, 1, st_line, 1, WHITE, BLACK);
    }
    if (stats_on) {
        printf("%s\n", st_line);
    }
}

/* ---- playing one file ---- */

static void message(const char *a, const char *b) {
    osd_setup(&fb);
    fb_clear(&fb, BLACK);
    fb_text(&fb, 80, 300, a, 2, WHITE, BLACK);
    if (b) {
        fb_text(&fb, 80, 350, b, 1, GREY, BLACK);
    }
    printf("tjcplay: %s %s\n", a, b ? b : "");
}

static void wait_key(void) {
    struct sdk_key k;

    for (;;) {
        if (sdk_key_poll(&k) && k.btn != BTN_NONE) {
            return;
        }
        sdk_idle(5000);
    }
}

static void free_stream(void) {
    int i;

    for (i = 0; i < QN; i++) {
        free(slots[i].buf);
        slots[i].buf = 0;
        slots[i].cap = 0;
    }
    free(abuf);
    abuf = 0;
    abuf_cap = 0;
    free(qoa_pcm);
    qoa_pcm = 0;
    free(fifo);
    fifo = 0;
    free(pending);
    free(taken);
    taken = 0;
    pending = 0;
    tjc_dec_free(&dec);
    if (fh >= 0) {
        sdk_close(fh);
        fh = -1;
    }
}

/* Returns 1 when the user asked to stop, 0 at the end of the file */
static int play(const char *path) {
    u8 hb[TJC_HDR_SIZE];
    struct tjc_header h;
    u32 i;
    int hs, quit = 0;
    struct sdk_key k;

    printf("tjcplay: %s\n", path);
    fh = sdk_open(path);
    if (fh < 0) {
        message("Cannot open the file", path);
        wait_key();
        return 0;
    }
    rpos = rlen = 0;
    reof = 0;
    hs = rd(hb, TJC_HDR_SIZE_V1) == TJC_HDR_SIZE_V1 ? tjc_parse_header(hb, TJC_HDR_SIZE_V1, &h) : -1;
    if (hs == 0 && rd(hb + TJC_HDR_SIZE_V1, TJC_HDR_SIZE - TJC_HDR_SIZE_V1) == TJC_HDR_SIZE - TJC_HDR_SIZE_V1) {
        hs = tjc_parse_header(hb, TJC_HDR_SIZE, &h);
    }
    if (hs <= 0) {
        message("Not a TJC file", path);
        sdk_close(fh);
        fh = -1;
        wait_key();
        return 0;
    }
    printf("tjcplay: TJC%d %dx%d, tiles %dx%d, q%d, %u/%u fps, audio %s %d ch %u Hz, %u bytes\n",
           h.version, h.width, h.height, h.tile_w, h.tile_h, h.quality, h.fps_num, h.fps_den,
           h.audio_codec ? "QOA" : "none", h.audio_channels, h.audio_rate, sdk_size(fh));

    if (tjc_dec_init(&dec, &h) < 0) {
        message("Out of memory", 0);
        sdk_close(fh);
        fh = -1;
        wait_key();
        return 0;
    }
    bitmap_len = (dec.tiles + 7) / 8;
    pending = malloc(dec.tiles);
    taken = calloc(dec.tiles, sizeof(*taken));
    gen = 0;
    fifo = malloc(FIFO_N * 4);
    qoa_pcm = malloc(TJC_QOA_MAX_FRAME * TJC_QOA_MAX_CH * 2);
    if (!pending || !taken || !fifo || !qoa_pcm) {
        message("Out of memory", 0);
        free_stream();
        wait_key();
        return 0;
    }
    memset(pending, 0, dec.tiles);
    pending_count = 0;
    q_head = q_count = 0;
    frames_read = 0;
    read_done = 0;
    read_error = 0;
    fifo_w = fifo_r = 0;
    rs_pos = 1u << 16;
    rs_step = h.audio_codec ? (u32) (((unsigned long long) h.audio_rate << 16) / 48000u) : 0;
    rs_prev_l = rs_prev_r = 0;
    st_shown = st_decoded = st_merged = st_late_max = 0;
    t_video = t_conv = t_audio = t_read = 0;
    paused = 0;

    display_setup();

    /* pre-read: half the queue, before the clock starts */
    while (!read_done && q_count < QN / 2) {
        int r = read_frame();

        if (r) {
            read_done = 1;
        }
    }
    if (read_error) {
        message("Read error", read_error);
        free_stream();
        wait_key();
        return 0;
    }

    clock_base = 0;
    hw_written = 0;
    noaudio_clock = 0;
    cnt_last = c0_count();
    cnt_frac = 0;
    if (h.audio_codec) {
        audio_start();
    }
    t_total0 = sw_t0 = c0_count();
    sw_video = sw_conv = sw_audio = sw_read = sw_frames = 0;

    while (!quit) {
        u32 now;

        pump_audio();

        /* read ahead while there is room for the frame and its audio */
        for (i = 0; i < 4 && !read_done && q_count < QN; i++) {
            u32 per = frame_due(frames_read + 1) - frame_due(frames_read);

            if (fifo_w - fifo_r + 3 * per + 1024 > FIFO_N - FIFO_KEEP) {
                break;
            }
            if (read_frame()) {
                read_done = 1;
            }
        }

        /* video: every frame that is due now, in one step. Tiles are
         * self-contained, so only the newest copy of each tile is decoded:
         * catching up costs at most one frame's worth of tiles and the
         * audio (decoded at read time) never waits for the video. */
        now = media_clock();
        if (!paused && q_count && now >= frame_due(slots[q_head].frame)) {
            u32 t0 = c0_count(), late = now - frame_due(slots[q_head].frame);
            int n = 1, j;

            while (n < q_count && now >= frame_due(slots[(q_head + n) % QN].frame)) {
                n++;
            }
            if (++gen == 0) {
                memset(taken, 0, dec.tiles * sizeof(*taken));
                gen = 1;
            }
            for (j = n - 1; j >= 0 && !quit; j--) {
                if (decode_slot(&slots[(q_head + j) % QN]) < 0) {
                    read_error = "corrupt tile data";
                    quit = 2;
                }
            }
            q_head = (q_head + n) % QN;
            q_count -= n;
            st_decoded += n;
            st_merged += n - 1;
            t_video += c0_count() - t0;
            if (late > st_late_max) {
                st_late_max = late;
            }

            t0 = c0_count();
            convert_pending();
            t_conv += c0_count() - t0;
            st_shown++;
            if ((st_shown & 31) == 0) {
                sdk_saver_kick();
            }
        } else if (read_done && !q_count) {
            /* end: let the last audio play out */
            if (!h.audio_codec || (fifo_w == fifo_r && ring_queued() < 256)) {
                break;
            }
            sdk_idle(2000);
        } else {
            sdk_idle(300);
        }

        stats_update();

        if (!sdk_key_poll(&k) || k.btn == BTN_NONE) {
            continue;
        }
        if (k.repeat) {
            /* held: only the volume keeps stepping */
            if (k.btn == BTN_LEFT) {
                volume = volume > 10 ? volume - 10 : 0;
            } else if (k.btn == BTN_RIGHT) {
                volume = volume < 150 ? volume + 10 : 150;
            }
        } else {
            switch (k.btn) {
            case BTN_BACK: case BTN_STOP: case BTN_HOME: case BTN_POWER:
                quit = 1;
                break;
            case BTN_OK: case BTN_PLAY: case BTN_PAUSE:
                set_pause(!paused);
                break;
            case BTN_LEFT:
                volume = volume > 10 ? volume - 10 : 0;
                break;
            case BTN_RIGHT:
                volume = volume < 150 ? volume + 10 : 150;
                break;
            case BTN_BLUE:
                zoom = !zoom;
                display_setup();
                convert_pending();
                break;
            case BTN_YELLOW:
                matrix709 = !matrix709;
                build_tables();
                mark_all();
                convert_pending();
                break;
            case BTN_INFO:
                stats_on = !stats_on;
                if (!stats_on && !zoom) {
                    display_setup();
                    convert_pending();
                }
                break;
            default:
                break;
            }
        }
    }

    if (h.audio_codec) {
        audio_stop();
    }
    {
        u32 total = c0_count() - t_total0;

        if (!total) {
            total = 1;
        }
        printf("tjcplay: %u frames read, %u decoded, %u shown, %u frames dropped (tiles merged), max late %u ms\n",
               frames_read, st_decoded, st_shown, st_merged, st_late_max / 48);
        printf("tjcplay: CPU video %u%%, conversion %u%%, audio out %u%%, usb %u%%\n",
               (u32) ((unsigned long long) t_video * 100 / total), (u32) ((unsigned long long) t_conv * 100 / total),
               (u32) ((unsigned long long) t_audio * 100 / total), (u32) ((unsigned long long) t_read * 100 / total));
    }
    if (read_error) {
        message("Playback stopped", read_error);
        free_stream();
        wait_key();
        return 0;
    }
    free_stream();
    return quit == 1;
}

/* ---- file list ---- */

static char files[MAX_FILES][SDK_PATH_MAX];
static int nfiles;

static int is_tjc(const char *n) {
    int l = strlen(n);

    return l > 4 && !strcasecmp(n + l - 4, ".tjc");
}

static void scan(const char *dir) {
    struct sdk_dirent e;
    int h = sdk_dir_open(dir);

    if (h < 0) {
        return;
    }
    while (nfiles < MAX_FILES && sdk_dir_read(h, &e)) {
        if (!e.is_dir && is_tjc(e.name)) {
            snprintf(files[nfiles++], SDK_PATH_MAX, "%s/%s", dir, e.name);
        }
    }
    sdk_dir_close(h);
}

static void draw_list(int sel) {
    int i, first = sel > 15 ? sel - 15 : 0;

    fb_clear(&fb, BLACK);
    fb_text(&fb, 80, 50, "TJC video player", 3, WHITE, BLACK);
    fb_text(&fb, 80, 110, "OK play, BACK exit. In the video: OK pause, LEFT/RIGHT volume,", 1, GREY, BLACK);
    fb_text(&fb, 80, 128, "BLUE zoom, YELLOW colour matrix, INFO statistics, BACK stop.", 1, GREY, BLACK);
    if (!nfiles) {
        fb_text(&fb, 80, 180, "No .TJC files in the app folder or in /VIDEO", 2, YELLOW, BLACK);
        return;
    }
    for (i = first; i < nfiles && i < first + 16; i++) {
        int y = 170 + (i - first) * 30;

        if (i == sel) {
            fb_rect(&fb, 70, y - 4, 1140, 28, RGB(40, 70, 140));
        }
        fb_text(&fb, 80, y, files[i], 1, WHITE, i == sel ? RGB(40, 70, 140) : BLACK);
    }
}

int main(int argc, char *argv[]) {
    struct sdk_key k;
    char appdir[SDK_PATH_MAX];
    const char *file = 0;
    int i, sel = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "zoom=0")) {
            zoom = 0;
        } else if (!strcasecmp(argv[i], "bt601")) {
            matrix709 = 0;
        } else if (is_tjc(argv[i])) {
            file = argv[i];
        }
    }
    overlay_saved = sdk_overlay_on;
    build_tables();
    {
        /* CP0 Count rate (no-audio clock) against U-Boot's ms timer */
        u32 t = get_timer(0), c;

        while (get_timer(0) == t) {
        }
        c = c0_count();
        t = get_timer(0);
        while (get_timer(0) - t < 100) {
        }
        cnt_per_sample = (c0_count() - c) / 4800;
        if (!cnt_per_sample) {
            cnt_per_sample = 6750;
        }
    }
    rbuf = malloc(RBUF_SIZE);
    if (!rbuf || osd_setup(&fb) < 0) {
        printf("tjcplay: no memory or no display\n");
        return 1;
    }

    if (file) {
        play(file);
        osd_setup(&fb);
        sdk_overlay_on = overlay_saved;
        return 0;
    }

    snprintf(appdir, sizeof(appdir), "/%s", sdk_app_dir);
    scan(appdir);
    scan("/VIDEO");
    draw_list(sel);
    for (;;) {
        if (!sdk_key_poll(&k) || k.btn == BTN_NONE) {
            sdk_idle(5000);
            continue;
        }
        if (k.btn == BTN_BACK || k.btn == BTN_HOME || k.btn == BTN_POWER) {
            break;
        }
        if (k.btn == BTN_UP && sel > 0) {
            sel--;
        } else if (k.btn == BTN_DOWN && sel + 1 < nfiles) {
            sel++;
        } else if (k.btn == BTN_OK && nfiles && !k.repeat) {
            play(files[sel]);
            osd_setup(&fb);
            sdk_overlay_on = overlay_saved;
        }
        draw_list(sel);
    }
    osd_setup(&fb);
    sdk_overlay_on = overlay_saved;
    return 0;
}
