/*
 * regdump: print blocks of registers once (read-only).
 *
 *   go ${a}                       dump the built-in display candidate list
 *   go ${a} <start-hex> <words-hex> [<start-hex> <words-hex> ...]
 *
 * All-zero lines are skipped to keep the output short.
 */
#include "uboot.h"

struct range {
    u32 start, words;
};

/* Display-related blocks seen in U-Boot's display driver */
static const struct range display_ranges[] = {
    { 0xbf440000, 0x80 },
    { 0xbf441000, 0x40 },
    { 0xbf470000, 0x40 },
    { 0xbf460000, 0x40 },
    { 0xbf260000, 0x60 },
    { 0xbf261100, 0x10 },
    { 0xbf261500, 0x10 },
    { 0xbf270000, 0x40 },
    { 0xbf410000, 0x68 },
};

static void dump(u32 start, u32 words) {
    u32 i, j;

    start &= ~3u;
    if (start < 0xa0000000u || start >= 0xc0000000u) {
        printf("skip 0x%08x: not in 0xa0000000..0xbfffffff\n", start);
        return;
    }
    printf("--- 0x%08x (%d words) ---\n", start, words);
    for (i = 0; i < words; i += 4) {
        u32 v[4], any = 0;
        for (j = 0; j < 4; j++) {
            v[j] = (i + j < words) ? REG32(start + (i + j) * 4) : 0;
            any |= v[j];
        }
        if (any) {
            printf("%08x: %08x %08x %08x %08x\n", start + i * 4, v[0], v[1], v[2], v[3]);
        }
    }
}

int main(int argc, char *argv[]) {
    int i;

    if (argc >= 3) {
        for (i = 1; i + 1 < argc; i += 2) {
            dump(parse_hex(argv[i]), parse_hex(argv[i + 1]));
        }
    } else {
        for (i = 0; i < (int) (sizeof(display_ranges) / sizeof(display_ranges[0])); i++) {
            dump(display_ranges[i].start, display_ranges[i].words);
        }
    }
    printf("--- end ---\n");
    return 0;
}
