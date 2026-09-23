/*
 * HDMI transmitter register dump (byte registers), shared by snaptx.c
 * (U-Boot) and hook_snaptx.c (inside the stock firmware).
 * U-Boot's TX helpers: bank 0 = 0xbf480000 + reg, bank 1 = 0xbf480100 + reg,
 * accessed with lbu/sb (0x8014132c / 0x80141344 / 0x80141360 / 0x8014137c).
 * Lines look like "T bf480000: 16 bytes" for diffing.
 */
#ifndef SNAPTX_H
#define SNAPTX_H

#define TX_BASE     0xbf480000u
#define TX_BYTES    0x200

typedef int (*txprint_t) (const char *fmt, ...);

static void snaptx_dump (txprint_t pf) {
    volatile unsigned char *p = (volatile unsigned char *) TX_BASE;
    unsigned int i, j;

    for (i = 0; i < TX_BYTES; i += 16) {
        unsigned char b[16];
        for (j = 0; j < 16; j++) {
            b[j] = p[i + j];
        }
        pf ("T %08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            TX_BASE + i, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
}

#endif
