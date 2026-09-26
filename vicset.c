/*
 * vicset: ask U-Boot's own HDMI driver to switch video mode.
 *
 *   source avstart.scr first, then:
 *   go ${a}          show the driver's current mode descriptor
 *   go ${a} p50      call set_mode with 1080p50 (resolution 7, rate 0)
 *   go ${a} p60      call set_mode with 1080p60 (resolution 7, rate 1)
 *   go ${a} i50      back to 1080i50 (resolution 6, rate 0)
 *
 * Found by disassembling U-Boot (link base 0x800ffff0 file offset 0):
 *   set_mode (dev, cfg)  = 0x8014876c, registered as HDMI dev ops[1]
 *     dev[0] = driver private pointer
 *     cfg+0x10 colour?, +0x14 rate (1 = 60 Hz), +0x18 resolution
 *     (1 480i, 2 576i, 3 480p, 4 576p, 5 720p, 6 1080i, 7 1080p),
 *     +0x1c aspect. to_vic () maps these to CEA VICs (1080p50 = 31).
 *   private pointer  = *(GOT[-32736] + 21072), current cfg copy right after
 *   GOT entry -32736 lives at link address 0x80188af0 (holds page 0x80190000)
 * Runtime address = link address + gd->reloc_off (gd in $k0, +0x14).
 */
#include "uboot.h"

#define LINK_SET_MODE   0x8014876cu
#define LINK_GOT_32736  0x80188af0u
#define SET_MODE_WORD0  0x3c1c0005u     /* lui gp,0x5 at set_mode entry */

static u32 reloc_off(void) {
    char *gd;
    __asm__ volatile("move %0, $26" : "=r" (gd));
    return *(u32 *) (gd + 0x14);
}

/* U-Boot functions are PIC: must be entered with $t9 = function address */
static int call2(u32 fn, u32 a0, u32 a1) {
    register u32 r_a0 __asm__("$4") = a0;
    register u32 r_a1 __asm__("$5") = a1;
    register u32 r_t9 __asm__("$25") = fn;
    register u32 r_v0 __asm__("$2");

    __asm__ volatile("jalr $25\n\tnop"
                      : "=r" (r_v0), "+r" (r_a0), "+r" (r_a1), "+r" (r_t9)
                      :
                      : "$3", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "$13",
                        "$14", "$15", "$24", "$31", "memory");
    return r_v0;
}

int main(int argc, char *argv[]) {
    u32 off = reloc_off();
    u32 set_mode = LINK_SET_MODE + off;
    u32 page = REG32(LINK_GOT_32736 + off);    /* relocated GOT entry */
    u32 *glob = (u32 *) (page + 21072);
    u32 priv = glob[0];
    u32 dev[4] = { priv, 0, 0, 0 };
    u32 cfg[16];
    int i, res = -1, rate = -1;

    printf("reloc_off %08x, set_mode at %08x (first word %08x, expect %08x)\n",
            off, set_mode, REG32(set_mode), SET_MODE_WORD0);
    printf("driver private %08x, saved cfg words: %08x %08x %08x %08x\n",
            priv, glob[1], glob[2], glob[3], glob[4]);

    if (argc < 2) {
        return 0;
    }
    if (REG32(set_mode) != SET_MODE_WORD0 || priv < 0x80000000u || priv >= 0x88000000u) {
        printf("sanity check failed, not calling set_mode\n");
        return 1;
    }
    if (!strcmp(argv[1], "p50")) {
        res = 7;
        rate = 0;
    } else if (!strcmp(argv[1], "p60")) {
        res = 7;
        rate = 1;
    } else if (!strcmp(argv[1], "i50")) {
        res = 6;
        rate = 0;
    } else {
        printf("unknown mode '%s' (p50, p60, i50)\n", argv[1]);
        return 1;
    }

    /* cfg: fields 0x10..0x1c as the driver saved them, then override */
    for (i = 0; i < 16; i++) {
        cfg[i] = 0;
    }
    cfg[4] = glob[1];
    cfg[5] = glob[2];
    cfg[6] = glob[3];
    cfg[7] = glob[4];
    cfg[5] = rate;
    cfg[6] = res;
    printf("calling set_mode (dev, cfg) with colour %d rate %d res %d aspect %d\n",
            cfg[4], cfg[5], cfg[6], cfg[7]);
    i = call2(set_mode, (u32) dev, (u32) cfg);
    printf("set_mode returned %d\n", i);
    return 0;
}
