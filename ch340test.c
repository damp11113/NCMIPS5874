/*
 * ch340test: loopback test for the CH340 driver (ch340.h + ubusb.h).
 *
 * Put a jumper wire between the CH340's TXD and RXD pins, plug it into
 * the box (hub is fine), then in U-Boot:
 *   usb start
 *   go ${a} [baud]          default 115200 (decimal)
 *
 * Lists the USB devices U-Boot knows, opens the CH340, sends 5 lines and
 * reads each one back.
 */
#include "uboot.h"
#include "ch340.h"

static u32 parse_dec (const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

int main (int argc, char *argv[]) {
    u32 baud = (argc > 1) ? parse_dec (argv[1]) : 115200;
    char msg[24], rx[40];
    int i, ver, ok = 0;

    printf ("reloc_off %08x\n", ub_reloc_off ());
    for (i = 0; i < UB_USB_MAX_DEVICE; i++) {
        void *dev = ub_usb_dev (i);

        if (dev) {
            printf ("usb_dev[%d] at %08x: devnum %d speed %d mps %d  %04x:%04x\n", i,
                    (u32) dev, UB_DEV_DEVNUM (dev), UB_DEV_SPEED (dev), UB_DEV_MPS (dev),
                    UB_DEV_VID (dev), UB_DEV_PID (dev));
        }
    }

    ver = ch340_open (baud);
    if (ver < 0) {
        printf ("No CH340 (1a86:7523) found. Run 'usb start' first.\n");
        return 1;
    }
    printf ("CH340 version %02x, %d baud 8N1\n", ver, baud);

    for (i = 0; i < 5; i++) {
        int n, got = 0, tries;

        n = 0;
        msg[n++] = 'P'; msg[n++] = 'I'; msg[n++] = 'N'; msg[n++] = 'G';
        msg[n++] = ' '; msg[n++] = '0' + i; msg[n++] = '\r'; msg[n++] = '\n';

        if (ch340_write (msg, n) != n) {
            printf ("write %d failed\n", i);
            continue;
        }
        /* Loopback: the bytes come back within a few ms */
        for (tries = 0; tries < 4 && got < n; tries++) {
            int k = ch340_read (rx + got, n - got);

            if (k < 0) {
                break;
            }
            got += k;
        }
        rx[got] = 0;
        printf ("sent %d, got %d: %s", n, got, got ? rx : "(nothing)\n");
        if (got == n) {
            ok++;
        }
    }
    printf ("ch340test: %d/5 lines came back\n", ok);
    return ok == 5 ? 0 : 1;
}
