// SPDX-License-Identifier: GPL-2.0-only
/* Key table / security setup, TX descriptor security and RX decryption
 * handling follow Linux drivers/net/wireless/realtek/rtl8xxxu (core.c):
 * Copyright (c) 2014 - 2017 Jes Sorensen, portions (c) Realtek. */
/*
 * wlan.h: WPA2-PSK station on the internal RTL8188FTV (rtl8188.h driver).
 *
 *   wlan_join (fw, fw_size, wifi_txt)   scan for the SSID, open auth,
 *                                       association, 4-way handshake
 *   wlan_install_keys ()                pairwise + group key into the
 *                                       chip's key table (CAM): the chip
 *                                       then en/decrypts CCMP itself
 *   wlan_send (dst, type, data, len)    one Ethernet-style frame out
 *   wlan_poll (ms, rx)                  receive for up to ms, call rx for
 *                                       each Ethernet-style frame for us
 *
 * WIFI.TXT (on the stick only): "ssid=..." and "psk=..." lines. The
 * passphrase is never printed.
 *
 * Crypto (wpa.h) is host-tested against the official vectors; the join was
 * verified on the box (rtljoin.c). Key table / security setup and the data
 * path follow Linux rtl8xxxu (rtl8xxxu_set_key, rtl8xxxu_cam_write,
 * rtl8xxxu_fill_txdesc, rx descriptor: security bits 20-22, swdec bit 27;
 * decrypted frames keep the 8-byte CCMP header and the 8-byte MIC).
 */
#ifndef WLAN_H
#define WLAN_H

#include "uboot.h"
#include "rtl8188.h"
#include "wpa.h"

void *memcpy (void *dst, const void *src, unsigned int n);
void *memset (void *dst, int c, unsigned int n);
int memcmp (const void *a, const void *b, unsigned int n);
unsigned int strlen (const char *s);

#define WLAN_RX_EP      0x81
#define WLAN_RX_SIZE    8192

enum { S_SCAN, S_AUTH, S_ASSOC, S_4WAY, S_DONE, S_FAIL };

static unsigned char wlan_rxbuf[WLAN_RX_SIZE] __attribute__ ((aligned (32)));
static unsigned char mac[6], bssid[6];
static u32 seq;
static int state, ap_channel;
static char ssid[33];
static u32 ssid_len;
static char psk[64];
static unsigned char ap_rates[2 + 16], ap_xrates[2 + 16];
static int ap_rsn_ok;
static int wlan_keys_on;                /* keys in the CAM: data is encrypted */
static int wlan_data_be;                /* 1: data on the BE queue / EP 0x03 */
static u32 wlan_pn = 1;                 /* CCMP packet number (48 bits) */
static u32 wlan_rx_frames, wlan_rx_data, wlan_rx_undecrypted, wlan_tx_data, wlan_rx_max;

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

static const unsigned char bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

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

/*
 * Data frame out. Like rtl_tx_mgmt (MGNT queue, EP 0x02, 1 Mbit/s), or
 * with wlan_data_be on the BE queue / EP 0x03 (Linux: 2 OUT endpoints ->
 * BE on the second). sec: TX descriptor security type AES, the chip
 * encrypts and appends the 8-byte MIC (frame carries the CCMP header).
 */
static int wlan_tx (const unsigned char *frame, u32 len, int sec) {
    unsigned char *d = rtl_txbuf;
    u32 i, w, csum = 0;
    int actual;

    if (len > 2048) {
        return -1;
    }
    for (i = 0; i < 40; i++) {
        d[i] = 0;
    }
    d[0] = len & 0xff;
    d[1] = len >> 8;
    d[2] = 40;
    d[3] = 0x80 | 0x08 | 0x04;
    if (frame[4] & 1) {
        d[3] |= 0x01;
    }
    w = (wlan_data_be ? 0x00 : 0x12) << 8;          /* txdw1: queue, macid 0 */
    if (sec) {
        w |= 0x00c00000;                            /* TXDESC_SEC_AES */
    }
    d[4] = w; d[5] = w >> 8; d[6] = w >> 16; d[7] = w >> 24;
    w = 1u << 16;                                   /* AGG_BREAK */
    d[8] = w; d[9] = w >> 8; d[10] = w >> 16; d[11] = w >> 24;
    w = 1u << 8;                                    /* USE_DRIVER_RATE */
    d[12] = w; d[13] = w >> 8; d[14] = w >> 16; d[15] = w >> 24;
    w = 0 | (6u << 18) | (1u << 17);                /* 1M, retry limit 6 */
    d[16] = w; d[17] = w >> 8; d[18] = w >> 16; d[19] = w >> 24;
    w = ((frame[22] | (frame[23] << 8)) >> 4) << 12;
    d[36] = w; d[37] = w >> 8; d[38] = w >> 16; d[39] = w >> 24;
    for (i = 0; i < 32; i += 2) {
        csum ^= d[i] | (d[i + 1] << 8);
    }
    d[28] = csum & 0xff;
    d[29] = csum >> 8;
    for (i = 0; i < len; i++) {
        d[40 + i] = frame[i];
    }
    return ub_bulk (rtl, wlan_data_be ? 0x03 : 0x02, d, 40 + len, &actual, 1000) < 0 ? -1 : 0;
}

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

/* Receive callback: Ethernet-style frame for us (dst, src, EtherType) */
typedef void (*wlan_rx_fn) (const unsigned char *dst, const unsigned char *src, u32 type,
                            const unsigned char *data, u32 len);

static wlan_rx_fn wlan_rx_cb;

/* A data frame from the AP after the handshake (decrypted by the chip:
 * 802.11 header, 8-byte CCMP header, LLC/SNAP, payload, 8-byte MIC) */
static void handle_data (unsigned char *f, u32 len, u32 h, int decrypted) {
    const unsigned char *da = f + 4, *sa = f + 16, *p;
    u32 n;

    if (!(f[1] & 0x02) || (f[1] & 0x01)) {
        return;                                 /* only from DS, not to DS / WDS */
    }
    if (f[1] & 0x40) {
        if (!decrypted) {
            wlan_rx_undecrypted++;
            return;
        }
        if (len < h + 8 + 8 + 8) {
            return;
        }
        p = f + h + 8;
        n = len - h - 8 - 8;
    } else {
        p = f + h;
        n = len - h;
    }
    if (n < 8 || p[0] != 0xaa || p[1] != 0xaa || p[2] != 0x03) {
        return;
    }
    if (!memcmp (sa, mac, 6)) {
        return;                                 /* our own broadcast, sent back by the AP */
    }
    if (be16 (p + 6) == 0x888e) {
        if (!(f[1] & 0x40)) {
            handle_eapol ((unsigned char *) p + 8, n - 8);
        }
        return;                                 /* group rekey (encrypted) not handled yet */
    }
    wlan_rx_data++;
    if (wlan_rx_cb) {
        wlan_rx_cb (da, sa, be16 (p + 6), p + 8, n - 8);
    }
}

/* One received 802.11 frame */
static void handle_frame (unsigned char *f, u32 len, int decrypted) {
    u32 fc, type, sub, i, h;
    const unsigned char *ie, *end = f + len;

    if (len < 24) {
        return;
    }
    wlan_rx_frames++;
    if (len > wlan_rx_max && (f[0] & 0x0c) == 0x08) {
        wlan_rx_max = len;                      /* biggest data frame seen */
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
    if (type == 2 && state == S_DONE && memcmp (f + 10, bssid, 6) == 0 &&
        (!memcmp (f + 4, mac, 6) || (f[4] & 1))) {
        handle_data (f, len, 24 + ((sub & 8) ? 2 : 0), decrypted);
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
        /* unencrypted data during the handshake: EAPOL? */
        h = 24 + ((sub & 8) ? 2 : 0);
        if (len >= h + 8 + EK_DATA && f[h] == 0xaa && f[h + 1] == 0xaa && f[h + 6] == 0x88 &&
            f[h + 7] == 0x8e) {
            handle_eapol (f + h + 8, len - h - 8);
        }
    }
}

/* RX descriptor (24 bytes): w0 pktlen 0-13 (no FCS), crc32 14, icverr 15,
 * drvinfo 16-19 (x8 bytes), security 20-22, shift 24-25, swdec 27 */
static void handle_rx (unsigned char *buf, int len) {
    int off = 0;

    while (off + 24 <= len) {
        u32 w0 = le32 (buf + off), w2 = le32 (buf + off + 8);
        u32 pktlen = w0 & 0x3fff;
        u32 drvinfo = ((w0 >> 16) & 0xf) * 8;
        u32 shift = (w0 >> 24) & 3;
        u32 total = 24 + drvinfo + shift + pktlen;
        int decrypted = ((w0 >> 20) & 7) != 0 && !(w0 & (1u << 27));

        if (pktlen == 0 || off + (int) total > len) {
            break;
        }
        if (!(w0 & (3u << 14)) && !(w2 & (1u << 28))) {
            handle_frame (buf + off + 24 + drvinfo + shift, pktlen, decrypted);
        }
        off += (total + 127) & ~127u;
    }
}

/* Receive for up to ms milliseconds, or until the state changes (join) */
static void wlan_listen (u32 ms, int until_state_change) {
    u32 t0 = get_timer (0);
    int s0 = state;

    while (get_timer (t0) < ms && (!until_state_change || state == s0)) {
        int actual = 0;

        if (ub_bulk (rtl, WLAN_RX_EP, wlan_rxbuf, WLAN_RX_SIZE, &actual, 200) < 0) {
            continue;
        }
        handle_rx (wlan_rxbuf, actual);
    }
}

__attribute__ ((unused))
static int wlan_poll (u32 ms, wlan_rx_fn rx) {
    wlan_rx_cb = rx;
    wlan_listen (ms, 0);
    return state == S_DONE ? 0 : -1;
}

/* Returns 0 when associated with keys negotiated, -1 (message printed) */
static int wlan_join (const unsigned char *fw, u32 fw_size, const char *wifi_txt) {
    u32 t0, c;
    int r, ch, i, tries;

    if (read_wifi_txt (wifi_txt) < 0) {
        printf ("WIFI.TXT: need ssid= and psk= lines\n");
        return -1;
    }
    printf ("network \"%s\", passphrase %d characters\n", ssid, (int) strlen (psk));
    if ((fw[1] << 8 | (fw[0] & 0xf0)) != 0x88f0) {
        printf ("no RTL8188F firmware at %08x\n", (u32) fw);
        return -1;
    }
    t0 = get_timer (0);
    wpa_pmk (psk, (const unsigned char *) ssid, ssid_len, pmk);
    printf ("PMK computed in %d ms\n", (int) get_timer (t0));

    if (rtl_open () < 0) {
        printf ("No Realtek chip. Run 'usb port 1' and 'usb reset' first.\n");
        return -1;
    }
    rtl_read_efuse ();
    for (i = 0; i < 6; i++) {
        mac[i] = rtl_efuse[0xd7 + i];
    }
    r = rtl_init_device (fw, fw_size);
    printf ("init_device: %d, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", r,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (r < 0) {
        return -1;
    }
    rtl_set_mac (mac);
    wr8 (0x0423, 0xff);                         /* HWSEQ_CTRL as Linux */
    rtl_enable_rf ();

    /* SNonce: HMAC of the CPU cycle counter, MAC and PMK (two halves) */
    {
        unsigned char in[32], hsh[20];

        __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
        memcpy (in, pmk, 16);
        memcpy (in + 16, mac, 6);
        for (i = 0; i < 4; i++) {
            in[22 + i] = c >> (8 * i);
        }
        in[26] = 1;
        hmac_sha1 (pmk + 16, 16, in, 27, hsh);
        memcpy (snonce, hsh, 20);
        __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
        in[26] = 2;
        in[27] = c;
        hmac_sha1 (pmk + 16, 16, in, 28, hsh);
        memcpy (snonce + 20, hsh, 12);
    }

    /* 1: find the network */
    state = S_SCAN;
    for (tries = 0; tries < 2 && state == S_SCAN; tries++) {
        for (ch = 1; ch <= 13 && state == S_SCAN; ch++) {
            rtl_set_channel (ch);
            rtl_set_tx_power (ch);
            send_probe ();
            wlan_listen (250, 1);
        }
    }
    if (state != S_AUTH) {
        printf ("\"%s\" not found\n", ssid);
        return -1;
    }
    if (!ap_rsn_ok) {
        printf ("the network does not announce WPA2 - stopping\n");
        return -1;
    }
    if (ap_channel) {
        rtl_set_channel (ap_channel);
        rtl_set_tx_power (ap_channel);
    }

    /* 2: authenticate, associate, 4-way handshake (resend on silence) */
    for (tries = 0; tries < 3 && state == S_AUTH; tries++) {
        send_auth ();
        wlan_listen (500, 1);
    }
    for (tries = 0; tries < 3 && state == S_ASSOC; tries++) {
        if (tries) {
            send_assoc ();
        }
        wlan_listen (800, 1);
    }
    if (state == S_4WAY) {
        wlan_listen (3000, 1);                  /* messages 1 and 3 */
        if (state == S_4WAY) {
            wlan_listen (3000, 1);
        }
    }
    if (state != S_DONE) {
        printf ("JOIN FAILED (state %d)\n", state);
        return -1;
    }
    printf ("CONNECTED to \"%s\"\n", ssid);
    return 0;
}

/*
 * Key table (CAM, Linux rtl8xxxu_cam_write): 8 words per entry at
 * entry << 3; word 0 = control (cipher 4 = CCMP << 2 | key id | valid
 * bit 15 | bit 6 for group keys) | MAC[0..1] << 16, word 1 = MAC[2..5],
 * words 2-5 = key. With the "default key" security config the chip picks
 * the entry by the key id of each frame: pairwise key id 0 -> entry 0,
 * group key id n -> entry n.
 */
static void cam_write (u32 entry, u32 keyid, int group, const unsigned char *key) {
    u32 ctrl = (4u << 2) | keyid | (1u << 15) | (group ? (1u << 6) : 0), val;
    int j;

    for (j = 5; j >= 0; j--) {
        if (j == 0) {
            val = ctrl | (bssid[0] << 16) | ((u32) bssid[1] << 24);
        } else if (j == 1) {
            val = bssid[2] | (bssid[3] << 8) | (bssid[4] << 16) | ((u32) bssid[5] << 24);
        } else {
            val = le32 (key + (j - 2) * 4);
        }
        wr32 (0x0674, val);                     /* REG_CAM_WRITE */
        wr32 (0x0670, (1u << 31) | (1u << 16) | ((entry << 3) + j));    /* REG_CAM_CMD */
        udelay (100);
    }
}

__attribute__ ((unused))
static void wlan_install_keys (void) {
    wr32 (0x0670, (1u << 31) | (1u << 30));     /* clear the whole CAM */
    udelay (100);
    cam_write (0, 0, 0, ptk + 32);              /* TK = PTK[32..47] */
    if (gtk_len >= 16 && gtk_id) {
        cam_write (gtk_id, gtk_id, 1, gtk);
    }
    wr16 (0x0100, rd16 (0x0100) | (1u << 9));   /* REG_CR: CR_SECURITY_ENABLE */
    wr8 (0x0680, 0xcf);         /* SECURITY_CFG: TX/RX sec, (BC) use default keys */
    wlan_keys_on = 1;
    printf ("keys installed: pairwise (entry 0), group key id %d, SECURITY_CFG %02x\n", gtk_id,
            rd8 (0x0680));
}

/* One Ethernet-style frame to dst (data, to DS, CCMP header with our
 * packet number, LLC/SNAP + EtherType). The chip encrypts. */
__attribute__ ((unused))
static int wlan_send (const unsigned char *dst, u32 type, const unsigned char *data, u32 len) {
    static unsigned char f[1600];
    unsigned char *p = hdr (f, wlan_keys_on ? 0x4108 : 0x0108, bssid, mac, dst);

    if (len > 1500) {
        return -1;
    }
    if (wlan_keys_on) {
        u32 pn = wlan_pn++;

        p[0] = pn;                              /* PN0 */
        p[1] = pn >> 8;                         /* PN1 */
        p[2] = 0;
        p[3] = 0x20;                            /* ExtIV, key id 0 */
        p[4] = pn >> 16;                        /* PN2 */
        p[5] = pn >> 24;                        /* PN3 */
        p[6] = 0;
        p[7] = 0;
        p += 8;
    }
    p[0] = 0xaa;
    p[1] = 0xaa;
    p[2] = 0x03;
    p[3] = p[4] = p[5] = 0;
    put_be16 (p + 6, type);
    memcpy (p + 8, data, len);
    wlan_tx_data++;
    return wlan_tx (f, (p + 8 + len) - f, wlan_keys_on);
}

#endif
