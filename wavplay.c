/*
 * wavplay: play a .wav file from RAM through audio.h (HDMI + RCA).
 *
 *   fatload usb 0 ${a} wavplay.bin
 *   fatload usb 0 81600000 testwav.wav        (last, so ${filesize} is its size)
 *   go ${a} 81600000 [${filesize}] [vol=NN] [<reg>=<val> ...]
 *
 * Arguments: load address (hex, default 81600000); file size (hex,
 * optional, else taken from the WAV header); vol=NN software volume in
 * percent (default 100; RCA is clean at full scale); <reg>=<val> (both hex)
 * writes audio register 0xbf490000 + reg after audio_start, e.g.
 * 60=... 64=... to try the gain registers (0x7ff0 = stock fw value).
 *
 * Supports PCM 16-bit, mono or stereo, any rate up to 96 kHz (resampled
 * to the hardware's 48 kHz with linear interpolation). STANDBY, a serial
 * key or any remote key stops.
 *
 * Memory: the file sits at 0x81600000 (phys 0x01600000). Display
 * write-back starts at phys 0x0469dc00, and the audio buffers are moved
 * to phys 0x04000000 (0x90000 bytes), so files up to ~42 MB fit.
 */
#define AUD_BUF_PHYS    0x04000000      /* past a file of up to ~42 MB */

#include "board.h"
#include "audio.h"
#include "ir.h"

#define CHUNK           512
#define WAV_MAX_END     0x84000000      /* audio buffers start here */

int memcmp (const void *a, const void *b, unsigned int n);    /* libc.c */

static short pcm[CHUNK * 2];

static u32 rd16 (const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static u32 rd32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static int has_eq (const char *s) {
    for (; *s; s++) {
        if (*s == '=') {
            return 1;
        }
    }
    return 0;
}

static const char *after_eq (const char *s) {
    while (*s && *s != '=') {
        s++;
    }
    return *s ? s + 1 : s;
}

static u32 parse_dec (const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

int main (int argc, char *argv[]) {
    const unsigned char *file, *p, *end, *data = 0;
    u32 load = (argc > 1) ? parse_hex (argv[1]) : 0x81600000;
    u32 fsize = 0, vol = 100, volq;
    u32 fmt = 0, chans = 0, rate = 0, bits = 0, dsize = 0;
    u32 frames, src = 0, frac = 0, step, start, last = 0;
    struct ir_event ev;
    int i;

    for (i = 2; i < argc; i++) {
        if (argv[i][0] == 'v' && argv[i][1] == 'o' && argv[i][2] == 'l') {
            vol = parse_dec (after_eq (argv[i]));
        } else if (!has_eq (argv[i])) {
            fsize = parse_hex (argv[i]);
        }
    }
    if (vol > 100) {
        vol = 100;
    }
    volq = vol * 256 / 100;

    file = (const unsigned char *) load;
    if (memcmp (file, "RIFF", 4) != 0 || memcmp (file + 8, "WAVE", 4) != 0) {
        printf ("wavplay: no RIFF/WAVE header at 0x%08x\n", load);
        printf ("  fatload usb 0 81600000 testwav.wav; go ${a} 81600000 ${filesize}\n");
        return 1;
    }
    end = file + (fsize ? fsize : 8 + rd32 (file + 4));
    if ((u32) end > WAV_MAX_END) {
        end = (const unsigned char *) WAV_MAX_END;
    }

    /* Walk the chunks for "fmt " and "data" */
    for (p = file + 12; p + 8 <= end; p += 8 + ((rd32 (p + 4) + 1) & ~1u)) {
        if (memcmp (p, "fmt ", 4) == 0) {
            fmt = rd16 (p + 8);
            chans = rd16 (p + 10);
            rate = rd32 (p + 12);
            bits = rd16 (p + 22);
        } else if (memcmp (p, "data", 4) == 0) {
            data = p + 8;
            dsize = rd32 (p + 4);
            break;
        }
    }
    if (!data) {
        printf ("wavplay: no data chunk\n");
        return 1;
    }
    if (data + dsize > end) {
        dsize = end - data;
        printf ("wavplay: file cut short, playing the %d bytes that are loaded\n", dsize);
    }
    printf ("wavplay: format %d, %d ch, %d Hz, %d bit, %d data bytes\n",
            fmt, chans, rate, bits, dsize);
    if ((fmt != 1 && fmt != 0xfffe) || bits != 16 || chans < 1 || chans > 2 ||
        rate < 8000 || rate > 96000) {
        printf ("wavplay: only PCM 16-bit mono/stereo, 8-96 kHz\n");
        return 1;
    }

    frames = dsize / (2 * chans);
    step = (rate << 12) / 3000;         /* rate * 65536 / 48000, 16.16 */
    printf ("wavplay: %d:%02d long. Stop: STANDBY, serial key or remote key.\n",
            frames / rate / 60, frames / rate % 60);

    ir_init ();
    audio_start ();
    for (i = 2; i < argc; i++) {
        if (has_eq (argv[i]) && argv[i][0] != 'v') {
            u32 off = parse_hex (argv[i]) & 0x1fc;

            AUD_REG (off) = parse_hex (after_eq (argv[i]));
            printf ("wavplay: reg +%03x = %08x\n", off, AUD_REG (off));
        }
    }
    printf ("wavplay: volume %d%%, +60..+7c: %08x %08x %08x %08x %08x %08x %08x %08x\n", vol,
            AUD_REG (0x60), AUD_REG (0x64), AUD_REG (0x68), AUD_REG (0x6c),
            AUD_REG (0x70), AUD_REG (0x74), AUD_REG (0x78), AUD_REG (0x7c));
    while (tstc ()) {
        getc ();
    }

    start = get_timer (0);
    while (src + 1 < frames && !standby_pressed () && !tstc () && !ir_poll (&ev)) {
        u32 n = audio_space ();
        u32 k, now;

        if (n > CHUNK) {
            n = CHUNK;
        }
        for (k = 0; k < n && src + 1 < frames; k++) {
            const short *a = (const short *) data + src * chans;
            const short *b = a + chans;
            int l = a[0] + (((b[0] - a[0]) * (int) frac) >> 16);
            int r = (chans == 2) ? a[1] + (((b[1] - a[1]) * (int) frac) >> 16) : l;

            l = (l * (int) volq) >> 8;
            r = (r * (int) volq) >> 8;

            /* Word = hi << 16 | lo, and WAV stores L then R, so R = hi,
             * L = lo (memory order). L/R not checked on hardware yet. */
            pcm[2 * k] = r;
            pcm[2 * k + 1] = l;

            frac += step;
            src += frac >> 16;
            frac &= 0xffff;
        }
        if (k) {
            audio_write (pcm, k);
        }

        now = get_timer (start);
        if (now - last >= 1000) {
            u32 t = src / rate;

            last = now;
            printf ("\r  %d:%02d / %d:%02d ", t / 60, t % 60,
                    frames / rate / 60, frames / rate % 60);
        }
    }

    /* Let what is still in the buffer play out (max ~0.4 s) */
    start = get_timer (0);
    while ((AUD_REG (0x104) & AUD_MASK) > 0x40 && get_timer (start) < 500) {
        udelay (1000);
    }
    if (tstc ()) {
        getc ();
    }
    while (standby_pressed ()) {
        udelay (10000);
    }

    audio_stop ();
    printf ("\nwavplay done\n");
    return 0;
}
