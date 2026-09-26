/*
 * regapply: apply the stock firmware's 1080p register values in groups,
 * to find what switches U-Boot's 1080i output to 1080p.
 *
 *   source avstart.scr first, then e.g.:
 *   go ${a} 1 2        apply groups 1 and 2
 *   go ${a} 1 2 3 4 5  apply everything
 *
 * Values come from snapdiff.txt (run1 = firmware 1080p, run2 = U-Boot 1080i).
 * Only video registers; a reboot undoes everything. Group 4 touches PLL /
 * clock registers: if the board freezes, power-cycle.
 */
#include "uboot.h"

struct regval {
    u32 addr, val;
};

/* 1: HDMI timing generator (status/counter words left out) */
static const struct regval g1[] = {
    { 0xbf470000, 0x00100600 }, { 0xbf470004, 0x111002d6 }, { 0xbf470008, 0x021c0002 },
    { 0xbf47000c, 0x00205205 }, { 0xbf470010, 0x002a02a0 }, { 0xbf470014, 0xa011002c },
    { 0xbf470024, 0x00010780 }, { 0xbf470028, 0x00050004 }, { 0xbf47002c, 0x04380024 },
    { 0xbf47004c, 0xd0000004 }, { 0xbf470060, 0x00000004 }, { 0xbf470068, 0x00000024 },
    { 0xbf47006c, 0x00000438 }, { 0xbf470070, 0x00000000 }, { 0xbf47007c, 0x80000000 },
    { 0, 0 }
};

/* 2: display mixer output size / timing */
static const struct regval g2[] = {
    { 0xbf440018, 0x04390001 }, { 0xbf4400b8, 0x04380780 }, { 0xbf440140, 0x800000c4 },
    { 0xbf440144, 0x04390439 }, { 0xbf4400f8, 0x02032703 }, { 0xbf440174, 0x40181888 },
    { 0, 0 }
};

/* 3: encoder / colour conversion block */
static const struct regval g3[] = {
    { 0xbf460000, 0x11000003 }, { 0xbf460004, 0x013b1000 }, { 0xbf460008, 0x0449212f },
    { 0xbf46000c, 0x0118012e }, { 0xbf460020, 0x014b00f5 }, { 0xbf460024, 0x006c012a },
    { 0xbf460038, 0x0008000f }, { 0xbf460040, 0x11fb85dc }, { 0xbf460060, 0x162b07ff },
    { 0xbf460064, 0x00010001 }, { 0xbf460068, 0x00f00129 }, { 0xbf46006c, 0x009f9595 },
    { 0xbf4600e4, 0x84808080 }, { 0xbf4600e8, 0x41fb85dc }, { 0xbf4600f0, 0x01e200f6 },
    { 0xbf4600f4, 0x01f601e5 },
    { 0, 0 }
};

/* 4: clock / PLL candidates */
static const struct regval g4[] = {
    { 0xbf5d0018, 0x0dc8e0c8 }, { 0xbf5d001c, 0x0dc8e0c8 }, { 0xbf5d0038, 0x00020407 },
    { 0xbf5d003c, 0x00020405 }, { 0xbf5d006c, 0x0a0a0a0a }, { 0xbf5d009c, 0x43000000 },
    { 0xbf5d00c4, 0x00009280 }, { 0xbf5d00c8, 0x349c426b }, { 0xbf5d00cc, 0x00036d49 },
    { 0xbf5d00d4, 0x00036d49 },
    { 0, 0 }
};

/* 5: HDMI misc + analog */
static const struct regval g5[] = {
    { 0xbf4700d8, 0x00000000 }, { 0xbf4700e0, 0xff000000 }, { 0xbf4700e4, 0x000b00e7 },
    { 0xbf4700e8, 0x00000020 }, { 0xbf4700ec, 0x01003f5d }, { 0xbf4700f0, 0x01004f35 },
    { 0xbf4700f4, 0x37000001 }, { 0xbf157004, 0x044af50c },
    { 0, 0 }
};

/* 8: video clock select + clock controller bits (fw values). bf5d005c low
 * byte is what U-Boot's "HD clk" code reads to name the video clock. */
static const struct regval g8[] = {
    { 0xbf5d005c, 0x0000b825 }, { 0xbf5d0058, 0x1c0186ea },
    { 0xbf50001c, 0x0011ff33 }, { 0xbf500030, 0x00000000 },
    { 0, 0 }
};

/* 9: group 5 without the bf157004 PLL write (display -> HDMI TX link only) */
static const struct regval g9[] = {
    { 0xbf4700d8, 0x00000000 }, { 0xbf4700e0, 0xff000000 }, { 0xbf4700e4, 0x000b00e7 },
    { 0xbf4700e8, 0x00000020 }, { 0xbf4700ec, 0x01003f5d }, { 0xbf4700f0, 0x01004f35 },
    { 0xbf4700f4, 0x37000001 },
    { 0, 0 }
};

/* 14: display mixer registers still different from the firmware after all
 * other groups (run4.txt diff). bf440144 kept reading back 021d021d (541
 * lines per field) even with interrupts off, so the mixer is still in
 * interlaced mode; one of these should be its interlace switch. */
static const struct regval g14[] = {
    { 0xbf4400dc, 0x00000000 }, { 0xbf4400e0, 0x00000000 }, { 0xbf440124, 0x00000000 },
    { 0xbf440164, 0x00000012 }, { 0xbf4401c4, 0x00000f00 }, { 0xbf441000, 0x00833b80 },
    { 0xbf441008, 0x11000000 }, { 0xbf44300c, 0x00000000 }, { 0xbf44305c, 0x00010108 },
    { 0, 0 }
};

static const struct regval *groups[] = { 0, g1, g2, g3, g4, g5, 0, 0, g8, g9, 0, 0, 0, 0, g14 };
static const char *names[] = {
    "", "HDMI timing", "mixer size", "encoder", "clock/PLL", "HDMI misc", "HDMI analog reset pulse",
    "HDMI PHY = firmware 1080p value", "video clock select", "HDMI link (group 5 minus PLL)", "HDMI TX clone (firmware)", "HDMI TX misc reset", "CPU interrupts off", "U-Boot set_mode 1080p60",
    "mixer interlace candidates"
};

/* 7: HDMI PHY control to the stock firmware's 1080p value (0x00177000),
 * set through the same bit-7 reset pulse U-Boot uses.
 * Seen on hardware: the monitor's refresh follows this PHY, not the display
 * clock regs (a pulse alone moved 1080i50 -> 1080i78, 43.8 kHz lines). */
static void hdmi_phy_fw(void) {
    u32 v = REG32(0xbf157000);

    REG32(0xbf157004) = 0x044af50c;
    REG32(0xbf157000) = 0x00177080;
    udelay(1000);
    REG32(0xbf157000) = 0x00177000;
    printf("  bf157004 = 044af50c, bf157000: %08x -> 00177080 -> 00177000\n", v);
}

/* 6: pulse bit 7 of 0xbf157000, like U-Boot's "hdmi analog reset"
 * (0x41071080 then 0x41071000), so the HDMI PHY relocks to a new clock */
static void hdmi_analog_reset(void) {
    u32 v = REG32(0xbf157000);

    REG32(0xbf157000) = v | 0x80;
    udelay(1000);
    REG32(0xbf157000) = v & ~0x80u;
    printf("  bf157000: %08x -> %08x -> %08x\n", v, v | 0x80, v & ~0x80u);
}

/* 10: HDMI TX byte registers cloned from the stock firmware's working
 * output (tx1.txt vs tx2.txt diff; status 0x10 and HDCP 0xa0-0xd7 left out).
 * Offsets from 0xbf480000 (bank 0) / 0xbf480100 (bank 1). */
struct txval {
    unsigned short reg;
    unsigned char val;
};

static const struct txval g10[] = {
    { 0x0103, 0xe6 },
    { 0x0104, 0x51 },
    { 0x0107, 0x10 },
    { 0x0113, 0xe6 },
    { 0x0114, 0x51 },
    { 0x0117, 0x10 },
    { 0x01c0, 0x3d },
    { 0x01cc, 0x02 },
    { 0x0007, 0x07 },
    { 0x0015, 0x98 },
    { 0x0016, 0x08 },
    { 0x0019, 0x58 },
    { 0x001f, 0x65 },
    { 0x0020, 0x04 },
    { 0x0023, 0x04 },
    { 0x0026, 0x98 },
    { 0x0027, 0x08 },
    { 0x002a, 0x58 },
    { 0x0030, 0x65 },
    { 0x0031, 0x04 },
    { 0x0034, 0x04 },
    { 0x0037, 0x00 },
    { 0x0043, 0xc0 },
    { 0x0047, 0x40 },
    { 0x004b, 0x04 },
    { 0x0080, 0x74 },
    { 0x0081, 0x00 },
    { 0x0082, 0x40 },
    { 0x0084, 0x01 },
    { 0xffff, 0 }
};

static void tx_clone(void) {
    const struct txval *t;

    for (t = g10; t->reg != 0xffff; t++) {
        volatile unsigned char *p = (volatile unsigned char *) (0xbf480000u + t->reg);
        unsigned char old = *p;
        *p = t->val;
        printf("  tx %03x: %02x -> %02x\n", t->reg, old, t->val);
    }
}

/* 11: TX "MISC_Reset" like U-Boot (0x801452f8): reg 0x08 = 2, then 0 */
static void tx_misc_reset(void) {
    volatile unsigned char *r8 = (volatile unsigned char *) 0xbf480008u;

    *r8 = 2;
    udelay(1000);
    *r8 = 0;
    printf("  tx 008: 02 -> 00 (misc reset)\n");
}

/* 12: disable CPU interrupts (CP0 Status.IE = 0) so U-Boot's display/HDMI
 * interrupt handlers stop reprogramming the display back to 1080i (seen:
 * bf440140/144 reverted after our writes). U-Boot's console and timer are
 * polled, so the prompt keeps working. */
static void irq_off(void) {
    u32 st;

    __asm__ volatile("mfc0 %0, $12" : "=r" (st));
    __asm__ volatile("mtc0 %0, $12\n\tehb" : : "r" (st & ~1u));
    printf("  CP0 Status %08x -> %08x (interrupts off)\n", st, st & ~1u);
}

/* 13: U-Boot HDMI driver set_mode (dev, cfg) with 1080p60 (res 7, rate 1),
 * same call as vicset.c, so no second program has to be loaded.
 * set_mode link 0x8014876c, GOT[-32736] at link 0x80188af0, runtime =
 * link + gd->reloc_off; checks the entry instruction before calling. */
#define LINK_SET_MODE   0x8014876cu
#define LINK_GOT_32736  0x80188af0u
#define SET_MODE_WORD0  0x3c1c0005u

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

static void set_mode_p60(void) {
    char *gd;
    u32 off, set_mode, page, *glob, dev[4], cfg[16];
    int i;

    __asm__ volatile("move %0, $26" : "=r" (gd));
    off = *(u32 *) (gd + 0x14);
    set_mode = LINK_SET_MODE + off;
    page = REG32(LINK_GOT_32736 + off);
    glob = (u32 *) (page + 21072);
    if (REG32(set_mode) != SET_MODE_WORD0 || glob[0] < 0x80000000u || glob[0] >= 0x88000000u) {
        printf("  sanity check failed, not calling set_mode\n");
        return;
    }
    for (i = 0; i < 16; i++) {
        cfg[i] = 0;
    }
    dev[0] = glob[0];
    cfg[4] = glob[1];
    cfg[5] = 1;         /* 60 Hz */
    cfg[6] = 7;         /* 1080p */
    cfg[7] = glob[4];
    printf("  set_mode (1080p60) returned %d, TX reg 0x10 = %02x\n",
            call2(set_mode, (u32) dev, (u32) cfg), REG8(0xbf480010));
}

/* Group numbers are decimal */
static int parse_dec(const char *s) {
    int v = 0;

    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
    }
    return v;
}

int main(int argc, char *argv[]) {
    int i;

    if (argc < 2) {
        printf("usage: go ${a} <group> [<group> ...]   groups 1..14:\n");
        for (i = 1; i <= 14; i++) {
            printf("  %d = %s\n", i, names[i]);
        }
        return 1;
    }
    for (i = 1; i < argc; i++) {
        int g = parse_dec(argv[i]);
        const struct regval *r;

        if (g < 1 || g > 14) {
            printf("skip unknown group '%s'\n", argv[i]);
            continue;
        }
        printf("group %d (%s):\n", g, names[g]);
        if (g == 6) {
            hdmi_analog_reset();
            continue;
        }
        if (g == 7) {
            hdmi_phy_fw();
            continue;
        }
        if (g == 10) {
            tx_clone();
            continue;
        }
        if (g == 11) {
            tx_misc_reset();
            continue;
        }
        if (g == 12) {
            irq_off();
            continue;
        }
        if (g == 13) {
            set_mode_p60();
            continue;
        }
        for (r = groups[g]; r->addr; r++) {
            u32 old = REG32(r->addr);
            REG32(r->addr) = r->val;
            printf("  %08x: %08x -> %08x\n", r->addr, old, r->val);
        }
    }
    printf("done. Output mode now: display %dx%d (bf4400b8=%08x)\n",
            REG32(0xbf4400b8) & 0xffff, REG32(0xbf4400b8) >> 16, REG32(0xbf4400b8));
    return 0;
}
