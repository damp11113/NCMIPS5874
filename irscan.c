/*
 * irscan: find the IR receiver pin.
 *
 *   go ${a}
 *
 * Reads the GPIO input registers as fast as possible for 5 seconds and
 * counts how often each bit toggles. An IR receiver pin toggles dozens of
 * times per remote key press; buttons only a few times. Read-only.
 */
#include "board.h"

#define SCAN_MS 5000

static const u32 in_regs[] = {
    0xbf0a0008,     /* bank 0, pins 0-31 */
    0xbf0a0018,     /* bank 1, pins 32-63 */
    0xbf155008,     /* bank 2, pins 64-71 */
};
#define NREGS 3

static u32 toggles[NREGS][32];

int main (int argc, char *argv[]) {
    u32 prev[NREGS], start, loops = 0;
    int r, b, found = 0;

    printf ("Point a remote at the box and hold/press buttons for 5 seconds...\n");
    printf ("Starting in 1 second.\n");
    udelay (1000000);
    printf ("GO!\n");

    for (r = 0; r < NREGS; r++) {
        prev[r] = REG32 (in_regs[r]);
    }

    start = get_timer (0);
    while (get_timer (start) < SCAN_MS) {
        for (r = 0; r < NREGS; r++) {
            u32 now = REG32 (in_regs[r]);
            u32 diff = now ^ prev[r];
            if (diff) {
                for (b = 0; b < 32; b++) {
                    if (diff & (1u << b)) {
                        toggles[r][b]++;
                    }
                }
                prev[r] = now;
            }
        }
        loops++;
    }

    printf ("Done, %d samples (%d per ms).\n\n", loops, loops / SCAN_MS);
    for (r = 0; r < NREGS; r++) {
        for (b = 0; b < 32; b++) {
            if (toggles[r][b]) {
                printf ("  reg 0x%08x bit %2d (pin %2d): %d toggles\n",
                        in_regs[r], b, r * 32 + b, toggles[r][b]);
                found++;
            }
        }
    }
    if (!found) {
        printf ("  No input bit toggled. IR is not on these GPIO banks.\n");
    }
    return 0;
}
