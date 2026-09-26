/*
 * hook_ipclog: logs every message the stock firmware sends to the AV core
 * (hookpatch 'ipc' mode + hook_entry_ipc.S on the send function
 * 0x801a7e4c). One "IPC" line per message:
 *   id (receiver: 0x10000413 video, 0x10000407 audio), cmd, p1..p3,
 *   timeout, wait-for-answer
 * plus 32 words behind every parameter that looks like a RAM pointer
 * ("P" lines), since some commands pass a parameter block.
 *
 * Build: ./buildhook.sh hook_ipclog.c hook_ipclog hook_entry_ipc.S
 */
typedef unsigned int u32;
typedef int(*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))
#define FW_PRINTF   ((printf_t) 0x8018c184)
#define FW_PRINT_EN (*(volatile unsigned char *) 0x80772d64)   /* see hook_audsnap.c */
#define FW_UART_MUTE (*(volatile u32 *) 0x808b5280)

static u32 seq = 1;                 /* in .data: hook .bss is never cleared */

/* Every message is also kept here, and the whole history is printed at
 * every video start command (0x10413): the boot-time messages come before
 * serial logging is usually running (and boot sends a start command too). */
#define HIST_MAX    64
static u32 hist[HIST_MAX][6] = { { 1 } };       /* initialised: stays in .data */

static u32 ram_ptr(u32 v) {
    if ((v & 3) || v < 0x80000000u || v >= 0xc0000000u || (v & 0x1fffffffu) >= 0x08000000u) {
        return 0;
    }
    return v;                       /* read as the firmware sees it (cached view) */
}

void hook_main(u32 id, const u32 *msg, u32 ack) {
    printf_t pf = FW_PRINTF;
    unsigned char old_en = FW_PRINT_EN;
    u32 old_mute = FW_UART_MUTE;
    u32 i, j;

    if (seq <= HIST_MAX) {
        u32 *h = hist[seq - 1];

        h[0] = id;
        h[1] = msg[0];
        h[2] = msg[1];
        h[3] = msg[2];
        h[4] = msg[3];
        h[5] = ack & 0xff;
    }
    FW_PRINT_EN = 1;
    FW_UART_MUTE = 0;
    if ((msg[0] & 0x7fffffffu) == 0x10413) {
        pf("=== IPC HISTORY #1..#%d ===\n", seq - 1);
        for (i = 0; i < seq - 1 && i < HIST_MAX; i++) {
            pf("H #%d id %08x cmd %08x p %08x %08x %08x ack %d\n", i + 1, hist[i][0], hist[i][1],
                hist[i][2], hist[i][3], hist[i][4], hist[i][5]);
        }
        /* What the boot-time pointer parameters point to now */
        for (i = 0; i < seq - 1 && i < 13; i++) {
            u32 k;

            for (k = 2; k <= 4; k++) {
                u32 p = ram_ptr(hist[i][k]);

                for (j = 0; p && j < 64; j += 4) {
                    pf("HP #%d p%d %08x: %08x %08x %08x %08x\n", i + 1, k - 1, p + j * 4,
                        REG32(p + j * 4), REG32(p + j * 4 + 4), REG32(p + j * 4 + 8),
                        REG32(p + j * 4 + 12));
                }
            }
        }
        pf("=== IPC HISTORY END ===\n");
    }
    /* ES block registers just before each video command: what the stock
     * player programs between commands (globals 0xbf260000.., video channel
     * 0xbf260100..0xbf26015c) */
    if (id == 0x10000413) {
        pf("R #%d g %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", seq,
            REG32(0xbf260000), REG32(0xbf260004), REG32(0xbf26000c), REG32(0xbf260014),
            REG32(0xbf260018), REG32(0xbf26001c), REG32(0xbf260020), REG32(0xbf260024),
            REG32(0xbf260028), REG32(0xbf26002c));
        for (i = 0; i < 0x60; i += 0x20) {
            u32 a = 0xbf260100 + i;

            pf("R #%d v+%02x %08x %08x %08x %08x %08x %08x %08x %08x\n", seq, i, REG32(a),
                REG32(a + 4), REG32(a + 8), REG32(a + 12), REG32(a + 16), REG32(a + 20),
                REG32(a + 24), REG32(a + 28));
        }
    }
    pf("IPC #%d id %08x cmd %08x p %08x %08x %08x t %d ack %d\n", seq, id, msg[0], msg[1],
        msg[2], msg[3], msg[4], ack & 0xff);
    for (i = 1; i <= 3; i++) {
        u32 p = ram_ptr(msg[i]);

        if (p) {
            for (j = 0; j < 32; j += 4) {
                pf("P #%d p%d %08x: %08x %08x %08x %08x\n", seq, i, p + j * 4, REG32(p + j * 4),
                    REG32(p + j * 4 + 4), REG32(p + j * 4 + 8), REG32(p + j * 4 + 12));
            }
        }
    }
    seq++;
    FW_UART_MUTE = old_mute;
    FW_PRINT_EN = old_en;
}
