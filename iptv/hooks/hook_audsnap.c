/*
 * hook_audsnap: runs INSIDE the stock firmware on every remote key press
 * (hookpatch 'irkey' mode + hook_entry_irkey.S) and prints the audio
 * registers in audranges.h, the moving pointer registers every 10 ms, the
 * HDMI TX registers and the real PCM buffers. Press a key once while
 * silent and once while something with sound is playing, then diff.
 *
 * Runs in IR interrupt context, like the firmware's own "ir_value" printf.
 */
#include "audranges.h"

typedef unsigned int u32;
typedef int(*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))
#define FW_PRINTF   ((printf_t) 0x8018c184)
/* Firmware debug output switches (set together by 0x8018c288 on /
 * 0x8018c2a0 off; the app turns output off after boot):
 * printf prints only while this byte is non-zero, */
#define FW_PRINT_EN (*(volatile unsigned char *) 0x80772d64)
/* and the low-level output drops text while this word is 1 */
#define FW_UART_MUTE (*(volatile u32 *) 0x808b5280)

static u32 snap_count = 1;     /* in .data: hook .bss is never cleared */

/* CP0 Count runs at ~324 MHz (measured by cpuinfo) */
static void delay_ms(u32 ms) {
    u32 start, now;

    __asm__ volatile("mfc0 %0, $9" : "=r" (start));
    do {
        __asm__ volatile("mfc0 %0, $9" : "=r" (now));
    } while (now - start < ms * 324000u);
}

static void dump_words(printf_t pf, const char *tag, u32 start, u32 words) {
    u32 i;

    for (i = 0; i < words; i += 4) {
        u32 a = start + i * 4;
        pf("%s %08x: %08x %08x %08x %08x\n", tag, a,
            REG32(a), REG32(a + 4), REG32(a + 8), REG32(a + 12));
    }
}

void hook_main(u32 user, u32 key) {
    printf_t pf = FW_PRINTF;
    u32 r;
    unsigned char old_en = FW_PRINT_EN;
    u32 old_mute = FW_UART_MUTE;

    FW_PRINT_EN = 1;
    FW_UART_MUTE = 0;
    pf("\n=== AUD SNAP BEGIN #%d (firmware, key %02x) ===\n", snap_count, key);
    for (r = 0; r < AUD_NRANGES; r++) {
        dump_words(pf, "S", aud_ranges[r].start, aud_ranges[r].words);
    }

    /* Pointer rates: sample the moving registers every 10 ms, print after */
    {
        static const unsigned short regs[7] = {
            0x100, 0x104, 0x108, 0x10c, 0x110, 0x130, 0x140
        };
        u32 t[5], v[5][7], k, j;

        for (k = 0; k < 5; k++) {
            __asm__ volatile("mfc0 %0, $9" : "=r" (t[k]));
            for (j = 0; j < 7; j++) {
                v[k][j] = REG32(0xbf490000 + regs[j]);
            }
            delay_ms(10);
        }
        pf("=== T lines: us, 100 104 108 10c 110 130 140 ===\n");
        for (k = 0; k < 5; k++) {
            pf("T %6d: %08x %08x %08x %08x %08x %08x %08x\n",
                (t[k] - t[0]) / 324, v[k][0], v[k][1], v[k][2], v[k][3],
                v[k][4], v[k][5], v[k][6]);
        }
    }

    /* HDMI TX registers (MMIO bytes, bank 0 + bank 1) while playing */
    dump_words(pf, "X", 0xbf480000, 0x80);

    /* Buffer contents. Address/size registers are in 8-byte units (the AV
     * core shifts them left by 3), e.g. 0xf54000 -> phys 0x07aa0000.
     * Buffer n: base 0x18 + n*0x10, size 0x1c + n*0x10; read pointers
     * 0x100 (buf 0), 0x110 (buf 1), 0x124 (buf 2). Uncached view = RAM. */
    {
        static const unsigned short rd_reg[3] = { 0x100, 0x110, 0x124 };

        for (r = 0; r < 3; r++) {
            u32 base = (REG32(0xbf490018 + r * 0x10) & 0x3ffffff) << 3;
            u32 size = (REG32(0xbf49001c + r * 0x10) & 0xfffff) << 3;
            u32 rd = (REG32(0xbf490000 + rd_reg[r]) & 0x3ffffff) << 3;
            u32 n = 0, first = 0, a;

            for (a = 0; a < size; a += 4) {
                if (REG32(0xa0000000 | (base + a)) != 0) {
                    if (n == 0) {
                        first = a;
                    }
                    n++;
                }
            }
            pf("=== buf %d phys %08x size %x read %08x: %d non-zero words, first at +%x ===\n",
                r, base, size, rd, n, first);
            if (n) {
                dump_words(pf, "B", 0xa0000000 | (base + (first & ~31u)), 0x10);
                dump_words(pf, "R", 0xa0000000 | (rd & ~31u), 0x20);
            }
        }
    }

    pf("=== AUD SNAP END #%d ===\n", snap_count);
    snap_count++;
    FW_PRINT_EN = old_en;
    FW_UART_MUTE = old_mute;
}
