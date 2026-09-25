/*
 * vdectest: first try at the AV core's hardware video decoder without the
 * stock firmware. Sends the stock player's start sequence (captured with
 * hook_ipclog, see continue.md) and feeds a raw Annex-B H.264 stream from
 * RAM into the ES ring, printing the ring pointers and IPC state.
 *
 * IPC send (stock fn 0x801a7e4c): 24-byte message {cmd | 0x80000000 when an
 * answer is wanted, p1, p2, p3, timeout ms, receiver id}; free slot n =
 * first 0 bit of 0xbf128188; 0xbf128010 |= bit if not in 0xbf12818c;
 * 0xbf128014 + 4n = message pointer (uncached); 0xbf12800c |= bit; the AV
 * core clears the bit in 0xbf128188 when done.
 *
 * Needs: AV core started with the normal 50 MB video memory (vdectest.scr),
 * the stream loaded to RAM. In U-Boot:
 *   source vdectest.scr   (AV init, printf on, loads test.h264 and this)
 *   go ${a} 81600000 ${filesize} [sync=N]
 * Any serial key stops.
 */
#include "uboot.h"

void *memcpy (void *dst, const void *src, unsigned int n);
void *memset (void *dst, int c, unsigned int n);

/* IPC */
#define MB_BUSY         0xbf128188
#define MB_MASK         0xbf12818c
#define MB_ENABLE       0xbf128010
#define MB_SLOT(n)      (0xbf128014 + 4 * (n))
#define MB_RING         0xbf12800c
#define ID_VDEC         0x10000413u
#define ACK             0x80000000u

/* Video ES / PTS ring registers (0xbf260100 block) */
#define ES_REG(off)     (*(volatile u32 *) (0xbf260100 + (off)))
#define ES_PTS_START    0x18
#define ES_START        0x1c
#define ES_PTS_END      0x20
#define ES_END          0x24
#define ES_PTS_RD       0x28
#define ES_RD           0x2c
#define ES_PTS_WR       0x30
#define ES_WR           0x34
#define ES_STATUS       0x38

/* Memory, as the stock firmware used it (phys) */
#define VDEC_HEAP       0x044d8000u     /* + 0x80000 = DPB, ~48 MB */
#define ES_PHYS         0x07680000u
#define ES_SIZE         0x00400000u
#define PTS_PHYS        0x03f00000u     /* 64 KB, free while no OSD runs */
#define PTS_SIZE        0x00010000u

static volatile u32 msg[8] __attribute__ ((aligned (32)));   /* used uncached */

/*
 * Buffers for the stock firmware's boot-time setup messages (avdump7/8):
 * zero-filled areas the AV core uses (state, user data, header info, a
 * 1 KB area it marks with 0xbeafdead) and a 0x1b00-byte coefficient
 * table. The stock firmware builds that table at run time (0x806ad08c; the
 * flash image has 0xff there), so it comes from a RAM dump of the running
 * stock firmware: mkvdectbl.py app_ram.bin -> VDECTBL.BIN on the stick,
 * loaded to 0x83e00000 by vdectest.scr (tbl=83e00000). Not shipped here.
 */
#define TBL_SIZE        0x1b00
static unsigned char vbuf_state[0x400] __attribute__ ((aligned (64)));  /* 0x110413 */
static unsigned char vbuf_a[0x100] __attribute__ ((aligned (64)));      /* 0x2d0413 p1 */
static unsigned char vbuf_usrdat[0x1100] __attribute__ ((aligned (64)));/* 0x2d0413 p2 */
static unsigned char vbuf_hdr[0x100] __attribute__ ((aligned (64)));    /* 0x360413 p1 */
static unsigned char vbuf_b[0x200] __attribute__ ((aligned (64)));      /* 0x360413 p2 */
static unsigned char vbuf_log[0x400] __attribute__ ((aligned (64)));    /* 0x220413 */
static unsigned char vbuf_tbl[TBL_SIZE] __attribute__ ((aligned (64))); /* 0x2e0413 */

/* Milliseconds from CP0 Count (324000 ticks per ms): U-Boot's get_timer
 * may need interrupts, which are off during the test. Call at least every
 * 13 s (Count wraps). */
static u32 ms_now (void) {
    static u32 last, ticks, ms;
    u32 c;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
    ticks += c - last;
    last = c;
    ms += ticks / 324000u;
    ticks %= 324000u;
    return ms;
}

static u32 uncached (const volatile void *p) {
    return ((u32) p & 0x1fffffffu) | 0xa0000000u;
}

/*
 * Messages from the AV core (stock handler 0x801a8638): pending bits in
 * 0xbf128188, message pointer in slot 0xbf128014 + 4n, 24 bytes like ours
 * (+0x14 = 0xdead for a special kind). The stock firmware acknowledges with
 * 0xbf1280c4 |= bit; 0xbf1280c8 |= bit. U-Boot has no handler for this
 * interrupt, so the test runs with interrupts off and polls.
 */
static u32 in_count;

static void ipc_poll (u32 ignore) {
    u32 pend = REG32 (MB_BUSY) & ~ignore, n;

    for (n = 0; n < 32; n++) {
        u32 bit = 1u << n, p;

        if (!(pend & bit)) {
            continue;
        }
        p = REG32 (MB_SLOT (n));
        if (in_count < 200) {
            if (p >= 0x80000000u && p < 0xc0000000u && (p & 0x1fffffffu) < 0x08000000u) {
                volatile u32 *m = (volatile u32 *) uncached ((void *) p);

                printf ("  <- av slot %d @%08x: %08x %08x %08x %08x %08x %08x\n", n, p, m[0], m[1],
                        m[2], m[3], m[4], m[5]);
            } else {
                printf ("  <- av slot %d, pointer %08x\n", n, p);
            }
        }
        in_count++;
        REG32 (0xbf1280c4) |= bit;
        REG32 (0xbf1280c8) |= bit;
    }
}

static int ipc_send (u32 cmd, u32 p1, u32 p2, u32 p3, int wait) {
    volatile u32 *m = (volatile u32 *) uncached (msg);
    u32 busy, bit = 0, n, t0;

    busy = REG32 (MB_BUSY);
    for (n = 0; n < 32; n++) {
        if (!(busy & (1u << n))) {
            bit = 1u << n;
            break;
        }
    }
    if (!bit) {
        printf ("ipc: no free slot (busy %08x)\n", busy);
        return -1;
    }
    m[0] = cmd | (wait ? ACK : 0);
    m[1] = p1;
    m[2] = p2;
    m[3] = p3;
    m[4] = 1000;
    m[5] = ID_VDEC;
    printf ("ipc: -> slot %d cmd %08x p %08x %08x %08x%s\n", n, cmd, p1, p2, p3,
            wait ? "" : " (no wait)");
    if (!(REG32 (MB_MASK) & bit)) {
        REG32 (MB_ENABLE) |= bit;
    }
    REG32 (MB_SLOT (n)) = uncached (msg);
    REG32 (MB_RING) |= bit;
    if (!wait) {
        return 0;
    }
    t0 = ms_now ();
    while (REG32 (MB_BUSY) & bit) {
        ipc_poll (bit);
        if ((ms_now () - t0) > 1000) {
            printf ("ipc: TIMEOUT cmd %08x (busy %08x)\n", cmd, REG32 (MB_BUSY));
            return -2;
        }
    }
    printf ("ipc:    done in %d ms, busy %08x\n", (int) (ms_now () - t0), REG32 (MB_BUSY));
    ipc_poll (0);
    return 0;
}

/* Zero a buffer through its uncached view and drop its cache lines, so no
 * dirty line can later overwrite what the AV core writes there. */
static u32 shared_buf (void *p, u32 size) {
    u32 a;

    memset ((void *) uncached (p), 0, size);
    for (a = (u32) p & ~31u; a < (u32) p + size; a += 32) {
        __asm__ volatile ("cache 0x11, 0(%0)" : : "r" (a) : "memory");  /* Hit_Invalidate_D */
    }
    __asm__ volatile ("sync" : : : "memory");
    return uncached (p);
}

/* The stock firmware's boot-time video setup (avdump7.log #2-#10) */
static void boot_setup (u32 tbl) {
    u32 t = shared_buf (vbuf_tbl, TBL_SIZE);

    memcpy ((void *) t, (const void *) tbl, TBL_SIZE);
    printf ("table from %08x: %08x %08x %08x %08x\n", tbl, ((u32 *) t)[0], ((u32 *) t)[1],
            ((u32 *) t)[2], ((u32 *) t)[3]);
    ipc_send (0x110413, shared_buf (vbuf_state, sizeof (vbuf_state)), 2, 0, 1);
    ipc_send (0x2d0413, shared_buf (vbuf_a, sizeof (vbuf_a)),
              shared_buf (vbuf_usrdat, sizeof (vbuf_usrdat)), 0, 1);
    ipc_send (0x360413, shared_buf (vbuf_hdr, sizeof (vbuf_hdr)),
              shared_buf (vbuf_b, sizeof (vbuf_b)) & ~0x20000000u, 0, 1);   /* stock: cached view */
    ipc_send (0x0a0413, 1, 0, 0, 1);
    ipc_send (0x020413, 1, 0, 0, 1);
    ipc_send (0x220413, shared_buf (vbuf_log, sizeof (vbuf_log)), 0x400, 0, 1);
    ipc_send (0x2f0413, TBL_SIZE, 0x400, 0, 1);
    ipc_send (0x2e0413, t, t, 0, 1);
    ipc_send (0x100413, 13, VDEC_HEAP, 0, 1);
    /* The stock boot starts the decoder once (format 4, its boot video) and
     * stops it: 'video state' 2 afterwards, and only a start from that
     * state sets up the ES ring / DPB (vdectest run 1 and 2: state 0). */
    ipc_send (0x010413, 4, 2, 0, 1);
    ipc_send (0x020413, 1, 2, 0, 1);
}

/*
 * Feeding. The stock player writes whole frames (access units) into the ES
 * ring and one 32-byte descriptor per frame into the PTS ring (avdump9.log):
 *   w0 1, w1 ES phys address of the frame (its 4-byte start code), w2 0,
 *   w3 PTS | 0x80000000 (90 kHz) or 0, w4 0, w5 ES phys address + 1 (the
 *   3-byte start code; first stock frames: + 0x1e), w6 slice NAL header <<
 *   8 | 1 (0x6501 IDR, 0x4101 P, 0x0101 B; 1 for the first frames), w7
 *   frame counter
 * then advances the ES wr offset (+0x34) and the PTS wr offset (+0x30).
 * PTS left 0 for now.
 */
static u32 au_count;
static u32 desc_slice;              /* desc=1: w5/w6 at the first slice (run 6) */
static u32 hw_desc;                 /* hw=1: no descriptors, see feed_au */
static unsigned char au_buf[1024 * 1024];      /* one access unit, 4-byte start codes */

static u32 es_used (void) {
    return (ES_REG (ES_WR) + ES_SIZE - ES_REG (ES_RD) % ES_SIZE) % ES_SIZE;
}

static u32 pts_used (void) {
    return (ES_REG (ES_PTS_WR) + PTS_SIZE - ES_REG (ES_PTS_RD) % PTS_SIZE) % PTS_SIZE;
}

/* Next start code at or after p: returns its position (4-byte codes
 * include their leading 0), *sc = its length; len if none */
static u32 next_sc (const unsigned char *s, u32 len, u32 p, u32 *sc) {
    for (; p + 3 <= len; p++) {
        if (s[p] == 0 && s[p + 1] == 0 && s[p + 2] == 1) {
            if (p > 0 && s[p - 1] == 0) {
                *sc = 4;
                return p - 1;
            }
            *sc = 3;
            return p;
        }
    }
    *sc = 0;
    return len;
}

/* One access unit from pos (at a start code): a new one begins at an
 * AUD/SEI/SPS/PPS or at a slice with first_mb_in_slice 0, once the current
 * one has a slice. Copies it into the ring, writes its descriptor.
 * Returns the position after it. */
static u32 feed_au (const unsigned char *s, u32 len, u32 pos) {
    volatile u32 *d;
    unsigned char *ring = (unsigned char *) (0xa0000000u | ES_PHYS);
    u32 q = pos, sc, end = len, hdr = 0, seen_vcl = 0;
    u32 wr = ES_REG (ES_WR) % ES_SIZE, n, first, pw;

    q = next_sc (s, len, q, &sc);
    while (q < len) {
        u32 h = s[q + sc], type = h & 0x1f, nq, nsc;

        if (seen_vcl && ((type >= 6 && type <= 9) ||
                         (type >= 1 && type <= 5 && q + sc + 1 < len && (s[q + sc + 1] & 0x80)))) {
            end = q;
            break;
        }
        if (type >= 1 && type <= 5 && !seen_vcl) {
            seen_vcl = 1;
            hdr = h;
        }
        nq = next_sc (s, len, q + sc + 1, &nsc);
        q = nq;
        sc = nsc;
    }
    /* Every NAL with a 4-byte start code: our IDR slices (after SPS/PPS)
     * have 3-byte codes, and run 10 lost most macroblocks (broken refs) */
    {
        u32 p = pos, o = 0, psc, nsc2;

        p = next_sc (s, end, p, &psc);
        while (p < end && o + 4 < sizeof (au_buf)) {
            u32 body = p + psc, nxt = next_sc (s, end, body + 1, &nsc2), k = nxt - body;

            if (o + 4 + k > sizeof (au_buf)) {
                k = sizeof (au_buf) - o - 4;
            }
            au_buf[o] = 0;
            au_buf[o + 1] = 0;
            au_buf[o + 2] = 0;
            au_buf[o + 3] = 1;
            memcpy (au_buf + o + 4, s + body, k);
            o += 4 + k;
            p = nxt;
            psc = nsc2;
        }
        n = o;
    }
    first = ES_SIZE - wr < n ? ES_SIZE - wr : n;
    memcpy (ring + wr, au_buf, first);
    memcpy (ring, au_buf + first, n - first);

    if (hw_desc) {
        /* hw=1: data + ES wr only; does a hardware parser write the
         * descriptors (+0x30) and move +0x2c / +0x40..+0x5c itself? */
        au_count++;
        __asm__ volatile ("sync" : : : "memory");
        ES_REG (ES_WR) = (wr + n) % ES_SIZE;
        return end;
    }
    pw = ES_REG (ES_PTS_WR) % PTS_SIZE;
    d = (volatile u32 *) (0xa0000000u | (PTS_PHYS + pw));
    d[0] = 1;
    d[1] = ES_PHYS + wr;
    d[2] = 0;
    d[3] = 0;
    d[4] = 0;
    if (desc_slice) {
        /* desc=1 (run 6): w5 at the first slice's 3-byte start code */
        u32 o, t = 0;

        for (o = 0; o + 5 < n; o++) {
            if (au_buf[o] == 0 && au_buf[o + 1] == 0 && au_buf[o + 2] == 1 &&
                (au_buf[o + 3] & 0x1f) >= 1 && (au_buf[o + 3] & 0x1f) <= 5) {
                t = o;
                break;
            }
        }
        d[5] = ES_PHYS + (wr + t) % ES_SIZE;
        d[6] = (hdr << 8) | 1;
    } else {
        /* w5 = the frame's own 3-byte start code (stock: w1 + 1), w6 = its
         * first NAL's header if that is a slice, else 1 */
        u32 h0 = au_buf[4];

        d[5] = ES_PHYS + (wr + 1) % ES_SIZE;
        d[6] = ((h0 & 0x1f) >= 1 && (h0 & 0x1f) <= 5) ? (h0 << 8) | 1 : 1;
    }
    d[7] = au_count++;
    __asm__ volatile ("sync" : : : "memory");
    ES_REG (ES_WR) = (wr + n) % ES_SIZE;
    ES_REG (ES_PTS_WR) = (pw + 32) % PTS_SIZE;
    return end;
}

static void show (const char *tag) {
    printf ("%s: ES rd %06x wr %06x st %08x  PTS rd %04x wr %04x  mb 0c %08x 10 %08x 188 %08x "
            "c0 %08x cc %08x 184 %08x\n", tag, ES_REG (ES_RD), ES_REG (ES_WR), ES_REG (ES_STATUS),
            ES_REG (ES_PTS_RD), ES_REG (ES_PTS_WR), REG32 (0xbf12800c), REG32 (0xbf128010),
            REG32 (MB_BUSY), REG32 (0xbf1280c0), REG32 (0xbf1280cc), REG32 (0xbf128184));
}

/* The whole video block (+0x00..+0x5c) and its mirror at +0x80, plus the
 * block's global registers 0xbf260000.. (stock: 0xbf260000 = 3) */
static void show_block (const char *tag) {
    u32 o;

    printf ("%s global: %08x %08x %08x %08x %08x %08x  +80: %08x\n", tag, REG32 (0xbf260000),
            REG32 (0xbf260004), REG32 (0xbf26000c), REG32 (0xbf260014), REG32 (0xbf260018),
            REG32 (0xbf26001c), REG32 (0xbf260080));

    for (o = 0; o < 0x60; o += 16) {
        printf ("%s +%02x: %08x %08x %08x %08x   +%02x: %08x %08x %08x %08x\n", tag, o,
                ES_REG (o), ES_REG (o + 4), ES_REG (o + 8), ES_REG (o + 12), o + 0x80,
                ES_REG (o + 0x80), ES_REG (o + 0x84), ES_REG (o + 0x88), ES_REG (o + 0x8c));
    }
}

static u32 arg_hex (int argc, char *argv[], int i, u32 def) {
    return argc > i ? parse_hex (argv[i]) : def;
}

int main (int argc, char *argv[]) {
    const unsigned char *src = (const unsigned char *) arg_hex (argc, argv, 1, 0x81600000);
    u32 len = arg_hex (argc, argv, 2, 0), pos = 0, sync = 3, tbl = 0, r38 = 0, have_r38 = 0, g0 = 3, prefill = 0, no5x = 0, i, t_show, status;

    for (i = 3; i < (u32) argc; i++) {
        if (argv[i][0] == 's' && argv[i][4] == '=') {       /* sync=N */
            sync = parse_hex (argv[i] + 5);
        } else if (argv[i][0] == 'n' && argv[i][4] == '=') {    /* no5x=1: skip +0x50/+0x5c */
            no5x = parse_hex (argv[i] + 5);
        } else if (argv[i][0] == 'h' && argv[i][2] == '=') {    /* hw=1: no descriptors */
            hw_desc = parse_hex (argv[i] + 3);
        } else if (argv[i][0] == 'p' && argv[i][3] == '=') {    /* pre=1: prefill */
            prefill = parse_hex (argv[i] + 4);
        } else if (argv[i][0] == 'g' && argv[i][1] == '=') {    /* g=<hex>: 0xbf260000 */
            g0 = parse_hex (argv[i] + 2);
        } else if (argv[i][0] == 'r' && argv[i][3] == '=') {    /* r38=<hex>: +0x38 control */
            r38 = parse_hex (argv[i] + 4);
            have_r38 = 1;
        } else if (argv[i][0] == 'd' && argv[i][4] == '=') {    /* desc=1: old w5/w6 */
            desc_slice = parse_hex (argv[i] + 5);
        } else if (argv[i][0] == 't' && argv[i][3] == '=') {    /* tbl=<addr>: boot setup */
            tbl = parse_hex (argv[i] + 4);
        }
    }
    if (!len) {
        printf ("usage: go ${a} <stream addr> <length> [sync=N] [tbl=<addr>]  (hex)\n");
        return 1;
    }
    __asm__ volatile ("mfc0 %0, $12" : "=r" (status));
    __asm__ volatile ("mtc0 %0, $12\n\tehb" : : "r" (status & ~1u));      /* IE off, see ipc_poll */
    printf ("vdectest: stream %08x, %d bytes, starts %02x %02x %02x %02x %02x\n", (u32) src, len,
            src[0], src[1], src[2], src[3], src[4]);
    show ("before");

    /* Video block config as the stock firmware leaves it (avdump.log).
     * +0x00 bits 12-14 = 7 switches the decoder to frame descriptors ('es
     * desc 1', AV core init_vdec_param 0x87e418f0); without them it misreads
     * slices (invalid PPS). The rest looks like demux setup (0xe0 = MPEG
     * video stream id), copied as is. */
    /* Boot-time values (avdump13 #7-#12); the AV core sets the upper bits of
     * +0x00 itself (0x000072d1 -> 0x5fa072d1 during the boot start) */
    ES_REG (0x00) = 0x000072d1;
    ES_REG (0x04) = 0x00000006;
    ES_REG (0x08) = 0x00000101;
    ES_REG (0x0c) = 0x00000000;
    ES_REG (0x10) = 0x00000000;
    ES_REG (0x14) = 0xffffffff;
    printf ("video block +0: %08x (es desc bits %d)\n", ES_REG (0x00), (ES_REG (0x00) >> 12) & 7);
    /* Global register of the ES block: stock 3 while playing (bit 0 video
     * channel +0x100, bit 1 audio +0x200?); 0 in our runs 1-7, where the
     * hardware reader (+0x40..+0x5c) never ran and ES rd stayed 0 */
    show_block ("reset");
    REG32 (0xbf260000) = g0;
    printf ("0xbf260000 <- %08x, reads %08x\n", g0, REG32 (0xbf260000));
    if (have_r38) {
        /* +0x38: stock 0x3x00200c while playing, ours 0x8000000c */
        ES_REG (ES_STATUS) = r38;
        printf ("+0x38 <- %08x, reads %08x\n", r38, ES_REG (ES_STATUS));
    }
    show_block ("before");

    /* ES / PTS rings (the stock firmware's layout, flags 7 as seen) */
    ES_REG (ES_START) = ES_PHYS | 7;
    ES_REG (ES_END) = ES_PHYS + ES_SIZE - 1;
    ES_REG (ES_PTS_START) = PTS_PHYS;
    ES_REG (ES_PTS_END) = PTS_PHYS + PTS_SIZE - 1;
    ES_REG (ES_WR) = 0;
    ES_REG (ES_PTS_WR) = 0;

    if (tbl) {
        boot_setup (tbl);
    } else {
        printf ("no tbl=: boot-time setup skipped (decoder stays in state 0)\n");
    }

    /* The stock player's sequence for an H.264 file */
    ipc_send (0x100413, 13, VDEC_HEAP, 0, 1);
    ipc_send (0x160413, 0, VDEC_HEAP, 0, 1);
    ipc_send (0x0d0413, sync, VDEC_HEAP, 0, 1);
    ipc_send (0x0f0413, 0, VDEC_HEAP, 0, 1);
    ipc_send (0x280413, 3, 0, 0, 0);

    /* Play-time values, written by the stock player right before its start
     * (avdump13 #21 -> #22), rings emptied; +0x00 keeps what the AV core
     * made of it, only bits 12-14 (es desc) must be 7 */
    ES_REG (0x00) |= 0x7000;
    ES_REG (0x04) = 0x00000007;
    ES_REG (0x08) = 0x05010101;
    ES_REG (0x0c) = 0xe0e00000;
    ES_REG (0x10) = 0x00008080;
    ES_REG (0x14) = 0xffff7f7f;
    ES_REG (ES_WR) = 0;
    ES_REG (ES_PTS_WR) = 0;
    /* The stock channel init (main fw 0x8023d180..) also writes +0x38..+0x58;
     * after it (avdump13 #22) +0x50 = 0070b1bd and +0x5c = 30c1 (stock sets
     * +0x5c at boot, #7), ours stayed 00300000 / 00000101 */
    if (!no5x) {
        ES_REG (0x50) = 0x0070b1bd;
        ES_REG (0x5c) = 0x000030c1;
    }
    REG32 (0xbf260000) = g0;
    show_block ("play cfg");

    /* pre=1: data in the ring before the start (runs 4-9). The stock player
     * starts with an EMPTY ring and feeds only after the resume (avdump13:
     * all pointers 0 from the start to the resume); the full start setup
     * (state 2) came from the boot start/stop cycle working once the table
     * was right, not from prefilling. With data present the decoder may parse
     * before 0x300413 has told it where the ring is. */
    if (prefill) {
        while (pos < len && es_used () < ES_SIZE / 2) {
            pos = feed_au (src, len, pos);
        }
        printf ("prefilled %d frames, %d KB\n", au_count, es_used () / 1024);
    }
    ipc_send (0x010413, 1, 2, 0, 1);            /* start: format 1 = H.264 */
    ipc_send (0x310413, 0, 2, 0, 1);
    ipc_send (0x030413, 0, 2, 0, 1);            /* pause */

    ipc_send (0x300413, 0xa0000000u | ES_PHYS, ES_SIZE, 0, 1);
    ipc_send (0x0d0413, sync == 3 ? 1 : sync, ES_SIZE, 0, 1);
    ipc_send (0x0f0413, 0, ES_SIZE, 0, 1);
    ipc_send (0x340413, 0, ES_SIZE, 0, 1);
    ipc_send (0x040413, 0, ES_SIZE, 0, 1);      /* resume */
    show ("started");
    show_block ("started");

    /* Feed whole frames while there is room in both rings */
    t_show = ms_now ();
    for (;;) {
        if (pos < len && es_used () < ES_SIZE - 512 * 1024 && pts_used () < PTS_SIZE - 64 * 32) {
            pos = feed_au (src, len, pos);
        }
        if ((ms_now () - t_show) >= 1000) {
            t_show = ms_now ();
            printf ("fed %d frames, %d / %d KB  ", au_count, pos / 1024, len / 1024);
            show ("run");
            show_block ("run");
        }
        ipc_poll (0);
        if (tstc ()) {
            getc ();
            break;
        }
    }
    ipc_send (0x020413, 1, ES_SIZE, 0, 1);      /* stop */
    show ("stopped");
    printf ("%d messages from the AV core\n", in_count);
    __asm__ volatile ("mtc0 %0, $12\n\tehb" : : "r" (status));
    return 0;
}
