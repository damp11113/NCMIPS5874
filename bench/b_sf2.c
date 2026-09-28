/* SoundFont mixer, current code: the real mix_voice () of synth_sf2.c */
#include "../apps/midi/synth_sf2.c"
#include "bench.h"

#define SF2_RUN sf2_run_new
#include "sf2_run.h"

u32 bench_sf2_new(int *outl, int *outr) {
    return sf2_run_new(outl, outr);
}
