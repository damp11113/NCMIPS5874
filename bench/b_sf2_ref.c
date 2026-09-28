/* SoundFont mixer, reference: the committed synth_sf2.c (bench/ref_sf2.c,
 * from git show HEAD:apps/midi/synth_sf2.c), public names renamed */
#define synth_sf2       ref_synth_sf2
#define synth_sf2_init  ref_synth_sf2_init
#define synth_sf2_name  ref_synth_sf2_name
#define synth_sf2_limit ref_synth_sf2_limit
#include "ref_sf2.c"
#include "bench.h"

#define SF2_RUN sf2_run_ref
#include "sf2_run.h"

u32 bench_sf2_ref(int *outl, int *outr) {
    return sf2_run_ref(outl, outr);
}
