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
static __attribute__((unused)) const unsigned char fd650_font[16] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
    0x7f, 0x6f, 0x77, 0x7c, 0x39, 0x5e, 0x79, 0x71
};

/*
 * Panel 650G11B wiring: standard segment bit n -> FD650 data bit
 * fd650_segbit[n] (a b c d e f g dot). From the stock firmware's font
 * table (0x8076e6ac: '0' = 0xf5, '1' = 0x05, ...), checked for 0-9.
 */
static __attribute__((unused)) const unsigned char fd650_segbit[8] = { 6, 0, 2, 4, 5, 7, 1, 3 };

/* Front buttons (key code with bit 6 = pressed; release = same without 0x40) */
#define FD650_KEY_PRESSED   0x40
#define FD650_KEY_MENU      0x05
#define FD650_KEY_OK        0x2d
#define FD650_KEY_VOLDOWN   0x15
#define FD650_KEY_VOLUP     0x1d
#define FD650_KEY_CHDOWN    0x25
#define FD650_KEY_CHUP      0x35

/* Standard segment bits -> this panel's bits */
static __attribute__((unused)) unsigned char fd650_map(unsigned char std) {
    unsigned char v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (std & (1 << i)) {
            v |= 1 << fd650_segbit[i];
        }
    }
    return v;
}

static __attribute__((unused)) int fd650_init(u32 prescale) {
    unsigned char on = 0x41;

    REG32(FD650_PINMUX) = (REG32(FD650_PINMUX) & ~0xffu) | 0x33;
    i2c_init(I2C_CH_FP, prescale);
    return i2c_write(I2C_CH_FP, FD650_CTRL, &on, 1);
}

static __attribute__((unused)) int fd650_control(unsigned char v) {
    return i2c_write(I2C_CH_FP, FD650_CTRL, &v, 1);
}

/* What this program last wrote to each register (the LED shares one) */
static __attribute__((unused)) unsigned char fd650_regs[4];

/* Raw segments to digit register n (0-3) */
static __attribute__((unused)) int fd650_digit(int n, unsigned char seg) {
    fd650_regs[n & 3] = seg;
    return i2c_write(I2C_CH_FP, FD650_DIG0 + (n & 3), &seg, 1);
}

/*
 * Panel 650G11B layout (fptest, verified): digit registers left to right
 * = 2, 3, 1; register 0 drives nothing. The dot segments are not wired,
 * except the right digit's (register 1, bit 3), which is the green LED D1
 * (fptest walk). fd650_show keeps the LED, fd650_led keeps the digit.
 */
#define FD650_LED_REG       1
#define FD650_LED_BIT       0x08

static __attribute__((unused)) const unsigned char fd650_pos_reg[3] = { 2, 3, 1 };

/* Letters of the stock firmware's panel font (already in panel bits) */
static __attribute__((unused)) const struct {
    char c;
    unsigned char seg;
} fd650_letters[] = {
    { 'A', 0xe7 }, { 'B', 0xb6 }, { 'b', 0xb6 }, { 'C', 0xf0 }, { 'c', 0x32 },
    { 'D', 0x37 }, { 'd', 0x37 }, { 'E', 0xf2 }, { 'F', 0xe2 }, { 'H', 0xa7 },
    { 'h', 0xa6 }, { 'L', 0xb0 }, { 'n', 0x26 }, { 'N', 0xe5 }, { 'O', 0xf5 },
    { 'o', 0x36 }, { 'P', 0xe3 }, { 'R', 0xe0 }, { 'r', 0xe0 }, { 'S', 0xd6 },
    { 'T', 0xa2 }, { 't', 0xa2 }, { 'U', 0xb5 }, { '-', 0x02 },
};

/*
 * Panel segments for a character: digits, the stock letters above (exact
 * case first, then the other case), a e f as A E F; others blank.
 */
static __attribute__((unused)) unsigned char fd650_char(char c) {
    unsigned int i;
    char other = (c >= 'a' && c <= 'z') ? c - 32 : (c >= 'A' && c <= 'Z') ? c + 32 : c;

    if (c >= '0' && c <= '9') {
        return fd650_map(fd650_font[c - '0']);
    }
    for (i = 0; i < sizeof(fd650_letters) / sizeof(fd650_letters[0]); i++) {
        if (fd650_letters[i].c == c) {
            return fd650_letters[i].seg;
        }
    }
    for (i = 0; i < sizeof(fd650_letters) / sizeof(fd650_letters[0]); i++) {
        if (fd650_letters[i].c == other) {
            return fd650_letters[i].seg;
        }
    }
    return 0;
}

/* Show up to 3 characters, left to right; missing ones are blank */
static __attribute__((unused)) void fd650_show(const char *s) {
    int i;

    for (i = 0; i < 3; i++) {
        char c = *s ? *s++ : ' ';
        int reg = fd650_pos_reg[i];
        unsigned char seg = fd650_char(c);

        if (reg == FD650_LED_REG) {
            seg = (seg & ~FD650_LED_BIT) | (fd650_regs[reg] & FD650_LED_BIT);
        }
        fd650_digit(reg, seg);
    }
}

static __attribute__((unused)) void fd650_led(int on) {
    fd650_digit(FD650_LED_REG, (fd650_regs[FD650_LED_REG] & ~FD650_LED_BIT) |
                 (on ? FD650_LED_BIT : 0));
}

/* Key code, 0 or negative on error */
static __attribute__((unused)) int fd650_key(void) {
    unsigned char k = 0;
    int r = i2c_read(I2C_CH_FP, FD650_KEY, &k, 1);

    return r < 0 ? r : k;
}

#endif
