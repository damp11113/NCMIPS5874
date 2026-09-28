/*
 * dspbench: go ${a} [<mp3 addr> <mp3 size>]   (hex, e.g. after
 * fatload usb 0 0x82000000 misery.mp3: go ${a} 82000000 ${filesize})
 * Prints time and checksum per kernel; run BENCHP.BIN and BENCHD.BIN and
 * compare. See bench.h.
 */
#include <stdlib.h>

#include "bench.h"

static void report(const char *name, const struct bench_result *r, const char *unit, u32 units) {
    u32 us = (u32) ((unsigned long long) r->ticks * 1000000 / BENCH_HZ);

    printf("  %-22s %9u ticks  %7u us", name, r->ticks, us);
    if (r->audio_ms) {
        /* CPU share when running in real time, in 0.01 % */
        u32 cpu = (u32) ((unsigned long long) r->ticks * 10000 * 1000 / BENCH_HZ / r->audio_ms);

        printf("  CPU %u.%02u %%", cpu / 100, cpu % 100);
    }
    if (units) {
        printf("  %u.%02u cycles/%s", 2 * r->ticks / units,
                (u32) (200ull * r->ticks / units % 100), unit);
    }
    printf("  sum %08x\n", r->sum);
}

/* Run current and committed code on the same input, compare every sample */
static void compare(const char *name, u32 (*fnew)(int *, int *), u32 (*fref)(int *, int *),
                    const char *unit, u32 units) {
    static int nl[48000], nr[48000], rl[48000], rr[48000];
    struct bench_result r;
    u32 tref, diff = 0, first = 0, h = 0;
    int i;

    r.ticks = fnew(nl, nr);
    tref = fref(rl, rr);
    for (i = 0; i < 48000; i++) {
        if (nl[i] != rl[i] || nr[i] != rr[i]) {
            if (!diff++) {
                first = i;
            }
        }
        h = h * 31 + (u32) nl[i] * 7 + (u32) nr[i];
    }
    r.audio_ms = 1000;
    r.sum = h;
    report(name, &r, unit, units);
    printf("  %-22s committed code: %u ticks -> %u.%02ux faster, ", "", tref,
            tref / r.ticks, tref % r.ticks * 100 / r.ticks);
    if (diff) {
        printf("%u SAMPLES DIFFER (first at %u: %d/%d vs %d/%d)\n", diff, first,
                nl[first], nr[first], rl[first], rr[first]);
    } else {
        printf("output identical\n");
    }
}

int main(int argc, char **argv) {
    struct bench_result r, c;
    const char *mp3 = 0;
    int mp3_len = 0;

#ifdef __mips_dsp
    printf("dspbench: built WITH -mdsp (DSP ASE rev %d)\n", __mips_dsp_rev);
#else
    printf("dspbench: built WITHOUT -mdsp\n");
#endif
    if (argc >= 3) {
        mp3 = (const char *) strtoul(argv[1], 0, 16);
        mp3_len = (int) strtoul(argv[2], 0, 16);
    }

    compare("SF2 mixer 64 voices", bench_sf2_new, bench_sf2_ref, "voice-sample", 64 * 48000);
    compare("OPL 18 channels", bench_opl_new, bench_opl_ref, "channel-sample", 18 * 48000);
    if (mp3 && mp3_len > 0) {
        if (bench_mp3(&r, (const unsigned char *) mp3, mp3_len)) {
            report("MP3 decode (Helix)", &r, 0, 0);
        }
    } else {
        printf("  MP3: no file given (args: <addr> <size> in hex)\n");
    }
    bench_doom(&r, &c);
    report("DOOM expand_line x20", &r, "frame", 20);
    report("DOOM draw full x20", &c, "frame", 20);
    return 0;
}
