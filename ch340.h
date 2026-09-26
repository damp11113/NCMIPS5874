/*
 * CH340 / CH341 USB-to-UART adapter driver on top of U-Boot's USB stack
 * (ubusb.h). Run 'usb start' first. Protocol from the Linux ch341 driver:
 *   vendor IN  0x5f            read version (2 bytes)
 *   vendor OUT 0xa1 0, 0       serial init
 *   vendor OUT 0x9a reg2:reg1  write register pair (value = reg2 << 8 | reg1,
 *                              index = val2 << 8 | val1)
 *     0x1312: baud prescaler/divisor, 0x0f2c: baud factor low byte,
 *     0x2518: line control (0xc3 = 8N1, RX + TX on)
 *   vendor OUT 0xa4 ~lines     modem control (DTR 0x20, RTS 0x40, active low)
 * Data: bulk OUT endpoint 2, bulk IN endpoint 0x82 (32-byte packets).
 * VERIFIED 2026-09-24: loopback 5/5 at 9600, 115200, 921600, 2000000 baud.
 */
#ifndef CH340_H
#define CH340_H

#include "ubusb.h"

#define CH340_VID       0x1a86
#define CH340_PID       0x7523
#define CH340_EP_OUT    0x02
#define CH340_EP_IN     0x82

static void *ch340_dev;
static unsigned char ch340_buf[512] __attribute__((aligned(32)));

static inline int ch340_out(u32 req, u32 value, u32 index) {
    return ub_control(ch340_dev, req, 0x40, value, index, 0, 0, 1000);
}

/* Returns the chip version (e.g. 0x30), or -1 if no CH340 was found */
static inline int ch340_open(u32 baud) {
    u32 factor, divisor = 3, a;
    int ver;

    ch340_dev = ub_find_device(CH340_VID, CH340_PID);
    if (!ch340_dev) {
        return -1;
    }

    if (ub_control(ch340_dev, 0x5f, 0xc0, 0, 0, ch340_buf, 2, 1000) < 0) {
        return -1;
    }
    ver = ch340_buf[0];

    /* baud = 1532620800 / (factor << (3 * (3 - divisor))) */
    factor = 1532620800u / baud;
    while (factor > 0xfff0 && divisor) {
        factor >>= 3;
        divisor--;
    }
    factor = 0x10000 - factor;
    a = (factor & 0xff00) | divisor;
    if (ver > 0x27) {
        a |= 0x80;
    }

    ch340_out(0xa1, 0, 0);
    ch340_out(0x9a, 0x1312, a);
    ch340_out(0x9a, 0x0f2c, factor & 0xff);
    ch340_out(0x9a, 0x2518, 0x00c3);           /* 8N1, RX + TX enabled */
    ch340_out(0xa4, 0xff9f, 0);                 /* DTR + RTS on */
    return ver;
}

/* Send n bytes; returns bytes sent or -1 */
static inline int ch340_write(const void *data, int n) {
    int done = 0, actual;

    while (done < n) {
        int k = n - done > (int) sizeof(ch340_buf) ? (int) sizeof(ch340_buf) : n - done;
        int i;

        for (i = 0; i < k; i++) {
            ch340_buf[i] = ((const unsigned char *) data)[done + i];
        }
        if (ub_bulk(ch340_dev, CH340_EP_OUT, ch340_buf, k, &actual, 1000) < 0) {
            return -1;
        }
        done += actual;
    }
    return done;
}

/* Receive up to max (<= 512) bytes, ends early on a short packet. Blocks
 * until data arrives or EHCI times out (~5 s), so only call it when the
 * other side is about to send. */
static inline int ch340_read(void *data, int max) {
    int actual = 0, i;

    if (max > (int) sizeof(ch340_buf)) {
        max = sizeof(ch340_buf);
    }
    if (ub_bulk(ch340_dev, CH340_EP_IN, ch340_buf, max, &actual, 1000) < 0) {
        return -1;
    }
    for (i = 0; i < actual; i++) {
        ((unsigned char *) data)[i] = ch340_buf[i];
    }
    return actual;
}

#endif
