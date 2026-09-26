/*
 * timertest: compare U-Boot's get_timer with the CP0 Count register.
 *
 *   go ${a}
 *
 * Every second of Count (324 MHz, measured by cpuinfo) prints Count,
 * get_timer (0) and get_timer (t0) for 10 s, then times a busy loop of
 * ~5 s with both clocks (like one ramtest pass). A serial key stops.
 * On the satellite box ramtest printed "pass time 0 ms" from get_timer.
 */
#include "uboot.h"

#define COUNT_HZ    324000000u

static inline u32 count(void) {
    u32 v;

    __asm__ volatile("mfc0 %0, $9" : "=r" (v));
    return v;
}

int main() {
    unsigned long g0 = get_timer(0);
    u32 c0 = count(), c, i;
    volatile u32 *p = (volatile u32 *) 0xa4000000u;

    printf("timertest: start Count %08x get_timer(0) %lu\n", c0, g0);
    for (i = 1; i <= 10 && !tstc(); i++) {
        while (count() - c0 < i * COUNT_HZ) {
        }
        c = count();
        printf("  %2u s: Count %08x  get_timer(0) %10lu  get_timer(start) %6lu ms\n",
                i, c, get_timer(0), get_timer(g0));
    }

    /* ~5 s of uncached reads (32 M reads at ~48 MB/s) */
    printf("busy loop (uncached reads, ~5 s)...\n");
    g0 = get_timer(0);
    c0 = count();
    for (i = 0; i < 64u * 1024 * 1024 / 4; i++) {
        (void) p[i & 0xffffff];
    }
    c = count();
    printf("  Count: %u ms   get_timer: %lu ms\n", (c - c0) / (COUNT_HZ / 1000), get_timer(g0));
    if (tstc()) {
        (void) getc();
    }
    return 0;
}
