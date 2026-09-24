/*
 * audplay: first own sound. Streams a sine tone through audio.h for up
 * to 20 s (HDMI + RCA), printing the ring-buffer pointers twice a second.
 *
 *   go ${a}          440 Hz
 *   go ${a} <hz>     other frequency (decimal)
 *   go ${a} <hz> x   also set HDMI TX reg 0x50 = 0x01 (stock fw value)
 *
 * Run after 'source avstart.scr' (HDMI up). STANDBY or a serial key stops.
 * Needs sine256.h: python mksine.py > sine256.h
 */
#include "board.h"
#include "audio.h"
#include "sine256.h"

#define CHUNK 256

static short pcm[CHUNK * 2];

static u32 parse_dec (const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

static void dump_regs (const char *tag) {
    u32 a;

    printf ("=== audio regs (%s) ===\n", tag);
    for (a = AUD_BASE; a < AUD_BASE + 0x180; a += 16) {
        printf ("S %08x: %08x %08x %08x %08x\n", a,
                REG32 (a), REG32 (a + 4), REG32 (a + 8), REG32 (a + 12));
    }
}

int main (int argc, char *argv[]) {
    u32 hz = (argc > 1) ? parse_dec (argv[1]) : 440;
    u32 phase = 0, step, start, last = 0, frames = 0;
    int tx_done = 0;

    if (hz == 0 || hz > 20000) {
        hz = 440;
    }
    /* phase step = hz * 2^32 / 48000 = hz * 89478.485 (no 64-bit divide) */
    step = hz * 89478 + hz * 485 / 1000;

    dump_regs ("before");
    audio_start ();
    if (argc > 2 && argv[2][0] == 'x') {
        REG8 (0xbf480050) = 0x01;       /* HDMI TX reg 0x50 as stock fw */
    }
    printf ("HDMI TX 0x50 %02x 0x51 %02x\n", REG8 (0xbf480050), REG8 (0xbf480051));
    printf ("audio_start: ctrl %08x base %08x size %08x read %08x write %08x\n",
            AUD_REG (0x00), AUD_REG (0x18), AUD_REG (0x1c),
            AUD_REG (0x100), AUD_REG (0x10c));
    printf ("Playing %d Hz. STANDBY or any serial key stops.\n", hz);
    printf ("T ms: read0 level0 write0 sample fifo | read1 level1 +118\n");

    while (tstc ()) {
        getc ();                /* drop leftover input (e.g. Enter) */
    }

    start = get_timer (0);
    while (!standby_pressed () && !tstc () && get_timer (start) < 20000) {
        u32 n = audio_space ();
        u32 i, now;

        if (n > CHUNK) {
            n = CHUNK;
        }
        for (i = 0; i < n; i++) {
            short s = sine256[phase >> 24] / 4;     /* about -12 dB */

            pcm[2 * i] = s;
            pcm[2 * i + 1] = s;
            phase += step;
        }
        if (n) {
            audio_write (pcm, n);
            frames += n;
        }

        now = get_timer (start);
        if (now - last >= 500) {
            last = now;
            printf ("T %5d: %08x %08x %08x %08x %08x | %08x %08x %08x (%d frames)\n", now,
                    AUD_REG (0x100), AUD_REG (0x104), AUD_REG (0x10c),
                    AUD_REG (0x130), AUD_REG (0x140),
                    AUD_REG (0x110), AUD_REG (0x114), AUD_REG (0x118), frames);
            if (now >= 2000 && !tx_done) {
                /* HDMI TX registers while playing, same "X" lines as
                 * hook_audsnap (a short audio gap while printing is OK) */
                u32 a;

                for (a = 0xbf480000; a < 0xbf480200; a += 16) {
                    printf ("X %08x: %08x %08x %08x %08x\n", a,
                            REG32 (a), REG32 (a + 4), REG32 (a + 8), REG32 (a + 12));
                }
                tx_done = 1;
            }
        }
    }
    if (tstc ()) {
        getc ();
    }
    while (standby_pressed ()) {
        udelay (10000);
    }

    audio_stop ();
    dump_regs ("after");
    printf ("audplay done, %d frames\n", frames);
    return 0;
}
