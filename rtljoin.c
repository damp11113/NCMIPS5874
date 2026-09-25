/*
 * rtljoin: join a WPA2-PSK (CCMP) network with the internal RTL8188FTV.
 * Finds the SSID on channels 1-13, authenticates (open system),
 * associates with an RSN element (CCMP/CCMP/PSK) and runs the 4-way
 * handshake (wpa.h: PBKDF2, PRF, HMAC-SHA1 MICs, AES key unwrap of the
 * group key). Then it stays on the channel and reports whether the access
 * point keeps us or deauthenticates us. Keys are not installed in the chip
 * yet (no encrypted data traffic).
 *
 * WIFI.TXT on the stick (keep it private, never in git):
 *   ssid=MyNetwork
 *   psk=my passphrase
 *
 *   fatload usb 0 ${a} rtljoin.bin
 *   mw.b 83d00000 0 400; fatload usb 0 83d00000 WIFI.TXT
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize} 83d00000
 */
#include "uboot.h"
#include "rtl8188.h"
#include "wpa.h"

void *memcpy (void *dst, const void *src, unsigned int n);
void *memset (void *dst, int c, unsigned int n);
int memcmp (const void *a, const void *b, unsigned int n);
unsigned int strlen (const char *s);

#define RX_EP           0x81
#define RX_BUF_SIZE     8192

enum { S_SCAN, S_AUTH, S_ASSOC, S_4WAY, S_DONE, S_FAIL };

static unsigned char rxbuf[RX_BUF_SIZE] __attribute__ ((aligned (32)));
static unsigned char mac[6], bssid[6];
static u32 seq;
static int state, ap_channel;
static char ssid[33];
static u32 ssid_len;
static char psk[64];
static unsigned char ap_rates[2 + 16], ap_xrates[2 + 16];
static int ap_rsn_ok;

/* Our RSN element: version 1, group CCMP, pairwise CCMP, AKM PSK, caps 0.
 * Sent in the association request and again in message 2 (must match). */
static const unsigned char rsn_ie[22] = {
    0x30, 20, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
    0x00, 0x00,
};

static unsigned char pmk[32], ptk[48], anonce[32], snonce[32], gtk[32];
static u32 gtk_len, gtk_id;

static u32 le32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static u32 be16 (const unsigned char *p) {
    return (p[0] << 8) | p[1];
}

static void put_be16 (unsigned char *p, u32 v) {
    p[0] = v >> 8;
    p[1] = v;
}

/* WIFI.TXT: "ssid=..." and "psk=..." lines, NUL-terminated in RAM */
static int read_wifi_txt (const char *t) {
    while (*t) {
        const char *v;
        u32 n = 0;

        if (!memcmp (t, "ssid=", 5)) {
            v = t + 5;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < 32) {
                ssid[n] = v[n];
                n++;
            }
            ssid[n] = 0;
            ssid_len = n;
        } else if (!memcmp (t, "psk=", 4)) {
            v = t + 4;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < 63) {
                psk[n] = v[n];
                n++;
            }
            psk[n] = 0;
        }
        while (*t && *t != '\n') {
            t++;
        }
        if (*t) {
            t++;
        }
    }
    return ssid_len && psk[0] ? 0 : -1;
}

/* 24-byte 802.11 header */
static unsigned char *hdr (unsigned char *f, u32 fc, const unsigned char *a1,
                           const unsigned char *a2, const unsigned char *a3) {
    f[0] = fc;
    f[1] = fc >> 8;
    f[2] = f[3] = 0;
    memcpy (f + 4, a1, 6);
    memcpy (f + 10, a2, 6);
    memcpy (f + 16, a3, 6);
    f[22] = (seq << 4) & 0xff;
    f[23] = (seq << 4) >> 8;
    seq = (seq + 1) & 0xfff;
    return f + 24;
}

static const unsigned char bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void send_probe (void) {
    static const unsigned char rates[] = {
        0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
        0x32, 0x04, 0x30, 0x48, 0x60, 0x6c,
    };
    unsigned char f[128], *p = hdr (f, 0x0040, bcast, mac, bcast);

    *p++ = 0;
    *p++ = ssid_len;
    memcpy (p, ssid, ssid_len);
    p += ssid_len;
    memcpy (p, rates, sizeof (rates));
    p += sizeof (rates);
    rtl_tx_mgmt (f, p - f);
}

static void send_auth (void) {
    unsigned char f[64], *p = hdr (f, 0x00b0, bssid, mac, bssid);

    *p++ = 0;                   /* algorithm: open system */
    *p++ = 0;
    *p++ = 1;                   /* transaction 1 */
    *p++ = 0;
    *p++ = 0;                   /* status */
    *p++ = 0;
    rtl_tx_mgmt (f, p - f);
}

static void send_assoc (void) {
    unsigned char f[256], *p = hdr (f, 0x0000, bssid, mac, bssid);

    *p++ = 0x31;                /* capability: ESS, privacy, short preamble */
    *p++ = 0x04;                /* short slot time */
    *p++ = 10;                  /* listen interval */
    *p++ = 0;
    *p++ = 0;
    *p++ = ssid_len;
    memcpy (p, ssid, ssid_len);
    p += ssid_len;
    memcpy (p, ap_rates, 2 + ap_rates[1]);
    p += 2 + ap_rates[1];
    if (ap_xrates[1]) {
        memcpy (p, ap_xrates, 2 + ap_xrates[1]);
        p += 2 + ap_xrates[1];
    }
    memcpy (p, rsn_ie, sizeof (rsn_ie));
    p += sizeof (rsn_ie);
    rtl_tx_mgmt (f, p - f);
}

/*
 * EAPOL-Key frame (802.1X version 2, type 3), key descriptor 2 (RSN):
 *   +0 ver, +1 type, +2 length (BE), +4 descriptor type,
 *   +5 key info (BE), +7 key length, +9 replay counter (8), +17 nonce (32),
 *   +49 IV (16), +65 RSC (8), +73 reserved (8), +81 MIC (16),
 *   +97 key data length (BE), +99 key data
 */
#define EK_INFO     5
#define EK_REPLAY   9
#define EK_NONCE    17
#define EK_MIC      81
#define EK_DLEN     97
#define EK_DATA     99

#define KI_VER2     0x0002
#define KI_PAIRWISE 0x0008
#define KI_INSTALL  0x0040
#define KI_ACK      0x0080
#define KI_MIC      0x0100
#define KI_SECURE   0x0200
#define KI_ENC      0x1000

/* Send an EAPOL-Key frame to the AP (data, to DS, LLC/SNAP 0x888e),
 * MIC with KCK = PTK[0..15] */
static void send_eapol (u32 info, const unsigned char *replay, const unsigned char *nonce,
                        const unsigned char *data, u32 dlen) {
    unsigned char f[256], *p = hdr (f, 0x0108, bssid, mac, bssid), *e;
    static const unsigned char llc[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
    unsigned char mic[20];
    u32 elen = EK_DATA + dlen;

    memcpy (p, llc, 8);
    e = p + 8;
    memset (e, 0, elen);
    e[0] = 2;                   /* 802.1X-2004 */
    e[1] = 3;                   /* EAPOL-Key */
    put_be16 (e + 2, elen - 4);
    e[4] = 2;                   /* RSN key descriptor */
    put_be16 (e + EK_INFO, info);
    memcpy (e + EK_REPLAY, replay, 8);
    if (nonce) {
        memcpy (e + EK_NONCE, nonce, 32);
    }
    put_be16 (e + EK_DLEN, dlen);
    if (dlen) {
        memcpy (e + EK_DATA, data, dlen);
    }
    hmac_sha1 (ptk, 16, e, elen, mic);
    memcpy (e + EK_MIC, mic, 16);
    rtl_tx_mgmt (f, (e - f) + elen);
}

static void derive_ptk (void) {
    unsigned char d[76];
    const unsigned char *a = mac, *b = bssid;

    if (memcmp (bssid, mac, 6) < 0) {
        a = bssid;
        b = mac;
    }
    memcpy (d, a, 6);
    memcpy (d + 6, b, 6);
    if (memcmp (anonce, snonce, 32) < 0) {
        memcpy (d + 12, anonce, 32);
        memcpy (d + 44, snonce, 32);
    } else {
        memcpy (d + 12, snonce, 32);
        memcpy (d + 44, anonce, 32);
    }
    wpa_prf (pmk, 32, "Pairwise key expansion", d, 76, ptk, 48);
}

/* MIC check of a received EAPOL-Key frame e (elen bytes) */
static int mic_ok (unsigned char *e, u32 elen) {
    unsigned char got[16], mic[20];

    memcpy (got, e + EK_MIC, 16);
    memset (e + EK_MIC, 0, 16);
    hmac_sha1 (ptk, 16, e, elen, mic);
    memcpy (e + EK_MIC, got, 16);
    return !memcmp (got, mic, 16);
}

/* Key data of message 3 (unwrapped): find the GTK KDE */
static void find_gtk (const unsigned char *k, u32 n) {
    u32 i = 0;

    while (i + 2 <= n) {
        u32 len = k[i + 1];

        if (k[i] == 0xdd && len >= 6 && k[i + 2] == 0x00 && k[i + 3] == 0x0f &&
            k[i + 4] == 0xac && k[i + 5] == 0x01 && len - 6 <= sizeof (gtk)) {
            gtk_id = k[i + 6] & 3;
            gtk_len = len - 6;
            memcpy (gtk, k + i + 8, gtk_len);
        }
        if (k[i] == 0xdd && len == 0) {
            break;                              /* padding */
        }
        i += 2 + len;
    }
}

static void handle_eapol (unsigned char *e, u32 len) {
    u32 info, dlen, elen;
    static unsigned char kd[256];

    if (len < EK_DATA || e[1] != 3 || e[4] != 2) {
        return;
    }
    elen = 4 + be16 (e + 2);
    if (elen > len) {
        return;
    }
    info = be16 (e + EK_INFO);
    dlen = be16 (e + EK_DLEN);
    if ((info & 7) != 2) {
        printf ("EAPOL: key descriptor version %d (only 2 = CCMP supported)\n", info & 7);
        state = S_FAIL;
        return;
    }
    if ((info & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK)) {
        /* message 1: ANonce -> PTK, message 2 with SNonce + our RSN element */
        memcpy (anonce, e + EK_NONCE, 32);
        derive_ptk ();
        send_eapol (KI_VER2 | KI_PAIRWISE | KI_MIC, e + EK_REPLAY, snonce, rsn_ie, sizeof (rsn_ie));
        printf ("EAPOL 1/4 received, 2/4 sent\n");
    } else if ((info & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK | KI_MIC)) {
        /* message 3: check MIC (proves the passphrase), unwrap GTK, send 4 */
        if (!mic_ok (e, elen)) {
            printf ("EAPOL 3/4: MIC WRONG - wrong passphrase?\n");
            state = S_FAIL;
            return;
        }
        if ((info & KI_ENC) && dlen >= 24 && dlen <= sizeof (kd) + 8 && !(dlen & 7)) {
            if (aes_unwrap (ptk + 16, e + EK_DATA, dlen / 8 - 1, kd) == 0) {
                find_gtk (kd, dlen - 8);
            } else {
                printf ("EAPOL 3/4: key data unwrap failed\n");
            }
        }
        send_eapol (KI_VER2 | KI_PAIRWISE | KI_MIC | KI_SECURE, e + EK_REPLAY, 0, 0, 0);
        printf ("EAPOL 3/4 received (MIC ok, GTK %d bytes, key id %d), 4/4 sent\n", gtk_len, gtk_id);
        state = S_DONE;
    }
}

/* One received 802.11 frame */
static void handle_frame (unsigned char *f, u32 len) {
    u32 fc, type, sub, i, h;
    const unsigned char *ie, *end = f + len;

    if (len < 24) {
        return;
    }
    fc = f[0] | (f[1] << 8);
    type = (fc >> 2) & 3;
    sub = (fc >> 4) & 15;

    if (type == 0 && (sub == 5 || sub == 8) && state == S_SCAN && len >= 36) {
        /* probe response / beacon: our SSID? */
        int match = 0, ch = 0, rsn = 0;

        for (ie = f + 36; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
            if (ie[0] == 0 && ie[1] == ssid_len && !memcmp (ie + 2, ssid, ssid_len)) {
                match = 1;
            } else if (ie[0] == 1 && ie[1] <= 16) {
                memcpy (ap_rates, ie, 2 + ie[1]);
            } else if (ie[0] == 50 && ie[1] <= 16) {
                memcpy (ap_xrates, ie, 2 + ie[1]);
            } else if (ie[0] == 3 && ie[1] == 1) {
                ch = ie[2];
            } else if (ie[0] == 48) {
                rsn = 1;
            }
        }
        if (match) {
            memcpy (bssid, f + 16, 6);
            ap_channel = ch;
            ap_rsn_ok = rsn;
            printf ("found \"%s\": BSSID %02x:%02x:%02x:%02x:%02x:%02x channel %d%s\n", ssid,
                    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ch,
                    rsn ? ", WPA2 (RSN)" : ", NO RSN element (open / WPA1?)");
            state = S_AUTH;
        }
        return;
    }
    for (i = 0; i < 6; i++) {
        if (f[4 + i] != mac[i]) {
            return;                     /* not for us */
        }
    }
    if (type == 0 && sub == 11 && state == S_AUTH && len >= 30) {
        u32 status = f[28] | (f[29] << 8);

        printf ("auth response: status %d\n", status);
        state = status ? S_FAIL : S_ASSOC;
        if (!status) {
            send_assoc ();
        }
    } else if (type == 0 && sub == 1 && state == S_ASSOC && len >= 30) {
        u32 status = f[26] | (f[27] << 8), aid = (f[28] | (f[29] << 8)) & 0x3fff;

        printf ("assoc response: status %d, AID %d\n", status, aid);
        state = status ? S_FAIL : S_4WAY;
    } else if (type == 0 && (sub == 12 || sub == 10)) {
        printf ("%s by the AP, reason %d\n", sub == 12 ? "DEAUTHENTICATED" : "DISASSOCIATED",
                len >= 26 ? f[24] | (f[25] << 8) : -1);
        state = S_FAIL;
    } else if (type == 2 && !(fc & 0x4000)) {
        /* unencrypted data: EAPOL? (QoS data has a 2-byte QoS field) */
        h = 24 + ((sub & 8) ? 2 : 0);
        if (len >= h + 8 + EK_DATA && f[h] == 0xaa && f[h + 1] == 0xaa && f[h + 6] == 0x88 &&
            f[h + 7] == 0x8e) {
            handle_eapol (f + h + 8, len - h - 8);
        }
    }
}

static void handle_rx (unsigned char *buf, int len) {
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
            handle_frame (buf + off + 24 + drvinfo + shift, pktlen);
        }
        off += (total + 127) & ~127u;
    }
}

/* Receive for up to ms milliseconds (or until the state changes) */
static void listen (u32 ms) {
    u32 t0 = get_timer (0);
    int s0 = state;

    while (get_timer (t0) < ms && state == s0) {
        int actual = 0;

        if (ub_bulk (rtl, RX_EP, rxbuf, RX_BUF_SIZE, &actual, 500) < 0) {
            continue;
        }
        handle_rx (rxbuf, actual);
    }
}

int main (int argc, char *argv[]) {
    const unsigned char *fw;
    u32 fw_size, t0, c;
    int r, ch, i, tries;

    if (argc < 4) {
        printf ("usage: go ${a} <fw-addr> <fw-size> <WIFI.TXT addr>\n");
        return 1;
    }
    fw = (const unsigned char *) parse_hex (argv[1]);
    fw_size = parse_hex (argv[2]);
    if (read_wifi_txt ((const char *) parse_hex (argv[3])) < 0) {
        printf ("WIFI.TXT: need ssid= and psk= lines\n");
        return 1;
    }
    printf ("network \"%s\", passphrase %d characters\n", ssid, (int) strlen (psk));
    if ((fw[1] << 8 | (fw[0] & 0xf0)) != 0x88f0) {
        printf ("no RTL8188F firmware at %08x\n", (u32) fw);
        return 1;
    }

    t0 = get_timer (0);
    wpa_pmk (psk, (const unsigned char *) ssid, ssid_len, pmk);
    printf ("PMK computed in %d ms\n", (int) get_timer (t0));
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

    /* SNonce: HMAC of the CPU cycle counter, MAC and PMK (two halves) */
    {
        unsigned char in[32], h[20];

        __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
        memcpy (in, pmk, 16);
        memcpy (in + 16, mac, 6);
        for (i = 0; i < 4; i++) {
            in[22 + i] = c >> (8 * i);
        }
        in[26] = 1;
        hmac_sha1 (pmk + 16, 16, in, 27, h);
        memcpy (snonce, h, 20);
        __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
        in[26] = 2;
        in[27] = c;
        hmac_sha1 (pmk + 16, 16, in, 28, h);
        memcpy (snonce + 20, h, 12);
    }

    /* 1: find the network */
    state = S_SCAN;
    for (tries = 0; tries < 2 && state == S_SCAN; tries++) {
        for (ch = 1; ch <= 13 && state == S_SCAN; ch++) {
            rtl_set_channel (ch);
            rtl_set_tx_power (ch);
            send_probe ();
            listen (250);
        }
    }
    if (state != S_AUTH) {
        printf ("\"%s\" not found\n", ssid);
        return 1;
    }
    if (!ap_rsn_ok) {
        printf ("the network does not announce WPA2 - stopping\n");
        return 1;
    }
    if (ap_channel) {
        rtl_set_channel (ap_channel);
        rtl_set_tx_power (ap_channel);
    }

    /* 2: authenticate, associate, 4-way handshake (resend on silence) */
    for (tries = 0; tries < 3 && state == S_AUTH; tries++) {
        send_auth ();
        listen (500);
    }
    for (tries = 0; tries < 3 && state == S_ASSOC; tries++) {
        if (tries) {
            send_assoc ();
        }
        listen (800);
    }
    if (state == S_4WAY) {
        listen (3000);                          /* messages 1 and 3 */
        if (state == S_4WAY) {
            listen (3000);
        }
    }
    if (state != S_DONE) {
        printf ("JOIN FAILED (state %d)\n", state);
        return 1;
    }

    /* 3: stay and see whether the AP keeps us */
    printf ("CONNECTED to \"%s\". Listening 20 s for a deauthentication...\n", ssid);
    listen (20000);
    if (state == S_DONE) {
        printf ("still associated after 20 s. Check your router's list of connected devices.\n");
    }
    return state == S_DONE ? 0 : 1;
}
