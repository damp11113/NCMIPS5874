/* OPL FM emulator, reference: the committed sdk/opl.c (bench/ref_opl.c,
 * from git show HEAD:sdk/opl.c), public names renamed */
#define opl_init    ref_opl_init
#define opl_reset   ref_opl_reset
#define opl_write   ref_opl_write
#define opl_render  ref_opl_render
#include "ref_opl.c"
#include "bench.h"

#define OPL_RUN opl_run_ref
#include "opl_run.h"

u32 bench_opl_ref(int *outl, int *outr) {
    return opl_run_ref(outl, outr);
}
