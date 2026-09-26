/*
 * SoC I2C master (Montage CS8001B family, found in the satellite box's
 * stock firmware, driver type 0x501 at 0x8028c818..0x8028e400).
 *
 * OpenCores-like controller, byte registers, one block per channel:
 *   +0x04 status   bit 7 = NACK received, bit 5 = arbitration lost / error
 *   +0x08 TX data
 *   +0x0c RX data
 *   +0x10 control  0x80 = enable (0xc0 = enable + interrupt)
 *   +0x14 command  0x80 START, 0x40 STOP, 0x20 WRITE, 0x10 READ,
 *                  0x04 NACK after a read (last byte);
 *                  reads back 4 when the command is done
 *   +0x18 prescaler high byte, +0x1c low byte:
 *                  prescale = clk / (5 * SCL) - 1  (stock: 100 kHz)
 *
 * Channels (stock names): 0 i2c0, 1 i2c1, 2 i2c_FP (front panel, pins
 * muxed by 0xbf15b400 low byte = 0x33), 3 i2c_HDMI, 4 i2c_QAM.
 * Channel 2's input clock is not known yet (stock reads it from a clock
 * table at run time), so callers pass the prescaler.
 *
 * Polled only; no interrupts. All functions return 0 or a negative
 * I2C_E* code.
 */
#ifndef I2C_H
#define I2C_H

#include "uboot.h"

#define I2C_ST          0x04
#define I2C_TXD         0x08
#define I2C_RXD         0x0c
#define I2C_CTRL        0x10
#define I2C_CMD         0x14
#define I2C_PRE_HI      0x18
#define I2C_PRE_LO      0x1c

#define I2C_CMD_START   0x80
#define I2C_CMD_STOP    0x40
#define I2C_CMD_WRITE   0x20
#define I2C_CMD_READ    0x10
#define I2C_CMD_NACK    0x04
#define I2C_CMD_DONE    0x04        /* value read back from I2C_CMD */

#define I2C_ST_NACK     0x80
#define I2C_ST_ERR      0x20

#define I2C_E_TIMEOUT   (-1)
#define I2C_E_NACK      (-2)
#define I2C_E_BUS       (-3)
#define I2C_E_CHANNEL   (-4)

#define I2C_CH_FP       2

static __attribute__((unused)) const u32 i2c_bases[5] = {
    0xbf560000u, 0xbf570000u, 0xbf158000u, 0xbf5c0000u, 0xbf580000u
};

static __attribute__((unused)) u32 i2c_base(int ch) {
    return (ch >= 0 && ch < 5) ? i2c_bases[ch] : 0;
}

/* Last value read from I2C_CMD by a timed-out wait (for diagnosis) */
static __attribute__((unused)) unsigned char i2c_last_cmd;

static __attribute__((unused)) int i2c_wait(u32 base) {
    int i;

    for (i = 0; i < 20000; i++) {
        i2c_last_cmd = REG8(base + I2C_CMD);
        if (i2c_last_cmd == I2C_CMD_DONE) {
            return 0;
        }
        udelay(2);
    }
    return I2C_E_TIMEOUT;
}

/* Enable channel ch with the given prescaler (see above) */
static __attribute__((unused)) int i2c_init(int ch, u32 prescale) {
    u32 base = i2c_base(ch);

    if (!base) {
        return I2C_E_CHANNEL;
    }
    REG8(base + I2C_PRE_HI) = (prescale >> 8) & 0xff;
    REG8(base + I2C_PRE_LO) = prescale & 0xff;
    REG8(base + I2C_CTRL) = 0x80;
    return 0;
}

/* Send one byte, with a START condition first if start != 0 */
static __attribute__((unused)) int i2c_write_byte(u32 base, unsigned char b, int start) {
    unsigned char st;

    REG8(base + I2C_TXD) = b;
    REG8(base + I2C_CMD) = start ? (I2C_CMD_START | I2C_CMD_WRITE) : I2C_CMD_WRITE;
    if (i2c_wait(base) < 0) {
        return I2C_E_TIMEOUT;
    }
    st = REG8(base + I2C_ST);
    if (st & I2C_ST_ERR) {
        return I2C_E_BUS;
    }
    return (st & I2C_ST_NACK) ? I2C_E_NACK : 0;
}

/* Receive one byte; last != 0 answers with NACK (end of the read) */
static __attribute__((unused)) int i2c_read_byte(u32 base, unsigned char *b, int last) {
    REG8(base + I2C_CMD) = last ? (I2C_CMD_READ | I2C_CMD_NACK) : I2C_CMD_READ;
    if (i2c_wait(base) < 0) {
        return I2C_E_TIMEOUT;
    }
    if (REG8(base + I2C_ST) & I2C_ST_ERR) {
        return I2C_E_BUS;
    }
    *b = REG8(base + I2C_RXD);
    return 0;
}

static __attribute__((unused)) int i2c_stop(u32 base) {
    REG8(base + I2C_CMD) = I2C_CMD_STOP;
    if (i2c_wait(base) < 0) {
        return I2C_E_TIMEOUT;
    }
    return (REG8(base + I2C_ST) & I2C_ST_ERR) ? I2C_E_BUS : 0;
}

/* Write len bytes to 7-bit address addr (len 0 = address probe) */
static __attribute__((unused)) int i2c_write(int ch, int addr, const unsigned char *data, int len) {
    u32 base = i2c_base(ch);
    int i, r;

    if (!base) {
        return I2C_E_CHANNEL;
    }
    r = i2c_write_byte(base, (unsigned char) (addr << 1), 1);
    for (i = 0; r == 0 && i < len; i++) {
        r = i2c_write_byte(base, data[i], 0);
    }
    i2c_stop(base);
    return r;
}

/* Read len bytes from 7-bit address addr */
static __attribute__((unused)) int i2c_read(int ch, int addr, unsigned char *data, int len) {
    u32 base = i2c_base(ch);
    int i, r;

    if (!base) {
        return I2C_E_CHANNEL;
    }
    r = i2c_write_byte(base, (unsigned char) ((addr << 1) | 1), 1);
    for (i = 0; r == 0 && i < len; i++) {
        r = i2c_read_byte(base, &data[i], i == len - 1);
    }
    i2c_stop(base);
    return r;
}

#endif
