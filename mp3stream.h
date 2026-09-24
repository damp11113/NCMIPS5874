/*
 * mp3stream: MP3 in RAM -> Helix fixed-point decoder -> audio.h (48 kHz).
 * Shared by mp3play.c and badapple.c (build with buildmp3.sh).
 *
 *   #include "audio.h"                     (AUD_BUF_PHYS set first if needed)
 *   #include "mp3stream.h"
 *
 *   if (mp3s_open (data, len) < 0) ...    decodes the first frame: mp3s_info
 *   audio_start ();
 *   while (mp3s_pump ()) {                 feed what audio_space () allows
 *       ... mp3s_out_frames / AUD_RATE = seconds played so far
 *   }
 *   mp3s_drain (); audio_stop ();
 *
 * Streaming (data not all in RAM): mp3s_open_stream (buf, size, refill)
 * with refill (dst, max) returning bytes added (0 = end of data). The
 * buffer is topped up between MP3 frames; refill time is not counted
 * as decode time.
 *
 * Mono becomes stereo, any rate is resampled to 48 kHz (linear). Bad
 * frames are counted and skipped (resync). Decode time is measured with
 * CP0 Count (mp3s_dec_ms). Volume: mp3s_volq = percent * 256 / 100.
 * Peaks since the caller last cleared them: mp3s_peak_l / mp3s_peak_r.
 * Channel order as wavplay: word = hi << 16 | lo, R = hi, L = lo.
 */
#ifndef MP3STREAM_H
#define MP3STREAM_H

#include "mp3dec.h"

#define MP3S_CHUNK          512
#define MP3S_COUNT_PER_MS   324000      /* CP0 Count rate (cpuinfo) / 1000 */

/* Helix allocates its state once (~30 KB): a bump allocator is enough */
static unsigned char helix_pool[48 * 1024] __attribute__ ((aligned (8)));
static u32 helix_used;

void *helix_malloc (int size) {
    void *p;

    size = (size + 7) & ~7;
    if (helix_used + size > sizeof (helix_pool)) {
        printf ("mp3stream: helix_malloc (%d) out of pool\n", size);
        return 0;
    }
    p = helix_pool + helix_used;
    helix_used += size;
    return p;
}

void helix_free (void *ptr) {
    (void) ptr;
}

static short mp3s_dec_out[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];  /* one frame */
static short mp3s_in[2 * (MAX_NGRAN * MAX_NSAMP + 8)];         /* stereo, decoded */
static short mp3s_pcm[MP3S_CHUNK * 2];                         /* stereo, 48 kHz */

static HMP3Decoder mp3s_dec;
static unsigned char *mp3s_ptr;
static int mp3s_left;
static MP3FrameInfo mp3s_info;
static u32 mp3s_frames_ok, mp3s_errors, mp3s_dec_ms, mp3s_dec_acc;
static u32 mp3s_have, mp3s_pos, mp3s_frac, mp3s_step, mp3s_rate;
static u32 mp3s_out_frames, mp3s_volq = 256;
static int mp3s_peak_l, mp3s_peak_r;
static u32 mp3s_dec_ticks;              /* raw CP0 Count, wraps: use deltas */

/* Streaming input (NULL refill = everything is in RAM already) */
static int (*mp3s_refill) (unsigned char *dst, int max);
static unsigned char *mp3s_buf;
static int mp3s_buf_size, mp3s_eof;

/* Move what is left to the buffer start and top it up */
static void mp3s_fill (void) {
    int i, n;

    if (!mp3s_refill || mp3s_eof) {
        return;
    }
    for (i = 0; i < mp3s_left; i++) {
        mp3s_buf[i] = mp3s_ptr[i];
    }
    mp3s_ptr = mp3s_buf;
    n = mp3s_refill (mp3s_buf + mp3s_left, mp3s_buf_size - mp3s_left);
    if (n <= 0) {
        mp3s_eof = 1;
    } else {
        mp3s_left += n;
    }
}

static inline u32 mp3s_count (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

/*
 * Decode the next good frame into dst as stereo pairs.
 * Returns the number of stereo frames, 0 at end of data.
 */
static int mp3s_decode_frame (short *dst) {
    for (;;) {
        unsigned char *frame_ptr;
        int off, err, n, i, frame_left;
        u32 t0, d;

        /* Keep >= 2 max-size frames buffered: Helix consumes the header
         * before it notices a frame is cut short */
        while (mp3s_refill && !mp3s_eof && mp3s_left < 2 * MAINBUF_SIZE) {
            mp3s_fill ();
        }
        off = MP3FindSyncWord (mp3s_ptr, mp3s_left);
        if (off < 0) {
            if (mp3s_refill && !mp3s_eof) {
                mp3s_ptr += mp3s_left > 3 ? mp3s_left - 3 : 0;  /* keep a split sync */
                mp3s_left = mp3s_left > 3 ? 3 : mp3s_left;
                mp3s_fill ();
                continue;
            }
            return 0;
        }
        mp3s_ptr += off;
        mp3s_left -= off;

        frame_ptr = mp3s_ptr;
        frame_left = mp3s_left;
        t0 = mp3s_count ();
        err = MP3Decode (mp3s_dec, &mp3s_ptr, &mp3s_left, mp3s_dec_out, 0);
        d = mp3s_count () - t0;
        mp3s_dec_ticks += d;
        mp3s_dec_acc += d;
        while (mp3s_dec_acc >= MP3S_COUNT_PER_MS) {
            mp3s_dec_acc -= MP3S_COUNT_PER_MS;
            mp3s_dec_ms++;
        }

        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (mp3s_refill && !mp3s_eof) {
                mp3s_ptr = frame_ptr;   /* frame cut at the buffer end: */
                mp3s_left = frame_left; /* rewind, top up, try again */
                mp3s_fill ();
                continue;
            }
            return 0;                   /* no more data: end */
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
            continue;                   /* bit reservoir filling up */
        }
        if (err != ERR_MP3_NONE) {
            mp3s_errors++;
            if (mp3s_left > 0) {        /* step past the bad sync, resync */
                mp3s_ptr++;
                mp3s_left--;
            }
            continue;
        }

        MP3GetLastFrameInfo (mp3s_dec, &mp3s_info);
        mp3s_frames_ok++;
        if (mp3s_info.nChans == 2) {
            n = mp3s_info.outputSamps / 2;
            for (i = 0; i < 2 * n; i++) {
                dst[i] = mp3s_dec_out[i];
            }
        } else {
            n = mp3s_info.outputSamps;
            for (i = 0; i < n; i++) {
                dst[2 * i] = mp3s_dec_out[i];
                dst[2 * i + 1] = mp3s_dec_out[i];
            }
        }
        if (n > 0) {
            return n;
        }
    }
}

static void mp3s_set_rate (u32 rate) {
    mp3s_rate = rate;
    mp3s_step = (rate << 12) / 3000;    /* rate * 65536 / 48000, 16.16 */
}

/* Returns 0, or -1 if the decoder fails or no frame is found */
static int mp3s_open (unsigned char *data, u32 len) {
    mp3s_ptr = data;
    mp3s_left = len;
    mp3s_dec = MP3InitDecoder ();
    if (!mp3s_dec) {
        printf ("mp3stream: MP3InitDecoder failed\n");
        return -1;
    }
    mp3s_have = mp3s_decode_frame (mp3s_in);
    if (!mp3s_have) {
        return -1;
    }
    mp3s_set_rate (mp3s_info.samprate);
    return 0;
}

/* Streaming: buf (a few KB or more) is refilled through refill () */
__attribute__ ((unused))
static int mp3s_open_stream (unsigned char *buf, int size, int (*refill) (unsigned char *, int)) {
    mp3s_buf = buf;
    mp3s_buf_size = size;
    mp3s_refill = refill;
    mp3s_eof = 0;
    mp3s_left = 0;
    mp3s_ptr = buf;
    mp3s_fill ();
    return mp3s_open (buf, mp3s_left);
}

/* Streaming: drop all buffered input and decoded samples, e.g. after the
 * caller moved its file position (seek). The decoder resyncs on its own
 * (the first frames may report a bit-reservoir underflow and are skipped). */
__attribute__ ((unused))
static void mp3s_restart (void) {
    mp3s_ptr = mp3s_buf;
    mp3s_left = 0;
    mp3s_eof = 0;
    mp3s_have = 0;
    mp3s_pos = 0;
    mp3s_frac = 0;
    mp3s_fill ();
}

/* Feed the audio ring as far as it has room (max MP3S_CHUNK frames).
 * Returns 0 at end of data, else 1. */
static int mp3s_pump (void) {
    u32 n = audio_space ();
    u32 k = 0;
    int i, more = 1;

    if (n > MP3S_CHUNK) {
        n = MP3S_CHUNK;
    }
    while (k < n) {
        const short *a, *b;
        int l, r;

        if (mp3s_pos + 1 >= mp3s_have) {
            /* Keep the last decoded frame (interpolation needs it),
             * append the next decoded MP3 frame after it */
            u32 keep = mp3s_have - mp3s_pos, got;

            for (i = 0; i < (int) (2 * keep); i++) {
                mp3s_in[i] = mp3s_in[2 * mp3s_pos + i];
            }
            mp3s_have = keep;
            mp3s_pos = 0;
            got = mp3s_decode_frame (mp3s_in + 2 * mp3s_have);
            if (!got) {
                more = 0;
                break;
            }
            mp3s_have += got;
            if ((u32) mp3s_info.samprate != mp3s_rate) {
                mp3s_set_rate (mp3s_info.samprate);
            }
            continue;
        }

        a = mp3s_in + 2 * mp3s_pos;
        b = a + 2;
        l = a[0] + (((b[0] - a[0]) * (int) mp3s_frac) >> 16);
        r = a[1] + (((b[1] - a[1]) * (int) mp3s_frac) >> 16);
        l = (l * (int) mp3s_volq) >> 8;
        r = (r * (int) mp3s_volq) >> 8;

        mp3s_pcm[2 * k] = r;
        mp3s_pcm[2 * k + 1] = l;
        k++;

        if (l < 0) {
            l = -l;
        }
        if (r < 0) {
            r = -r;
        }
        if (l > mp3s_peak_l) {
            mp3s_peak_l = l;
        }
        if (r > mp3s_peak_r) {
            mp3s_peak_r = r;
        }

        mp3s_frac += mp3s_step;
        mp3s_pos += mp3s_frac >> 16;
        mp3s_frac &= 0xffff;
    }
    if (k) {
        audio_write (mp3s_pcm, k);
        mp3s_out_frames += k;
    }
    return more;
}

/* Let what is still in the audio ring play out (max ~0.5 s) */
__attribute__ ((unused))
static void mp3s_drain (void) {
    u32 start = get_timer (0);

    while ((AUD_REG (0x104) & AUD_MASK) > 0x40 && get_timer (start) < 500) {
        udelay (1000);
    }
}

/* One-line summary: frames, bad frames, decode time and CPU share */
__attribute__ ((unused))
static void mp3s_report (const char *who) {
    u32 audio_ms = mp3s_out_frames / (AUD_RATE / 1000);

    printf ("%s: %d MP3 frames, %d bad, decode %d ms for %d ms audio", who,
            mp3s_frames_ok, mp3s_errors, mp3s_dec_ms, audio_ms);
    if (audio_ms) {
        printf (" (%d.%d %% CPU)", mp3s_dec_ms * 100 / audio_ms,
                (mp3s_dec_ms * 1000 / audio_ms) % 10);
    }
    printf ("\n");
}

#endif
