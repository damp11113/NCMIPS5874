/*
 * regwatch: read-only register watcher.
 *
 *   go ${a} <start-hex> [words-hex]
 *   go ${a} bf15c000 40
 *
 * Takes a snapshot of <words> 32-bit registers from <start>, then keeps
 * re-reading them and prints every change. Press a key on the serial
 * console to quit. Never writes to hardware.
 */
#include "uboot.h"

#define MAX_WORDS 256

static u32 snap[MAX_WORDS];

int main (int argc, char *argv[]) {
    u32 start, words, i, changes = 0;

    if (argc < 2) {
        printf ("usage: go <addr> <start-hex> [words-hex, max 100]\n");
        printf ("  e.g. go ${a} bf15c000 40\n");
        return 1;
    }

    start = parse_hex (argv[1]) & ~3u;
    if (start < 0xa0000000u || start >= 0xc0000000u) {
        printf ("Refusing 0x%08x: use an uncached address 0xa0000000..0xbfffffff\n", start);
        printf ("  (full 8 hex digits, e.g. bf15c000)\n");
        return 1;
    }
    words = (argc > 2) ? parse_hex (argv[2]) : 0x40;
    if (words == 0 || words > MAX_WORDS) {
        words = MAX_WORDS;
    }

    printf ("Snapshot 0x%08x .. 0x%08x (%d words)\n", start, start + words * 4 - 4, words);
    for (i = 0; i < words; i++) {
        snap[i] = REG32 (start + i * 4);
    }
    for (i = 0; i < words; i++) {
        if (i % 4 == 0) {
            printf ("\n%08x:", start + i * 4);
        }
        printf (" %08x", snap[i]);
    }
    printf ("\n\nWatching. Press/release the STANDBY button, point the remote and press keys.\n");
    printf ("Press any key on the serial console to stop.\n\n");

    while (!tstc ()) {
        for (i = 0; i < words; i++) {
            u32 now = REG32 (start + i * 4);
            if (now != snap[i]) {
                printf ("%08x: %08x -> %08x  (changed bits %08x)\n",
                        start + i * 4, snap[i], now, snap[i] ^ now);
                snap[i] = now;
                changes++;
            }
        }
        udelay (20000);
    }
    getc ();

    printf ("Stopped, %d changes seen.\n", changes);
    return 0;
}
