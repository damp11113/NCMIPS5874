/* OPL FM emulator, current code (sdk/opl.c is linked) */
#include <string.h>

#include "opl.h"
#include "bench.h"

#define OPL_RUN opl_run_new
#include "opl_run.h"

u32 bench_opl_new(int *outl, int *outr) {
    return opl_run_new(outl, outr);
}
