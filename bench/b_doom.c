/* DOOM frame drawing (doom/dg_nc5874.c): 8-bit rows -> palette LUT ->
 * 3x wide ARGB1555 pixel pairs (expand_line), then the 3.6x row copy to
 * uncached memory like the OSD plane (the real draw cost) */
#include <string.h>

#include "bench.h"

#define SW      320
#define SH      200
#define OUT_W   960
#define OUT_H   720
#define FRAMES  20

static u32 lut[256];
static u32 line_buf[OUT_W / 2];
static unsigned char frame[SW * SH];

/* Same as doom/dg_nc5874.c */
static void expand_line(const unsigned char *src) {
    u32 *out = line_buf;
    int x;

    for (x = 0; x < SW; x += 2) {
        u32 a = lut[src[x]], b = lut[src[x + 1]];

        out[0] = a;
        out[1] = (a & 0xffff0000u) | (b & 0xffff);
        out[2] = b;
        out += 3;
    }
}

/* r: expand only; copy: expand + row copy to uncached RAM (phys 0x04400000,
 * the SDK's OSD plane area: nothing else lives there at the U-Boot prompt) */
void bench_doom(struct bench_result *r, struct bench_result *copy) {
    volatile u32 *dst0 = (volatile u32 *) 0xa4400000u;
    u32 seed = 777, t = 0, tc = 0, h = 0, t0;
    int i, f, y;

    for (i = 0; i < 256; i++) {
        u32 c = 0x8000 | ((i * 37) & 0x7fff);

        lut[i] = (c << 16) | c;
    }
    for (i = 0; i < SW * SH; i++) {
        seed = seed * 1103515245u + 12345u;
        frame[i] = (unsigned char) (seed >> 16);
    }
    for (f = 0; f < FRAMES; f++) {
        for (y = 0; y < SH; y++) {
            t0 = bench_ticks();
            expand_line(frame + y * SW);
            t += bench_ticks() - t0;
            for (i = 0; i < OUT_W / 2; i += 7) {
                h = h * 31 + line_buf[i];
            }
        }
    }
    r->ticks = t;
    r->audio_ms = 0;
    r->sum = h;

    t0 = bench_ticks();
    for (f = 0; f < FRAMES; f++) {
        for (y = 0; y < SH; y++) {
            int oy, oy_end = (y + 1) * OUT_H / SH;

            expand_line(frame + y * SW);
            for (oy = y * OUT_H / SH; oy < oy_end; oy++) {
                volatile u32 *dst = dst0 + oy * (1280 / 2);

                for (i = 0; i < OUT_W / 2; i++) {
                    dst[i] = line_buf[i];
                }
            }
        }
    }
    tc = bench_ticks() - t0;
    copy->ticks = tc;
    copy->audio_ms = 0;
    copy->sum = 0;
}
