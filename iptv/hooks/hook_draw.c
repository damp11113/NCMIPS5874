/*
 * hook_draw: runs inside the original firmware (same hook as hook_dump).
 * Draws 8 test bars into the pixel buffers of both OSD planes and holds
 * them for 15 seconds, so the colours reveal the pixel format.
 *
 * Found with hook_dump / hook_hdr:
 *   layer address register (0xbf441028 layer 6, 0xbf441030 layer 5)
 *     = region header physical address >> 3
 *   region header (do NOT overwrite it):
 *     word 2: height << 16 | width        (0x02d00500 = 720 x 1280)
 *     word 3: pitch_px << 16 | alpha      (0x050000ff = 1280 px, alpha 0xff)
 *     word 4: pixel buffer, uncached CPU address (0xa2a99450)
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef int (*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))

#define COUNT_HZ    324000000u  /* CP0 Count rate measured by cpuinfo */

/* Bar values chosen so each 16-bit format gives a different colour set */
static const u16 bars[8] = {
    0xf800,     /* RGB565 red      | ARGB1555 opaque red   | ARGB4444 opaque dark red */
    0x07e0,     /* RGB565 green    | ARGB1555 transparent  | ARGB4444 transparent     */
    0x001f,     /* RGB565 blue     | ARGB1555 transparent  | ARGB4444 transparent     */
    0xffff,     /* white in all */
    0x7c00,     /* RGB565 dark olive | ARGB1555 transparent red                       */
    0x83e0,     /* RGB565 olive    | ARGB1555 opaque green                            */
    0xf0f0,     /* RGB565 pink-red | ARGB4444 opaque green                            */
    0x0000,     /* black or transparent */
};

struct plane {
    volatile u32 *hdr;
    volatile u16 *pix;
    u32 w, h, pitch;
};

static u32 read_count (void) {
    u32 v;
    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

static void wait_seconds (u32 s) {
    while (s--) {
        u32 t0 = read_count ();
        while (read_count () - t0 < COUNT_HZ) {
        }
    }
}

/* Returns 0 if the header does not look sane */
static int plane_get (printf_t pf, const char *name, u32 reg, struct plane *p) {
    u32 phys = (REG32 (reg) & 0x03ffffffu) << 3;
    u32 buf;

    p->hdr = (volatile u32 *) (0xa0000000u | phys);
    p->w = p->hdr[2] & 0xffff;
    p->h = p->hdr[2] >> 16;
    p->pitch = p->hdr[3] >> 16;
    buf = p->hdr[4];
    p->pix = (volatile u16 *) buf;

    pf ("%s: hdr %08x, %dx%d, pitch %d px, pixels at %08x\n",
        name, phys, p->w, p->h, p->pitch, buf);
    if (p->w == 0 || p->w > 1920 || p->h == 0 || p->h > 1080 || p->pitch < p->w
            || buf < 0xa0100000u || buf >= 0xa8000000u) {
        pf ("%s: header looks wrong, not drawing\n", name);
        return 0;
    }
    pf ("%s: first px %04x %04x, centre px %04x\n", name, p->pix[0], p->pix[1],
        p->pix[(p->h / 2) * p->pitch + p->w / 2]);
    return 1;
}

static void draw_bars (struct plane *p, int vertical) {
    u32 x, y;

    for (y = 0; y < p->h; y++) {
        for (x = 0; x < p->w; x++) {
            u32 bar = vertical ? x * 8 / p->w : y * 8 / p->h;
            p->pix[y * p->pitch + x] = bars[bar];
        }
    }
}

void hook_main (printf_t pf) {
    struct plane l6, l5;
    int ok6, ok5;

    pf ("\n=== HOOK DRAW ===\n");
    ok6 = plane_get (pf, "layer6", 0xbf441028, &l6);
    ok5 = plane_get (pf, "layer5", 0xbf441030, &l5);

    if (ok6) {
        draw_bars (&l6, 1);     /* layer 6: vertical bars */
    }
    if (ok5) {
        draw_bars (&l5, 0);     /* layer 5: horizontal bars */
    }
    pf ("Drew bars: layer6 = VERTICAL, layer5 = HORIZONTAL.\n");
    pf ("Bar values: f800 07e0 001f ffff 7c00 83e0 f0f0 0000. Holding 15 s...\n");

    wait_seconds (15);
    pf ("=== HOOK DRAW END, firmware continues ===\n");
}
