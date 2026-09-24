/*
 * USB CDC-ACM serial device (e.g. ESP32-C3 USB Serial/JTAG, 303a:1001) on
 * top of U-Boot's USB stack (ubusb.h). Run 'usb start' first. U-Boot has
 * already set the configuration; we read the configuration descriptor and
 * use the two bulk endpoints of the CDC data interface (class 0x0a).
 * No line coding or DTR/RTS is sent: on the ESP32-C3 those lines drive
 * its reset / download mode, and the data flows without them.
 */
#ifndef CDCACM_H
#define CDCACM_H

#include "ubusb.h"

#define ESP_VID         0x303a
#define ESP32C3_PID     0x1001          /* USB Serial/JTAG (C3, S3, C6, H2) */

static void *cdc_dev;
static u32 cdc_ep_in, cdc_ep_out;
static unsigned char cdc_buf[512] __attribute__ ((aligned (32)));

/* 0 = ok, -1 = no such device, -2 = descriptor read failed,
 * -3 = no CDC data interface with bulk IN + OUT */
static inline int cdc_open (u32 vid, u32 pid) {
    int len, i, cls = -1;

    cdc_dev = ub_find_device (vid, pid);
    if (!cdc_dev) {
        return -1;
    }
    if (ub_control (cdc_dev, 0x06, 0x80, 0x0200, 0, cdc_buf, 9, 1000) < 9) {
        return -2;
    }
    len = cdc_buf[2] | (cdc_buf[3] << 8);
    if (len > (int) sizeof (cdc_buf)) {
        len = sizeof (cdc_buf);
    }
    if (ub_control (cdc_dev, 0x06, 0x80, 0x0200, 0, cdc_buf, len, 1000) < len) {
        return -2;
    }

    cdc_ep_in = cdc_ep_out = 0;
    for (i = 0; i + 2 <= len && cdc_buf[i] >= 2 && i + cdc_buf[i] <= len; i += cdc_buf[i]) {
        const unsigned char *d = cdc_buf + i;

        if (d[1] == 4) {                                /* interface: class */
            cls = d[5];
        } else if (d[1] == 5 && cls == 0x0a && (d[3] & 3) == 2) {  /* bulk EP */
            if (d[2] & 0x80) {
                cdc_ep_in = d[2];
            } else {
                cdc_ep_out = d[2];
            }
        }
    }
    return (cdc_ep_in && cdc_ep_out) ? 0 : -3;
}

/* Send n bytes; returns bytes sent or -1 */
static inline int cdc_write (const void *data, int n) {
    int done = 0, actual;

    while (done < n) {
        int k = n - done > (int) sizeof (cdc_buf) ? (int) sizeof (cdc_buf) : n - done;
        int i;

        for (i = 0; i < k; i++) {
            cdc_buf[i] = ((const unsigned char *) data)[done + i];
        }
        if (ub_bulk (cdc_dev, cdc_ep_out, cdc_buf, k, &actual, 1000) < 0) {
            return -1;
        }
        done += k;
    }
    return done;
}

/* Receive up to max (<= 512) bytes, ends early on a short packet. Blocks
 * until data arrives or EHCI times out (~5 s), so only call it when the
 * other side is about to send. */
static inline int cdc_read (void *data, int max) {
    int actual = 0, i;

    if (max > (int) sizeof (cdc_buf)) {
        max = sizeof (cdc_buf);
    }
    if (ub_bulk (cdc_dev, cdc_ep_in, cdc_buf, max, &actual, 1000) < 0) {
        return -1;
    }
    for (i = 0; i < actual; i++) {
        ((unsigned char *) data)[i] = cdc_buf[i];
    }
    return actual;
}

#endif
