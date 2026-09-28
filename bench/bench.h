/*
 * dspbench: time the hot loops of the SDK apps with the real code. The
 * same sources are built twice (bench/build.sh): BENCHP.BIN plain,
 * BENCHD.BIN with -mdsp (MIPS DSP ASE rev 1). Same checksums in both runs
 * = same output bit for bit.
 */
#ifndef BENCH_H
#define BENCH_H

#include "sdk.h"

/* CP0 Count: 324 MHz on these boxes (CPU clock / 2) */
static inline u32 bench_ticks(void) {
    u32 c;

    __asm__ volatile("mfc0 %0, $9" : "=r" (c));
    return c;
}

#define BENCH_HZ    324000000u

struct bench_result {
    u32 ticks;          /* time of the measured part */
    u32 audio_ms;       /* real time of the audio it produced (0 = not audio) */
    u32 sum;            /* checksum of the output */
};

/* 1 s of 48 kHz stereo into outl / outr (48000 ints each), returns ticks.
 * _new = current code, _ref = committed copy (bench/ref_*.c) */
u32 bench_sf2_new(int *outl, int *outr);
u32 bench_sf2_ref(int *outl, int *outr);
u32 bench_opl_new(int *outl, int *outr);
u32 bench_opl_ref(int *outl, int *outr);
int bench_mp3(struct bench_result *r, const unsigned char *mp3, int len);
void bench_doom(struct bench_result *r, struct bench_result *copy);

#endif
