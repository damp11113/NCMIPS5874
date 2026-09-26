/*
 * FD650 / AiP650 / TM1650 front panel (satellite box) on SoC I2C channel 2.
 *
 * The chip has no real I2C address: the first byte selects the register,
 * so on the bus it looks like several 7-bit addresses:
 *   0x24 (byte 0x48) control: data bit 0 = display on, bits 6..4 =
 *        brightness (0 = brightest, 1..7 = 1/8..7/8), bit 3 = 7-segment mode
 *   0x27 (byte 0x4f) read key code
 *   0x34 / 0x35 / 0x36 / 0x37 (bytes 0x68 / 0x6a / 0x6c / 0x6e) digit
 *        registers 0-3, data = segments (bit order: see fd650_segbit)
 * Stock firmware: pinmux 0xbf15b400 low byte = 0x33, then control 0x41.
 * Any other I2C device on the front cable must avoid 0x24-0x27, 0x34-0x37.
 */
#ifndef FD650_H
#define FD650_H

#include "i2c.h"

#define FD650_PINMUX    0xbf15b400u
#define FD650_CTRL      0x24
#define FD650_KEY       0x27
#define FD650_DIG0      0x34

/* Standard segments (bit 0 = a ... bit 6 = g, bit 7 = dot) for 0-9, A-F */
static const unsigned char fd650_font[16] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
    0x7f, 0x6f, 0x77, 0x7c, 0x39, 0x5e, 0x79, 0x71
};

/*
 * Panel 650G11B wiring: standard segment bit n -> FD650 data bit
 * fd650_segbit[n] (a b c d e f g dot). From the stock firmware's font
 * table (0x8076e6ac: '0' = 0xf5, '1' = 0x05, ...), checked for 0-9.
 */
static const unsigned char fd650_segbit[8] = { 6, 0, 2, 4, 5, 7, 1, 3 };

/* Front buttons (key code with bit 6 = pressed; release = same without 0x40) */
#define FD650_KEY_PRESSED   0x40
#define FD650_KEY_MENU      0x05
#define FD650_KEY_OK        0x2d
#define FD650_KEY_VOLDOWN   0x15
#define FD650_KEY_VOLUP     0x1d
#define FD650_KEY_CHDOWN    0x25
#define FD650_KEY_CHUP      0x35

/* Standard segment bits -> this panel's bits */
static __attribute__ ((unused)) unsigned char fd650_map (unsigned char std) {
    unsigned char v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (std & (1 << i)) {
            v |= 1 << fd650_segbit[i];
        }
    }
    return v;
}

static __attribute__ ((unused)) int fd650_init (u32 prescale) {
    unsigned char on = 0x41;

    REG32 (FD650_PINMUX) = (REG32 (FD650_PINMUX) & ~0xffu) | 0x33;
    i2c_init (I2C_CH_FP, prescale);
    return i2c_write (I2C_CH_FP, FD650_CTRL, &on, 1);
}

static __attribute__ ((unused)) int fd650_control (unsigned char v) {
    return i2c_write (I2C_CH_FP, FD650_CTRL, &v, 1);
}

/* Raw segments to digit n (0-3) */
static __attribute__ ((unused)) int fd650_digit (int n, unsigned char seg) {
    return i2c_write (I2C_CH_FP, FD650_DIG0 + (n & 3), &seg, 1);
}

/*
 * Panel 650G11B layout (fptest, verified): digit registers left to right
 * = 2, 3, 1; register 0 drives only the green LED D1 (bit 1). The dot
 * segments are not wired.
 */
#define FD650_LED_REG       0
#define FD650_LED_BIT       0x02

static const unsigned char fd650_pos_reg[3] = { 2, 3, 1 };

/* Panel segments for a character (0-9, A-F, a-f, '-', ' '; others blank) */
static __attribute__ ((unused)) unsigned char fd650_char (char c) {
    if (c >= '0' && c <= '9') {
        return fd650_map (fd650_font[c - '0']);
    }
    if (c >= 'A' && c <= 'F') {
        return fd650_map (fd650_font[c - 'A' + 10]);
    }
    if (c >= 'a' && c <= 'f') {
        return fd650_map (fd650_font[c - 'a' + 10]);
    }
    if (c == '-') {
        return fd650_map (0x40);
    }
    return 0;
}

/* Show up to 3 characters, left aligned; missing ones are blank */
static __attribute__ ((unused)) void fd650_show (const char *s) {
    int i;

    for (i = 0; i < 3; i++) {
        char c = *s ? *s++ : ' ';

        fd650_digit (fd650_pos_reg[i], fd650_char (c));
    }
}

static __attribute__ ((unused)) void fd650_led (int on) {
    fd650_digit (FD650_LED_REG, on ? FD650_LED_BIT : 0);
}

/* Key code, 0 or negative on error */
static __attribute__ ((unused)) int fd650_key (void) {
    unsigned char k = 0;
    int r = i2c_read (I2C_CH_FP, FD650_KEY, &k, 1);

    return r < 0 ? r : k;
}

#endif
