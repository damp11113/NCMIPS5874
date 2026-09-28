/* Included by b_sf2.c (current synth_sf2.c) and b_sf2_ref.c (the committed
 * copy in ref_sf2.c): 64 looping voices at different pitches, every other
 * one centred (gain_l == gain_r), 1 s of 48 kHz audio in 32-sample blocks.
 * Returns the CP0 ticks spent in mix_voice (). */
#define NV      64
#define NSMP    65536

static short wave[NSMP + 2];

static u32 SF2_RUN(int *outl, int *outr) {
    static int left[BLOCK], right[BLOCK];
    u32 seed = 12345, t = 0, t0;
    int i, b, x = 0;

    for (i = 0; i < NSMP + 2; i++) {        /* smooth random wave */
        seed = seed * 1103515245u + 12345u;
        x += (int) ((seed >> 16) & 2047) - 1024;
        x = x > 30000 ? 30000 : x < -30000 ? -30000 : x;
        wave[i] = (short) x;
    }
    memset(voices, 0, sizeof(voices));
    for (i = 0; i < NV; i++) {
        struct voice *v = &voices[i];

        v->active = 1;
        v->data = wave;
        v->pos = 1000 + i * 797;
        v->step = 0x8000 + i * 1777;         /* 0.5x .. 2.2x */
        v->end = NSMP;
        v->loop_start = 1000 + i * 13;
        v->loop_end = 60000 - i * 17;
        v->loop_mode = 1;
        v->gain_l = 8000 + i * 100;
        v->gain_r = i & 1 ? 12000 - i * 90 : v->gain_l;
    }
    for (b = 0; b < SYNTH_RATE / BLOCK; b++) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        t0 = bench_ticks();
        for (i = 0; i < NV; i++) {
            mix_voice(&voices[i], left, right, BLOCK);
        }
        t += bench_ticks() - t0;
        memcpy(outl + b * BLOCK, left, sizeof(left));
        memcpy(outr + b * BLOCK, right, sizeof(right));
    }
    return t;
}
