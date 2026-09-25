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

static u32 uncached (const volatile void *p) {
    return ((u32) p & 0x1fffffffu) | 0xa0000000u;
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
    if (!(REG32 (MB_MASK) & bit)) {
        REG32 (MB_ENABLE) |= bit;
    }
    REG32 (MB_SLOT (n)) = uncached (msg);
    REG32 (MB_RING) |= bit;
    printf ("ipc: slot %d cmd %08x p %08x %08x %08x%s\n", n, cmd, p1, p2, p3, wait ? "" : " (no wait)");
    if (!wait) {
        return 0;
    }
    t0 = get_timer (0);
    while (REG32 (MB_BUSY) & bit) {
        if (get_timer (t0) > 1000) {
            printf ("ipc: TIMEOUT cmd %08x (busy %08x)\n", cmd, REG32 (MB_BUSY));
            return -2;
        }
    }
    return 0;
}

static void show (const char *tag) {
    printf ("%s: ES rd %06x wr %06x st %08x  PTS rd %04x wr %04x  mb 0c %08x 10 %08x 188 %08x "
            "c0 %08x cc %08x 184 %08x\n", tag, ES_REG (ES_RD), ES_REG (ES_WR), ES_REG (ES_STATUS),
            ES_REG (ES_PTS_RD), ES_REG (ES_PTS_WR), REG32 (0xbf12800c), REG32 (0xbf128010),
            REG32 (MB_BUSY), REG32 (0xbf1280c0), REG32 (0xbf1280cc), REG32 (0xbf128184));
}

static u32 arg_hex (int argc, char *argv[], int i, u32 def) {
    return argc > i ? parse_hex (argv[i]) : def;
}

int main (int argc, char *argv[]) {
    const unsigned char *src = (const unsigned char *) arg_hex (argc, argv, 1, 0x81600000);
    u32 len = arg_hex (argc, argv, 2, 0), pos = 0, sync = 3, i, t_show;
    unsigned char *ring = (unsigned char *) (0xa0000000u | ES_PHYS);

    for (i = 3; i < (u32) argc; i++) {
        if (argv[i][0] == 's' && argv[i][4] == '=') {       /* sync=N */
            sync = parse_hex (argv[i] + 5);
        }
    }
    if (!len) {
        printf ("usage: go ${a} <stream addr> <length> [sync=N]  (hex, like fatload)\n");
        return 1;
    }
    printf ("vdectest: stream %08x, %d bytes, starts %02x %02x %02x %02x %02x\n", (u32) src, len,
            src[0], src[1], src[2], src[3], src[4]);
    show ("before");

    /* ES / PTS rings (the stock firmware's layout, flags 7 as seen) */
    ES_REG (ES_START) = ES_PHYS | 7;
    ES_REG (ES_END) = ES_PHYS + ES_SIZE - 1;
    ES_REG (ES_PTS_START) = PTS_PHYS;
    ES_REG (ES_PTS_END) = PTS_PHYS + PTS_SIZE - 1;
    ES_REG (ES_WR) = 0;
    ES_REG (ES_PTS_WR) = 0;

    /* The stock player's sequence for an H.264 file */
    ipc_send (0x100413, 13, VDEC_HEAP, 0, 1);
    ipc_send (0x160413, 0, VDEC_HEAP, 0, 1);
    ipc_send (0x0d0413, sync, VDEC_HEAP, 0, 1);
    ipc_send (0x0f0413, 0, VDEC_HEAP, 0, 1);
    ipc_send (0x280413, 3, 0, 0, 0);
    ipc_send (0x010413, 1, 2, 0, 1);            /* start: format 1 = H.264 */
    ipc_send (0x310413, 0, 2, 0, 1);
    ipc_send (0x030413, 0, 2, 0, 1);            /* pause */

    /* First data into the ring before telling the decoder about it */
    {
        u32 n = len < ES_SIZE / 2 ? len : ES_SIZE / 2;

        memcpy (ring, src, n);
        pos = n;
        ES_REG (ES_WR) = n;
    }
    ipc_send (0x300413, 0xa0000000u | ES_PHYS, ES_SIZE, 0, 1);
    ipc_send (0x0d0413, sync == 3 ? 1 : sync, ES_SIZE, 0, 1);
    ipc_send (0x0f0413, 0, ES_SIZE, 0, 1);
    ipc_send (0x340413, 0, ES_SIZE, 0, 1);
    ipc_send (0x040413, 0, ES_SIZE, 0, 1);      /* resume */
    show ("started");

    /* Feed: keep the ring topped up; wr is an offset into the ring */
    t_show = get_timer (0);
    for (;;) {
        u32 rd = ES_REG (ES_RD) % ES_SIZE, wr = ES_REG (ES_WR) % ES_SIZE;
        u32 used = (wr + ES_SIZE - rd) % ES_SIZE, space = ES_SIZE - used - 4096;

        if (pos < len && space >= 65536) {
            u32 n = 65536, first;

            if (n > len - pos) {
                n = len - pos;
            }
            first = ES_SIZE - wr < n ? ES_SIZE - wr : n;
            memcpy (ring + wr, src + pos, first);
            memcpy (ring, src + pos + first, n - first);
            pos += n;
            ES_REG (ES_WR) = (wr + n) % ES_SIZE;
        }
        if (get_timer (t_show) >= 1000) {
            t_show = get_timer (0);
            printf ("fed %d / %d KB  ", pos / 1024, len / 1024);
            show ("run");
        }
        if (tstc ()) {
            getc ();
            break;
        }
    }
    ipc_send (0x020413, 1, ES_SIZE, 0, 1);      /* stop */
    show ("stopped");
    return 0;
}
