/*
 * hook_hdr: runs inside the original firmware (read-only).
 * The OSD layer address registers point to a region HEADER, not pixels.
 * Dump the start of each header, then follow every word that looks like
 * a RAM address (plain or >> 3) and dump a little of what it points to.
 */
typedef unsigned int u32;
typedef int (*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))

#define HDR_WORDS   24
#define RAM_SIZE    0x08000000u     /* 128 MB */

static void dump_words (printf_t pf, u32 phys, u32 words) {
    volatile u32 *p = (volatile u32 *) (0xa0000000u | phys);
    u32 i;

    for (i = 0; i < words; i += 4) {
        pf ("  %08x: %08x %08x %08x %08x\n", phys + i * 4, p[i], p[i + 1], p[i + 2], p[i + 3]);
    }
}

static void follow (printf_t pf, u32 hdr_phys, u32 idx, u32 v) {
    u32 cand[2] = { v, v << 3 };
    int k;

    for (k = 0; k < 2; k++) {
        u32 a = cand[k];
        if (a >= 0x00100000u && a < RAM_SIZE && (a & 3) == 0 && a != hdr_phys) {
            pf (" word %d = %08x -> %s %08x:\n", idx, v, k ? "(x8)" : "", a);
            dump_words (pf, a, 8);
        }
    }
}

static void layer (printf_t pf, const char *name, u32 reg) {
    u32 phys = (REG32 (reg) & 0x03ffffffu) << 3;
    volatile u32 *h = (volatile u32 *) (0xa0000000u | phys);
    u32 i;

    pf ("--- %s: reg %08x = %08x, header phys %08x ---\n", name, reg, REG32 (reg), phys);
    dump_words (pf, phys, HDR_WORDS);
    for (i = 0; i < HDR_WORDS; i++) {
        follow (pf, phys, i, h[i]);
    }
}

void hook_main (printf_t pf) {
    pf ("\n=== HOOK HDR BEGIN ===\n");
    layer (pf, "layer6", 0xbf441028);
    layer (pf, "layer5", 0xbf441030);
    layer (pf, "layer1", 0xbf44102c);
    layer (pf, "layer7", 0xbf441024);
    pf ("=== HOOK HDR END ===\n");
}
