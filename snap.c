/*
 * snap: print every register in snapranges.h (read-only), U-Boot version.
 * Run after 'source avstart.scr' to capture U-Boot's 1080i video state.
 * Lines look like "S bf440000: w0 w1 w2 w3" so logs can be diffed.
 */
#include "uboot.h"
#include "snapranges.h"

int main(int argc, char *argv[]) {
    u32 r, i;

    printf("=== SNAP BEGIN (u-boot) ===\n");
    for (r = 0; r < SNAP_NRANGES; r++) {
        for (i = 0; i < snap_ranges[r].words; i += 4) {
            u32 a = snap_ranges[r].start + i * 4;
            printf("S %08x: %08x %08x %08x %08x\n", a,
                    REG32(a), REG32(a + 4), REG32(a + 8), REG32(a + 12));
        }
    }
    printf("=== SNAP END ===\n");
    return 0;
}
