/*
 * vsyncprobe: find a register that follows the display scan (for vsync).
 *
 *   source avstart.scr first, then:  go ${a}
 *
 * Read-only. Samples candidate registers as fast as possible for 1 second
 * and reports, per register: distinct values seen, min/max, and how often
 * the value went DOWN (a wrap = start of a new field/frame for a line
 * counter). A real line counter wraps ~50 times/s at 1080i50 (per field).
 */
#include "uboot.h"

#define NREG 8

static const u32 regs[NREG] = {
    0xbf440140, 0xbf440144, 0xbf470080, 0xbf470084,
    0xbf47008c, 0xbf470090, 0xbf470094, 0xbf440004,
};

static u32 vmin[NREG], vmax[NREG], wraps[NREG], changes[NREG], last[NREG];

static u32 count(void) {
    u32 v;
    __asm__ volatile("mfc0 %0, $9" : "=r" (v));
    return v;
}

int main(int argc, char *argv[]) {
    u32 i, t0, samples = 0;
    const u32 one_second = 324000000u;      /* CP0 Count ticks (cpuinfo) */

    for (i = 0; i < NREG; i++) {
        last[i] = vmin[i] = vmax[i] = REG32(regs[i]);
    }
    t0 = count();
    while (count() - t0 < one_second) {
        for (i = 0; i < NREG; i++) {
            u32 v = REG32(regs[i]);
            if (v != last[i]) {
                changes[i]++;
                if (v < last[i]) {
                    wraps[i]++;
                }
                last[i] = v;
            }
            if (v < vmin[i]) vmin[i] = v;
            if (v > vmax[i]) vmax[i] = v;
        }
        samples++;
    }

    printf("%d sample rounds in 1 s\n", samples);
    printf("register    changes/s  wraps/s   min        max\n");
    for (i = 0; i < NREG; i++) {
        printf("%08x  %9d  %7d   %08x   %08x\n",
                regs[i], changes[i], wraps[i], vmin[i], vmax[i]);
    }
    return 0;
}
