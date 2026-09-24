/*
 * rtlscan: WiFi scan with the internal RTL8188FTV. Brings the chip up
 * (firmware + MAC/BB/RF init from rtl8188.h), then listens on channels
 * 1-13 and prints every access point whose beacon it receives.
 *
 *   fatload usb 0 ${a} rtlscan.bin
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize} [ms-per-channel]   (default 400, decimal)
 *
 * RX frames arrive on bulk IN endpoint 1: 24-byte RX descriptor, then
 * drvinfo_sz * 8 bytes of PHY status, then 'shift' bytes, then the 802.11
 * frame (rtl8xxxu_parse_rxdesc24). A read blocks until a frame arrives or
 * EHCI times out (~5 s) on a silent channel.
 */
#include "uboot.h"
#include "rtl8188.h"

#define RX_EP           0x81
#define RX_BUF_SIZE     8192
#define MAX_APS         48

static unsigned char rxbuf[RX_BUF_SIZE] __attribute__ ((aligned (32)));

static struct ap {
    unsigned char bssid[6];
    char ssid[33];
    int channel;
    u32 beacons;
} aps[MAX_APS];
static int n_aps;

static u32 parse_dec (const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

static u32 le32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static int same_mac (const unsigned char *a, const unsigned char *b) {
    int i;

    for (i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* 802.11 beacon (type mgmt, subtype 8): addr3 = BSSID, body = 8 timestamp
 * + 2 interval + 2 capability, then IEs (id 0 = SSID, id 3 = channel) */
static void handle_frame (const unsigned char *f, u32 len, int tuned) {
    const unsigned char *ie, *end = f + len;
    struct ap *ap = 0;
    int i, ch = tuned;

    if (len < 36 || f[0] != 0x80) {
        return;
    }
    for (i = 0; i < n_aps; i++) {
        if (same_mac (aps[i].bssid, f + 16)) {
            ap = &aps[i];
            break;
        }
    }
    if (ap) {
        ap->beacons++;
        return;
    }
    if (n_aps == MAX_APS) {
        return;
    }
    ap = &aps[n_aps++];
    for (i = 0; i < 6; i++) {
        ap->bssid[i] = f[16 + i];
    }
    ap->ssid[0] = 0;
    for (ie = f + 36; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
        if (ie[0] == 0) {
            int k = ie[1] > 32 ? 32 : ie[1];

            for (i = 0; i < k; i++) {
                char c = ie[2 + i];

                ap->ssid[i] = (c >= ' ' && c < 127) ? c : '.';
            }
            ap->ssid[k] = 0;
        } else if (ie[0] == 3 && ie[1] == 1) {
            ch = ie[2];
        }
    }
    ap->channel = ch;
    ap->beacons = 1;
    printf ("  ch %2d  %02x:%02x:%02x:%02x:%02x:%02x  \"%s\"%s\n", ch,
            ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4],
            ap->bssid[5], ap->ssid[0] ? ap->ssid : "(hidden)",
            ch != tuned ? "  (heard on neighbour channel)" : "");
}

/* Walk one bulk IN transfer (normally one frame, RX aggregation is off) */
static int handle_rx (const unsigned char *buf, int len, int tuned) {
    int off = 0, frames = 0;

    while (off + 24 <= len) {
        u32 w0 = le32 (buf + off), w2 = le32 (buf + off + 8);
        u32 pktlen = w0 & 0x3fff;
        u32 drvinfo = ((w0 >> 16) & 0xf) * 8;
        u32 shift = (w0 >> 24) & 3;
        u32 total = 24 + drvinfo + shift + pktlen;

        if (pktlen == 0 || off + (int) total > len) {
            break;
        }
        if (!(w0 & (1u << 14)) && !(w2 & (1u << 28))) {     /* CRC ok, not a report */
            handle_frame (buf + off + 24 + drvinfo + shift, pktlen, tuned);
            frames++;
        }
        off += (total + 127) & ~127u;
    }
    return frames;
}

int main (int argc, char *argv[]) {
    const unsigned char *fw;
    u32 fw_size, dwell = 400, t0;
    int r, ch;

    if (argc < 3) {
        printf ("usage: go ${a} <fw-addr> <fw-size> [ms-per-channel]\n");
        return 1;
    }
    fw = (const unsigned char *) parse_hex (argv[1]);
    fw_size = parse_hex (argv[2]);
    if (argc > 3) {
        dwell = parse_dec (argv[3]);
    }
    if ((fw[1] << 8 | (fw[0] & 0xf0)) != 0x88f0) {
        printf ("no RTL8188F firmware at %08x\n", (u32) fw);
        return 1;
    }
    if (rtl_open () < 0) {
        printf ("No Realtek chip. Run 'usb port 1' and 'usb reset' first.\n");
        return 1;
    }

    rtl_read_efuse ();
    printf ("MAC %02x:%02x:%02x:%02x:%02x:%02x, init...\n", rtl_efuse[0xd7],
            rtl_efuse[0xd8], rtl_efuse[0xd9], rtl_efuse[0xda], rtl_efuse[0xdb],
            rtl_efuse[0xdc]);
    t0 = get_timer (0);
    r = rtl_init_device (fw, fw_size);
    printf ("init_device: %d in %d ms  (CR %04x RCR %08x)\n", r, get_timer (t0),
            rd16 (REG_CR), rd32 (0x0608));
    if (r < 0) {
        return 1;
    }
    rtl_enable_rf ();

    printf ("Scanning channels 1-13, %d ms each (silent channels can add ~5 s):\n", dwell);
    for (ch = 1; ch <= 13; ch++) {
        int frames = 0, reads = 0;

        rtl_set_channel (ch);
        t0 = get_timer (0);
        while (get_timer (t0) < dwell) {
            int actual = 0;

            if (ub_bulk (rtl, RX_EP, rxbuf, RX_BUF_SIZE, &actual, 1000) < 0) {
                break;                      /* EHCI timeout: nothing on air */
            }
            reads++;
            frames += handle_rx (rxbuf, actual, ch);
        }
        printf ("ch %2d: %d transfers, %d frames\n", ch, reads, frames);
        if (tstc ()) {
            getc ();
            break;
        }
    }

    printf ("Found %d access points.\n", n_aps);
    return 0;
}
