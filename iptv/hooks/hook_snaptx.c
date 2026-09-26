/*
 * hook_snaptx: dump the HDMI transmitter's byte registers from inside the
 * stock firmware, i.e. in its working 1080p50 state. Same format as snaptx.
 */
#include "snaptx.h"

void hook_main(txprint_t pf) {
    pf("\n=== SNAPTX BEGIN (firmware) ===\n");
    snaptx_dump(pf);
    pf("=== SNAPTX END ===\n");
}
