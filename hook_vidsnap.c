/*
 * hook_vidsnap: runs INSIDE the stock firmware on every remote key press
 * (hookpatch 'irkey' mode + hook_entry_irkey.S) and prints what the video
 * decoder interface looks like while a video plays:
 *   S lines  decoder block 0xbf260000 / 0xbf261000, IPC mailbox block
 *            0xbf128000 (message pointers at +0x14 + 4n, pending +0x188)
 *   M lines  the message structs the mailbox words point to
 *   C lines  decoder block words that change within 30 ms (4 samples,
 *            10 ms apart): the moving ES ring pointers
 * Press INFO (or any key) a few times while test.mp4 plays.
 *
 * Build: ./buildhook.sh hook_vidsnap.c hook_vidsnap hook_entry_irkey.S
 */
typedef unsigned int u32;
typedef int (*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))
#define FW_PRINTF   ((printf_t) 0x8018c184)
#define FW_PRINT_EN (*(volatile unsigned char *) 0x80772d64)   /* see hook_audsnap.c */
#define FW_UART_MUTE (*(volatile u32 *) 0x808b5280)

#define DEC_BASE    0xbf260000u
#define DEC_WORDS   256
#define MBOX_BASE   0xbf128000u
#define SAMPLES     4

static u32 snap_count = 1;          /* in .data: hook .bss is never cleared */
static u32 samp[SAMPLES][DEC_WORDS];

static void delay_ms (u32 ms) {
    u32 start, now;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (start));
    do {
        __asm__ volatile ("mfc0 %0, $9" : "=r" (now));
    } while (now - start < ms * 324000u);
}

static void dump_words (printf_t pf, const char *tag, u32 start, u32 words) {
    u32 i;

    for (i = 0; i < words; i += 4) {
        u32 a = start + i * 4;
        pf ("%s %08x: %08x %08x %08x %08x\n", tag, a,
            REG32 (a), REG32 (a + 4), REG32 (a + 8), REG32 (a + 12));
    }
}

/* A RAM pointer we can read without faulting: kseg0/kseg1, below 128 MB */
static u32 ram_ptr (u32 v) {
    if ((v & 3) || v < 0x80000000u || v >= 0xc0000000u || (v & 0x1fffffffu) >= 0x08000000u) {
        return 0;
    }
    return 0xa0000000u | (v & 0x1fffffffu);        /* read uncached: shared with the AV core */
}

void hook_main (u32 user, u32 key) {
    printf_t pf = FW_PRINTF;
    unsigned char old_en = FW_PRINT_EN;
    u32 old_mute = FW_UART_MUTE;
    u32 k, i;

    (void) user;
    FW_PRINT_EN = 1;
    FW_UART_MUTE = 0;
    pf ("\n=== VID SNAP BEGIN #%d (key %02x) ===\n", snap_count, key);

    /* Moving words first, before the printing takes time */
    for (k = 0; k < SAMPLES; k++) {
        for (i = 0; i < DEC_WORDS; i++) {
            samp[k][i] = REG32 (DEC_BASE + i * 4);
        }
        delay_ms (10);
    }
    for (i = 0; i < DEC_WORDS; i++) {
        for (k = 1; k < SAMPLES && samp[k][i] == samp[0][i]; k++) {
        }
        if (k < SAMPLES) {
            pf ("C %08x: %08x %08x %08x %08x\n", DEC_BASE + i * 4,
                samp[0][i], samp[1][i], samp[2][i], samp[3][i]);
        }
    }

    dump_words (pf, "S", DEC_BASE, 0x100);
    dump_words (pf, "S", DEC_BASE + 0x1000, 0x80);
    dump_words (pf, "S", MBOX_BASE, 0x80);

    /* Video PTS / ES-descriptor ring (0xbf260118 start, +0x20 end, +0x30
     * wr offset): the 32 words before the write offset, and the ES ring
     * bytes at the read offset (+0x1c start, +0x2c rd offset). */
    {
        u32 ps = REG32 (DEC_BASE + 0x118) & 0x1fffffffu, pe = REG32 (DEC_BASE + 0x120) & 0x1fffffffu;
        u32 pw = REG32 (DEC_BASE + 0x130), es = REG32 (DEC_BASE + 0x11c) & 0x1ffffff8u;
        u32 er = REG32 (DEC_BASE + 0x12c), from;

        if (ps && pe > ps && ps < 0x08000000u) {
            u32 size = pe + 1 - ps;

            from = (pw + size - 0x80) % size;
            pf ("D pts ring %08x-%08x wr %x, 32 words from offset %x:\n", ps, pe, pw, from);
            for (i = 0; i < 32; i += 4) {
                u32 a = 0xa0000000u | (ps + (from + i * 4) % size);

                pf ("D %05x: %08x %08x %08x %08x\n", (from + i * 4) % size, REG32 (a),
                    REG32 (a + 4), REG32 (a + 8), REG32 (a + 12));
            }
        }
        if (es && es < 0x08000000u) {
            u32 a = 0xa0000000u | ((es + er) & ~15u);

            pf ("E es ring %08x rd %x: %08x %08x %08x %08x %08x %08x %08x %08x\n", es, er,
                REG32 (a), REG32 (a + 4), REG32 (a + 8), REG32 (a + 12), REG32 (a + 16),
                REG32 (a + 20), REG32 (a + 24), REG32 (a + 28));
        }
    }

    /* Message structs behind the mailbox words */
    for (i = 0; i < 32; i++) {
        u32 v = REG32 (MBOX_BASE + 0x14 + i * 4), p = ram_ptr (v);

        if (p) {
            pf ("M slot %d -> %08x\n", i, v);
            dump_words (pf, "M", p, 16);
        }
    }
    pf ("=== VID SNAP END #%d ===\n", snap_count);
    snap_count++;
    FW_UART_MUTE = old_mute;
    FW_PRINT_EN = old_en;
}
