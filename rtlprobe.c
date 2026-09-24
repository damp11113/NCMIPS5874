/*
 * rtlprobe: first transmission with the internal RTL8188FTV. On each
 * channel 1-13 it sends two broadcast probe requests (wildcard SSID) and
 * listens; every probe response addressed to our own MAC proves that the
 * chip really transmitted.
 *
 *   fatload usb 0 ${a} rtlprobe.bin
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize} [ms-per-channel]   (default 300, decimal)
 */
#include "uboot.h"
#include "rtl8188.h"

#define RX_EP           0x81
#define RX_BUF_SIZE     8192

static unsigned char rxbuf[RX_BUF_SIZE] __attribute__ ((aligned (32)));
static unsigned char mac[6];
static u32 seq;
static int responses, beacons;

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

/* Broadcast probe request: wildcard SSID, 1-54 Mbit/s rates */
static int send_probe (void) {
    static const unsigned char body[] = {
        0x00, 0x00,                                             /* SSID: any */
        0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,  /* rates */
        0x32, 0x04, 0x30, 0x48, 0x60, 0x6c,                     /* ext rates */
    };
    unsigned char f[24 + sizeof (body)];
    u32 i;

    f[0] = 0x40;                                /* mgmt, subtype 4: probe request */
    f[1] = 0x00;
    f[2] = f[3] = 0;                            /* duration */
    for (i = 0; i < 6; i++) {
        f[4 + i] = 0xff;                        /* addr1 = broadcast */
        f[10 + i] = mac[i];                     /* addr2 = us */
        f[16 + i] = 0xff;                       /* addr3 = broadcast BSSID */
    }
    f[22] = (seq << 4) & 0xff;
    f[23] = (seq << 4) >> 8;
    seq = (seq + 1) & 0xfff;
    for (i = 0; i < sizeof (body); i++) {
        f[24 + i] = body[i];
    }
    return rtl_tx_mgmt (f, sizeof (f));
}

static void handle_frame (const unsigned char *f, u32 len, int ch) {
    const unsigned char *ie, *end = f + len;
    char ssid[33];
    int i;

    if (len < 36) {
        return;
    }
    if (f[0] == 0x80) {
        beacons++;
        return;
    }
    if (f[0] != 0x50) {                         /* probe response */
        return;
    }
    for (i = 0; i < 6; i++) {
        if (f[4 + i] != mac[i]) {
            return;                             /* for another station */
        }
    }
    ssid[0] = 0;
    for (ie = f + 36; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
        if (ie[0] == 0) {
            int k = ie[1] > 32 ? 32 : ie[1];

            for (i = 0; i < k; i++) {
                char c = ie[2 + i];

                ssid[i] = (c >= ' ' && c < 127) ? c : '.';
            }
            ssid[k] = 0;
        }
    }
    responses++;
    printf ("  ch %2d  PROBE RESPONSE to us from %02x:%02x:%02x:%02x:%02x:%02x  \"%s\"\n",
            ch, f[16], f[17], f[18], f[19], f[20], f[21], ssid[0] ? ssid : "(hidden)");
}

static void handle_rx (const unsigned char *buf, int len, int ch) {
    int off = 0;

    while (off + 24 <= len) {
        u32 w0 = le32 (buf + off), w2 = le32 (buf + off + 8);
        u32 pktlen = w0 & 0x3fff;
        u32 drvinfo = ((w0 >> 16) & 0xf) * 8;
        u32 shift = (w0 >> 24) & 3;
        u32 total = 24 + drvinfo + shift + pktlen;

        if (pktlen == 0 || off + (int) total > len) {
            break;
        }
        if (!(w0 & (1u << 14)) && !(w2 & (1u << 28))) {
            handle_frame (buf + off + 24 + drvinfo + shift, pktlen, ch);
        }
        off += (total + 127) & ~127u;
    }
}

int main (int argc, char *argv[]) {
    const unsigned char *fw;
    u32 fw_size, dwell = 300, t0;
    int r, ch, i;

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
    for (i = 0; i < 6; i++) {
        mac[i] = rtl_efuse[0xd7 + i];
    }
    r = rtl_init_device (fw, fw_size);
    printf ("init_device: %d, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", r,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (r < 0) {
        return 1;
    }
    rtl_set_mac (mac);
    wr8 (0x0423, 0xff);                         /* HWSEQ_CTRL as Linux */
    rtl_enable_rf ();

    for (ch = 1; ch <= 13; ch++) {
        int sent = 0, before = responses;

        rtl_set_channel (ch);
        rtl_set_tx_power (ch);
        for (i = 0; i < 2; i++) {
            if (send_probe () == 0) {
                sent++;
            }
        }
        t0 = get_timer (0);
        while (get_timer (t0) < dwell) {
            int actual = 0;

            if (ub_bulk (rtl, RX_EP, rxbuf, RX_BUF_SIZE, &actual, 1000) < 0) {
                break;
            }
            handle_rx (rxbuf, actual, ch);
        }
        printf ("ch %2d: sent %d probes, %d responses to us\n", ch, sent,
                responses - before);
        if (tstc ()) {
            getc ();
            break;
        }
    }
    printf ("Total: %d probe responses to us, %d beacons seen.\n", responses, beacons);
    if (responses) {
        printf ("TX WORKS: access points answered our probe requests.\n");
    }
    return 0;
}
