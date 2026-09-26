/*
 * WPA2-PSK crypto for our programs: SHA-1, HMAC-SHA1, PBKDF2 (passphrase
 * -> PMK), the 802.11 PRF (PMK -> PTK), AES-128 and AES key unwrap
 * (RFC 3394, GTK in 4-way handshake message 3). Plain C, no libc needed,
 * so it also builds on the PC for testing (wpatest.c).
 */
#ifndef WPA_H
#define WPA_H

typedef unsigned int wpa_u32;

/* ---- SHA-1 -------------------------------------------------------------- */

struct sha1 {
    wpa_u32 h[5];
    wpa_u32 len;                        /* bytes so far */
    unsigned char buf[64];
};

static wpa_u32 sha1_rol(wpa_u32 x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(wpa_u32 *h, const unsigned char *p) {
    wpa_u32 w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (wpa_u32) p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (; i < 80; i++) {
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            t = ((b & c) | (~b & d)) + 0x5a827999;
        } else if (i < 40) {
            t = (b ^ c ^ d) + 0x6ed9eba1;
        } else if (i < 60) {
            t = ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdc;
        } else {
            t = (b ^ c ^ d) + 0xca62c1d6;
        }
        t += sha1_rol(a, 5) + e + w[i];
        e = d; d = c; c = sha1_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_init(struct sha1 *s) {
    s->h[0] = 0x67452301;
    s->h[1] = 0xefcdab89;
    s->h[2] = 0x98badcfe;
    s->h[3] = 0x10325476;
    s->h[4] = 0xc3d2e1f0;
    s->len = 0;
}

static void sha1_update(struct sha1 *s, const unsigned char *p, wpa_u32 n) {
    while (n--) {
        s->buf[s->len++ & 63] = *p++;
        if (!(s->len & 63)) {
            sha1_block(s->h, s->buf);
        }
    }
}

static void sha1_final(struct sha1 *s, unsigned char *out) {
    wpa_u32 bits = s->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;

    sha1_update(s, &pad, 1);
    while ((s->len & 63) != 56) {
        sha1_update(s, &zero, 1);
    }
    for (i = 0; i < 8; i++) {
        lenb[i] = i < 4 ? 0 : bits >> (8 * (7 - i));
    }
    sha1_update(s, lenb, 8);
    for (i = 0; i < 20; i++) {
        out[i] = s->h[i / 4] >> (8 * (3 - (i & 3)));
    }
}

/* ---- HMAC-SHA1 ---------------------------------------------------------- */

/* Inner/outer states after the key block, reused by PBKDF2 */
struct hmac_sha1 {
    struct sha1 in, out;
};

static void hmac_sha1_init(struct hmac_sha1 *m, const unsigned char *key, wpa_u32 klen) {
    unsigned char k[64], kh[20];
    int i;

    if (klen > 64) {
        struct sha1 s;

        sha1_init(&s);
        sha1_update(&s, key, klen);
        sha1_final(&s, kh);
        key = kh;
        klen = 20;
    }
    for (i = 0; i < 64; i++) {
        k[i] = ((wpa_u32) i < klen ? key[i] : 0) ^ 0x36;
    }
    sha1_init(&m->in);
    sha1_update(&m->in, k, 64);
    for (i = 0; i < 64; i++) {
        k[i] ^= 0x36 ^ 0x5c;
    }
    sha1_init(&m->out);
    sha1_update(&m->out, k, 64);
}

/* One MAC from prepared states; data given as up to 4 pieces (NULL ends) */
static void hmac_sha1_run(const struct hmac_sha1 *m, const unsigned char *d0, wpa_u32 n0,
                           const unsigned char *d1, wpa_u32 n1,
                           const unsigned char *d2, wpa_u32 n2,
                           const unsigned char *d3, wpa_u32 n3, unsigned char *mac) {
    struct sha1 s = m->in;
    unsigned char ih[20];

    sha1_update(&s, d0, n0);
    if (d1) {
        sha1_update(&s, d1, n1);
    }
    if (d2) {
        sha1_update(&s, d2, n2);
    }
    if (d3) {
        sha1_update(&s, d3, n3);
    }
    sha1_final(&s, ih);
    s = m->out;
    sha1_update(&s, ih, 20);
    sha1_final(&s, mac);
}

static void hmac_sha1(const unsigned char *key, wpa_u32 klen, const unsigned char *data,
                       wpa_u32 len, unsigned char *mac) {
    struct hmac_sha1 m;

    hmac_sha1_init(&m, key, klen);
    hmac_sha1_run(&m, data, len, 0, 0, 0, 0, 0, 0, mac);
}

/* ---- PMK = PBKDF2-HMAC-SHA1 (passphrase, SSID, 4096, 32) ---------------- */

static void wpa_pmk(const char *pass, const unsigned char *ssid, wpa_u32 ssid_len,
                     unsigned char *pmk) {
    struct hmac_sha1 m;
    unsigned char u[20], t[20], cnt[4];
    wpa_u32 plen = 0;
    int blk, i, j;

    while (pass[plen]) {
        plen++;
    }
    hmac_sha1_init(&m, (const unsigned char *) pass, plen);
    for (blk = 1; blk <= 2; blk++) {
        cnt[0] = cnt[1] = cnt[2] = 0;
        cnt[3] = blk;
        hmac_sha1_run(&m, ssid, ssid_len, cnt, 4, 0, 0, 0, 0, u);
        for (j = 0; j < 20; j++) {
            t[j] = u[j];
        }
        for (i = 1; i < 4096; i++) {
            hmac_sha1_run(&m, u, 20, 0, 0, 0, 0, 0, 0, u);
            for (j = 0; j < 20; j++) {
                t[j] ^= u[j];
            }
        }
        for (j = 0; j < (blk == 1 ? 20 : 12); j++) {
            pmk[(blk - 1) * 20 + j] = t[j];
        }
    }
}

/* ---- PRF-n (IEEE 802.11 12.7.1.2): HMAC(K, label || 0 || data || i) --- */

static void wpa_prf(const unsigned char *key, wpa_u32 klen, const char *label,
                     const unsigned char *data, wpa_u32 dlen, unsigned char *out, wpa_u32 olen) {
    struct hmac_sha1 m;
    unsigned char mac[20], zero = 0, i = 0;
    wpa_u32 llen = 0, k, pos = 0;

    while (label[llen]) {
        llen++;
    }
    hmac_sha1_init(&m, key, klen);
    while (pos < olen) {
        hmac_sha1_run(&m, (const unsigned char *) label, llen, &zero, 1, data, dlen, &i, 1, mac);
        for (k = 0; k < 20 && pos < olen; k++) {
            out[pos++] = mac[k];
        }
        i++;
    }
}

/* ---- AES-128 ------------------------------------------------------------ */

static const unsigned char aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static unsigned char aes_inv_sbox[256];

static unsigned char aes_xt(unsigned char x) {
    return (x << 1) ^ ((x & 0x80) ? 0x1b : 0);
}

static unsigned char aes_mul(unsigned char a, unsigned char b) {
    unsigned char r = 0;

    while (b) {
        if (b & 1) {
            r ^= a;
        }
        a = aes_xt(a);
        b >>= 1;
    }
    return r;
}

/* 11 round keys of 16 bytes */
static void aes128_key(const unsigned char *key, unsigned char *rk) {
    unsigned char rcon = 1, t[4];
    int i, j;

    for (i = 0; i < 16; i++) {
        rk[i] = key[i];
    }
    for (i = 16; i < 176; i += 4) {
        for (j = 0; j < 4; j++) {
            t[j] = rk[i - 4 + j];
        }
        if (!(i & 15)) {
            unsigned char x = t[0];

            t[0] = aes_sbox[t[1]] ^ rcon;
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[x];
            rcon = aes_xt(rcon);
        }
        for (j = 0; j < 4; j++) {
            rk[i + j] = rk[i - 16 + j] ^ t[j];
        }
    }
}

__attribute__((unused))
static void aes128_encrypt(const unsigned char *rk, unsigned char *b) {
    unsigned char t[16];
    int r, i, c;

    for (i = 0; i < 16; i++) {
        b[i] ^= rk[i];
    }
    for (r = 1; r <= 10; r++) {
        for (i = 0; i < 16; i++) {                      /* SubBytes + ShiftRows */
            t[i] = aes_sbox[b[(i + 4 * (i & 3)) & 15]];
        }
        for (c = 0; c < 16; c += 4) {                   /* MixColumns */
            unsigned char a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3];
            unsigned char x = a0 ^ a1 ^ a2 ^ a3;

            if (r == 10) {
                break;
            }
            t[c] ^= x ^ aes_xt(a0 ^ a1);
            t[c + 1] ^= x ^ aes_xt(a1 ^ a2);
            t[c + 2] ^= x ^ aes_xt(a2 ^ a3);
            t[c + 3] ^= x ^ aes_xt(a3 ^ a0);
        }
        for (i = 0; i < 16; i++) {
            b[i] = t[i] ^ rk[16 * r + i];
        }
    }
}

static void aes128_decrypt(const unsigned char *rk, unsigned char *b) {
    unsigned char t[16];
    int r, i, c;

    if (!aes_inv_sbox[0]) {
        for (i = 0; i < 256; i++) {
            aes_inv_sbox[aes_sbox[i]] = i;
        }
    }
    for (r = 10; r >= 1; r--) {
        for (i = 0; i < 16; i++) {
            t[i] = b[i] ^ rk[16 * r + i];
        }
        if (r != 10) {
            for (c = 0; c < 16; c += 4) {               /* InvMixColumns */
                unsigned char a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3];

                t[c] = aes_mul(a0, 14) ^ aes_mul(a1, 11) ^ aes_mul(a2, 13) ^ aes_mul(a3, 9);
                t[c + 1] = aes_mul(a0, 9) ^ aes_mul(a1, 14) ^ aes_mul(a2, 11) ^ aes_mul(a3, 13);
                t[c + 2] = aes_mul(a0, 13) ^ aes_mul(a1, 9) ^ aes_mul(a2, 14) ^ aes_mul(a3, 11);
                t[c + 3] = aes_mul(a0, 11) ^ aes_mul(a1, 13) ^ aes_mul(a2, 9) ^ aes_mul(a3, 14);
            }
        }
        for (i = 0; i < 16; i++) {                      /* InvShiftRows + InvSubBytes */
            b[(i + 4 * (i & 3)) & 15] = aes_inv_sbox[t[i]];
        }
    }
    for (i = 0; i < 16; i++) {
        b[i] ^= rk[i];
    }
}

/* RFC 3394 unwrap: in = (n + 1) * 8 bytes, out = n * 8 bytes. 0 = ok. */
static int aes_unwrap(const unsigned char *kek, const unsigned char *in, wpa_u32 n,
                       unsigned char *out) {
    unsigned char rk[176], a[8], b[16];
    wpa_u32 i, t;
    int j, k;

    aes128_key(kek, rk);
    for (k = 0; k < 8; k++) {
        a[k] = in[k];
    }
    for (i = 0; i < 8 * n; i++) {
        out[i] = in[8 + i];
    }
    for (j = 5; j >= 0; j--) {
        for (i = n; i >= 1; i--) {
            t = n * j + i;
            for (k = 0; k < 8; k++) {
                b[k] = a[k] ^ (k >= 4 ? t >> (8 * (7 - k)) : 0);
                b[8 + k] = out[8 * (i - 1) + k];
            }
            aes128_decrypt(rk, b);
            for (k = 0; k < 8; k++) {
                a[k] = b[k];
                out[8 * (i - 1) + k] = b[8 + k];
            }
        }
    }
    for (k = 0; k < 8; k++) {
        if (a[k] != 0xa6) {
            return -1;
        }
    }
    return 0;
}

#endif
