/*
 * hook_dump: runs inside the original firmware once its display is up,
 * prints the display registers with the firmware's own printf, then
 * returns so the firmware keeps booting normally.
 *
 * Linked at 0x80004000 (free RAM below the firmware image).
 * No U-Boot services are available here: only the printf pointer.
 */
typedef unsigned int u32;
typedef int(*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))

struct range {
    u32 start, words;
};

static const struct range ranges[] = {
    { 0xbf440000, 0x80 },   /* display mixer / layers */
    { 0xbf441000, 0x40 },   /* layer buffer addresses */
    { 0xbf442000, 0x20 },
    { 0xbf443000, 0x20 },
    { 0xbf470000, 0x40 },   /* HDMI timing */
    { 0xbf260000, 0x60 },
    { 0xbf261100, 0x10 },
    { 0xbf261500, 0x10 },
};

void hook_main(printf_t pf) {
    u32 r, i;

    pf("\n=== HOOK DUMP BEGIN ===\n");
    for (r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        u32 base = ranges[r].start;

        pf("--- 0x%08x ---\n", base);
        for (i = 0; i < ranges[r].words; i += 4) {
            u32 a = base + i * 4;
            u32 v0 = REG32(a), v1 = REG32(a + 4), v2 = REG32(a + 8), v3 = REG32(a + 12);

            if (v0 | v1 | v2 | v3) {
                pf("%08x: %08x %08x %08x %08x\n", a, v0, v1, v2, v3);
            }
        }
    }
    pf("=== HOOK DUMP END ===\n");
}
