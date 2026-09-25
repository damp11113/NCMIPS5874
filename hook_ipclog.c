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
typedef int (*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))
#define FW_PRINTF   ((printf_t) 0x8018c184)
#define FW_PRINT_EN (*(volatile unsigned char *) 0x80772d64)   /* see hook_audsnap.c */
#define FW_UART_MUTE (*(volatile u32 *) 0x808b5280)

static u32 seq = 1;                 /* in .data: hook .bss is never cleared */

static u32 ram_ptr (u32 v) {
    if ((v & 3) || v < 0x80000000u || v >= 0xc0000000u || (v & 0x1fffffffu) >= 0x08000000u) {
        return 0;
    }
    return v;                       /* read as the firmware sees it (cached view) */
}

void hook_main (u32 id, const u32 *msg, u32 ack) {
    printf_t pf = FW_PRINTF;
    unsigned char old_en = FW_PRINT_EN;
    u32 old_mute = FW_UART_MUTE;
    u32 i, j;

    FW_PRINT_EN = 1;
    FW_UART_MUTE = 0;
    pf ("IPC #%d id %08x cmd %08x p %08x %08x %08x t %d ack %d\n", seq, id, msg[0], msg[1],
        msg[2], msg[3], msg[4], ack & 0xff);
    for (i = 1; i <= 3; i++) {
        u32 p = ram_ptr (msg[i]);

        if (p) {
            for (j = 0; j < 32; j += 4) {
                pf ("P #%d p%d %08x: %08x %08x %08x %08x\n", seq, i, p + j * 4, REG32 (p + j * 4),
                    REG32 (p + j * 4 + 4), REG32 (p + j * 4 + 8), REG32 (p + j * 4 + 12));
            }
        }
    }
    seq++;
    FW_UART_MUTE = old_mute;
    FW_PRINT_EN = old_en;
}
