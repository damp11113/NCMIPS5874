/*
 * hook_snap: print every register in snapranges.h (read-only), from inside
 * the stock firmware (hook) to capture its 1080p video state.
 * Same line format as snap.c for diffing.
 */
#include "snapranges.h"

typedef unsigned int u32;
typedef int(*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))

void hook_main(printf_t pf) {
    u32 r, i;

    pf("\n=== SNAP BEGIN (firmware) ===\n");
    for (r = 0; r < SNAP_NRANGES; r++) {
        for (i = 0; i < snap_ranges[r].words; i += 4) {
            u32 a = snap_ranges[r].start + i * 4;
            pf("S %08x: %08x %08x %08x %08x\n", a,
                REG32(a), REG32(a + 4), REG32(a + 8), REG32(a + 12));
        }
    }
    pf("=== SNAP END ===\n");
}
