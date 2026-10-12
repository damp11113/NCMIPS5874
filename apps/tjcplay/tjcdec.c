/*
 * tjcdec: TJC stream decoder in freestanding C, see tjcdec.h.
 *
 * Port of the decode path of tjc/tjc.h (IJG "islow" integer IDCT, JPEG
 * Annex K Huffman tables, libjpeg-style quality scaling). Every step keeps
 * the original arithmetic so the output stays bit-identical.
 */
#include <stdlib.h>
#include <string.h>
#include "tjcdec.h"

static const uint8_t std_luma_q[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};

static const uint8_t std_chroma_q[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

/* Standard JPEG Huffman tables (ITU T.81 Annex K.3) */
static const uint8_t dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const uint8_t ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

static const uint8_t ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* ---- Huffman ---- */

struct huff_dec {
    uint16_t lut[512];          /* 9-bit lookahead: (length << 8) | symbol, 0 = longer code */
    int32_t maxcode[17];
    int32_t valoff[17];
    uint8_t vals[256];
};

static struct huff_dec huff_dc[2], huff_ac[2];     /* [0] luma, [1] chroma */
static int huff_ready;

static void build_huff(const uint8_t bits[16], const uint8_t *vals, struct huff_dec *dec) {
    uint32_t code = 0;
    int k = 0, len, i;

    memset(dec, 0, sizeof(*dec));
    for (len = 1; len <= 16; len++) {
        int n = bits[len - 1];

        dec->maxcode[len] = -1;
        if (n) {
            dec->valoff[len] = k - (int32_t) code;
            for (i = 0; i < n; i++, k++, code++) {
                uint8_t sym = vals[k];

                dec->vals[k] = sym;
                if (len <= 9) {
                    uint32_t first = code << (9 - len), count = 1u << (9 - len), j;

                    for (j = 0; j < count; j++) {
                        dec->lut[first + j] = (uint16_t) ((len << 8) | sym);
                    }
                }
            }
            dec->maxcode[len] = (int32_t) code - 1;
        }
        code <<= 1;
    }
}

/* ---- bit reader: MSB first, zero bits past the end ---- */

struct bitreader {
    const uint8_t *p;
    uint32_t size, pos;
    uint32_t buf;
    int cnt;
};

static inline void br_refill(struct bitreader *br) {
    while (br->cnt <= 24) {
        uint32_t b = br->pos < br->size ? br->p[br->pos] : 0u;

        br->pos++;
        br->buf |= b << (24 - br->cnt);
        br->cnt += 8;
    }
}

static inline uint32_t br_get(struct bitreader *br, int n) {
    uint32_t v;

    if (n == 0) {
        return 0;
    }
    br_refill(br);
    v = br->buf >> (32 - n);
    br->buf <<= n;
    br->cnt -= n;
    return v;
}

static inline int huff_decode(struct bitreader *br, const struct huff_dec *t) {
    uint32_t e;
    int len;

    br_refill(br);
    e = t->lut[br->buf >> 23];
    if (e) {
        br->buf <<= e >> 8;
        br->cnt -= e >> 8;
        return e & 0xff;
    }
    for (len = 10; len <= 16; len++) {
        int32_t code = (int32_t) (br->buf >> (32 - len));

        if (code <= t->maxcode[len]) {
            br->buf <<= len;
            br->cnt -= len;
            return t->vals[code + t->valoff[len]];
        }
    }
    return -1;
}

static inline int32_t clamp_coef(int32_t v) {
    return v < -2048 ? -2048 : (v > 2047 ? 2047 : v);
}

/* One block into natural-order dequantized coefficients. -1 = bad data,
 * 0 = DC only, 1 = AC present. */
static int decode_block(struct bitreader *br, int table, const uint16_t dq[64], int *dc_pred,
                        int32_t coef[64]) {
    const struct huff_dec *ac = &huff_ac[table];
    int s, diff = 0, has_ac = 0, k;

    memset(coef, 0, 64 * sizeof(int32_t));
    s = huff_decode(br, &huff_dc[table]);
    if (s < 0 || s > 11) {
        return -1;
    }
    if (s) {
        diff = (int) br_get(br, s);
        if (diff < (1 << (s - 1))) {
            diff -= (1 << s) - 1;
        }
    }
    *dc_pred += diff;
    if (*dc_pred < -2047 || *dc_pred > 2047) {
        return -1;
    }
    coef[0] = clamp_coef(*dc_pred * (int32_t) dq[0]);

    for (k = 1; k < 64;) {
        int rs = huff_decode(br, ac), r, a;

        if (rs < 0) {
            return -1;
        }
        r = rs >> 4;
        s = rs & 15;
        if (s == 0) {
            if (r != 15) {
                break;                  /* EOB */
            }
            k += 16;                    /* ZRL */
            continue;
        }
        k += r;
        if (k > 63) {
            return -1;
        }
        a = (int) br_get(br, s);
        if (a < (1 << (s - 1))) {
            a -= (1 << s) - 1;
        }
        coef[zigzag[k]] = clamp_coef(a * (int32_t) dq[k]);
        has_ac = 1;
        k++;
    }
    return has_ac;
}

/* ---- inverse DCT: IJG islow (Loeffler-Ligtenberg-Moschytz), 13-bit
 * constants, 2 extra bits between passes ---- */

#define CONST_BITS      13
#define PASS1_BITS      2
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172
#define DESCALE(x, n)   (((x) + ((int32_t) 1 << ((n) - 1))) >> (n))

static inline uint8_t clamp_u8(int32_t v) {
    return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void idct8x8(const int32_t in[64], uint8_t *dst, int stride) {
    int32_t ws[64];
    int x, y;

    /* Pass 1: columns */
    for (x = 0; x < 8; x++) {
        const int32_t *c = in + x;
        int32_t *w = ws + x;
        int32_t z1, z2, z3, z4, z5, tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;

        if ((c[8] | c[16] | c[24] | c[32] | c[40] | c[48] | c[56]) == 0) {
            int32_t dc = c[0] * (1 << PASS1_BITS);

            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
            continue;
        }
        z2 = c[16];
        z3 = c[48];
        z1 = (z2 + z3) * FIX_0_541196100;
        tmp2 = z1 - z3 * FIX_1_847759065;
        tmp3 = z1 + z2 * FIX_0_765366865;
        z2 = c[0];
        z3 = c[32];
        tmp0 = (z2 + z3) * (1 << CONST_BITS);
        tmp1 = (z2 - z3) * (1 << CONST_BITS);
        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = c[56];
        tmp1 = c[40];
        tmp2 = c[24];
        tmp3 = c[8];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336;
        tmp1 *= FIX_2_053119869;
        tmp2 *= FIX_3_072711026;
        tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        w[0] = DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
        w[56] = DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        w[8] = DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
        w[48] = DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        w[16] = DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
        w[40] = DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        w[24] = DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
        w[32] = DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
    }

    /* Pass 2: rows, removing the remaining scale (factor 8 incl.) and the
     * level shift */
#define SH2 (CONST_BITS + PASS1_BITS + 3)
    for (y = 0; y < 8; y++) {
        const int32_t *r = ws + y * 8;
        uint8_t *o = dst + y * stride;
        int32_t z1, z2, z3, z4, z5, tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;

        if ((r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7]) == 0) {
            memset(o, clamp_u8(DESCALE(r[0] * (1 << CONST_BITS), SH2) + 128), 8);
            continue;
        }
        z2 = r[2];
        z3 = r[6];
        z1 = (z2 + z3) * FIX_0_541196100;
        tmp2 = z1 - z3 * FIX_1_847759065;
        tmp3 = z1 + z2 * FIX_0_765366865;
        tmp0 = (r[0] + r[4]) * (1 << CONST_BITS);
        tmp1 = (r[0] - r[4]) * (1 << CONST_BITS);
        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = r[7];
        tmp1 = r[5];
        tmp2 = r[3];
        tmp3 = r[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336;
        tmp1 *= FIX_2_053119869;
        tmp2 *= FIX_3_072711026;
        tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        o[0] = clamp_u8(DESCALE(tmp10 + tmp3, SH2) + 128);
        o[7] = clamp_u8(DESCALE(tmp10 - tmp3, SH2) + 128);
        o[1] = clamp_u8(DESCALE(tmp11 + tmp2, SH2) + 128);
        o[6] = clamp_u8(DESCALE(tmp11 - tmp2, SH2) + 128);
        o[2] = clamp_u8(DESCALE(tmp12 + tmp1, SH2) + 128);
        o[5] = clamp_u8(DESCALE(tmp12 - tmp1, SH2) + 128);
        o[3] = clamp_u8(DESCALE(tmp13 + tmp0, SH2) + 128);
        o[4] = clamp_u8(DESCALE(tmp13 - tmp0, SH2) + 128);
    }
}

/* Same arithmetic idct8x8 performs when only coefficient 0 is non-zero */
static void idct8x8_dc(int32_t dc, uint8_t *dst, int stride) {
    int32_t ws = dc * (1 << PASS1_BITS);
    uint8_t v = clamp_u8(DESCALE(ws * (1 << CONST_BITS), SH2) + 128);
    int y;

    for (y = 0; y < 8; y++) {
        memset(dst + y * stride, v, 8);
    }
}

/* ---- stream header, layout ---- */

static uint32_t worst_case_tile_bytes(int tile_w, int tile_h) {
    uint32_t luma = (uint32_t) ((tile_w + 7) / 8) * (uint32_t) ((tile_h + 7) / 8);
    uint32_t chroma = (uint32_t) (((tile_w + 1) / 2 + 7) / 8) * (uint32_t) (((tile_h + 1) / 2 + 7) / 8);

    return ((luma + 2 * chroma) * (27 + 63 * 26) + 7) / 8;
}

int tjc_parse_header(const uint8_t *in, int n, struct tjc_header *h) {
    struct tjc_header r;
    int need;

    if (n < 4) {
        return 0;
    }
    if (in[0] != 'T' || in[1] != 'J' || in[2] != 'C' || (in[3] != '1' && in[3] != '2')) {
        return -1;
    }
    memset(&r, 0, sizeof(r));
    r.version = in[3] - '0';
    need = r.version == 1 ? TJC_HDR_SIZE_V1 : TJC_HDR_SIZE;
    if (n < need) {
        return 0;
    }
    r.fps_num = 30;
    r.fps_den = 1;
    if (r.version == 2) {
        r.fps_num = tjc_u32(in + 18);
        r.fps_den = tjc_u32(in + 22);
        r.audio_codec = in[26];
        if (r.fps_num == 0 || r.fps_den == 0 || r.audio_codec > 1) {
            return -1;
        }
        if (r.audio_codec) {
            r.audio_channels = in[27];
            r.audio_rate = tjc_u32(in + 28);
            if (r.audio_channels < 1 || r.audio_channels > TJC_QOA_MAX_CH || r.audio_rate < 1 ||
                    r.audio_rate > 0xffffff) {
                return -1;
            }
        }
    }
    r.width = (int) tjc_u16(in + 4);
    r.height = (int) tjc_u16(in + 6);
    r.tile_w = in[8];
    r.tile_h = in[9];
    if (in[10] != 0 || in[11] > 2) {
        return -1;
    }
    r.refresh_mode = in[11];
    r.refresh_param = (int) tjc_u16(in + 12);
    r.quality = in[14];
    if (r.quality < 1 || r.quality > 100 || r.width < 1 || r.height < 1 || r.tile_w < 1 ||
            r.tile_h < 1 || worst_case_tile_bytes(r.tile_w, r.tile_h) > 65535) {
        return -1;
    }
    *h = r;
    return need;
}

int tjc_dec_init(struct tjc_dec *d, const struct tjc_header *h) {
    int scale = h->quality < 50 ? 5000 / h->quality : 200 - h->quality * 2, k;
    uint32_t ls, cs;

    memset(d, 0, sizeof(*d));
    d->h = *h;
    d->cwidth = (h->width + 1) / 2;
    d->cheight = (h->height + 1) / 2;
    d->cols = (h->width + h->tile_w - 1) / h->tile_w;
    d->rows = (h->height + h->tile_h - 1) / h->tile_h;
    d->tiles = d->cols * d->rows;
    d->pwidth = d->cols * h->tile_w;
    d->pheight = d->rows * h->tile_h;
    d->pcwidth = (d->pwidth + 1) / 2;
    d->pcheight = (d->pheight + 1) / 2;

    ls = (uint32_t) d->pwidth * (uint32_t) d->pheight;
    cs = (uint32_t) d->pcwidth * (uint32_t) d->pcheight;
    d->plane[0] = malloc(ls);
    d->plane[1] = malloc(cs);
    d->plane[2] = malloc(cs);
    if (!d->plane[0] || !d->plane[1] || !d->plane[2]) {
        tjc_dec_free(d);
        return -1;
    }
    memset(d->plane[0], 16, ls);
    memset(d->plane[1], 128, cs);
    memset(d->plane[2], 128, cs);
    d->stride[0] = d->pwidth;
    d->stride[1] = d->stride[2] = d->pcwidth;

    for (k = 0; k < 64; k++) {
        int nat = zigzag[k];
        int l = (std_luma_q[nat] * scale + 50) / 100;
        int c = (std_chroma_q[nat] * scale + 50) / 100;

        d->dq[0][k] = (uint16_t) (l < 1 ? 1 : (l > 255 ? 255 : l));
        d->dq[1][k] = (uint16_t) (c < 1 ? 1 : (c > 255 ? 255 : c));
    }
    if (!huff_ready) {
        build_huff(dc_luma_bits, dc_vals, &huff_dc[0]);
        build_huff(dc_chroma_bits, dc_vals, &huff_dc[1]);
        build_huff(ac_luma_bits, ac_luma_vals, &huff_ac[0]);
        build_huff(ac_chroma_bits, ac_chroma_vals, &huff_ac[1]);
        huff_ready = 1;
    }
    return 0;
}

void tjc_dec_free(struct tjc_dec *d) {
    int c;

    for (c = 0; c < 3; c++) {
        free(d->plane[c]);
        d->plane[c] = 0;
    }
}

void tjc_tile_rect(const struct tjc_dec *d, int tile, int c, int *x, int *y, int *w, int *h) {
    int x0 = (tile % d->cols) * d->h.tile_w, y0 = (tile / d->cols) * d->h.tile_h;

    if (c == 0) {
        *x = x0;
        *y = y0;
        *w = d->h.tile_w;
        *h = d->h.tile_h;
        return;
    }
    *x = (x0 + 1) / 2;
    *y = (y0 + 1) / 2;
    *w = (x0 + d->h.tile_w + 1) / 2 - *x;
    *h = (y0 + d->h.tile_h + 1) / 2 - *y;
}

int tjc_decode_tile(struct tjc_dec *d, const uint8_t *data, uint32_t size, int tile) {
    struct bitreader br;
    int32_t coef[64];
    uint8_t blk[64];
    int c;

    br.p = data;
    br.size = size;
    br.pos = 0;
    br.buf = 0;
    br.cnt = 0;
    for (c = 0; c < 3; c++) {
        int x0, y0, w, h, bx, by, table = c ? 1 : 0, pred = 0;
        int stride = d->stride[c];

        tjc_tile_rect(d, tile, c, &x0, &y0, &w, &h);
        for (by = 0; by < h; by += 8) {
            for (bx = 0; bx < w; bx += 8) {
                int r = decode_block(&br, table, d->dq[table], &pred, coef);
                uint8_t *dst = d->plane[c] + (y0 + by) * stride + x0 + bx;
                int bw = w - bx < 8 ? w - bx : 8, bh = h - by < 8 ? h - by : 8, yy;

                if (r < 0) {
                    return -1;
                }
                if (bw == 8 && bh == 8) {
                    if (r) {
                        idct8x8(coef, dst, stride);
                    } else {
                        idct8x8_dc(coef[0], dst, stride);
                    }
                } else {
                    if (r) {
                        idct8x8(coef, blk, 8);
                    } else {
                        idct8x8_dc(coef[0], blk, 8);
                    }
                    for (yy = 0; yy < bh; yy++) {
                        memcpy(dst + yy * stride, blk + yy * 8, bw);
                    }
                }
            }
        }
    }
    /* more bits consumed than the payload holds = corrupt */
    return br.pos * 8 - (uint32_t) br.cnt > size * 8 ? -1 : 0;
}

/* ---- QOA audio ----
 *
 * Port of the QOA reference decoder (https://github.com/phoboslab/qoa):
 *   Copyright (c) 2023, Dominic Szablewski - https://phoboslab.org
 *   SPDX-License-Identifier: MIT
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions: The above copyright
 *   notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
 *   WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
 */

#define QOA_SLICE_LEN   20

static const int16_t qoa_dequant[16][8] = {
    {1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

static inline uint64_t get_be64(const uint8_t *p) {
    return ((uint64_t) ((uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]) << 32) |
           ((uint32_t) p[4] << 24 | (uint32_t) p[5] << 16 | (uint32_t) p[6] << 8 | p[7]);
}

int tjc_qoa_decode(const uint8_t *data, uint32_t size, int channels, uint32_t rate, int16_t *out,
                   uint32_t *consumed) {
    uint32_t header_size = 8 + 16 * (uint32_t) channels, fsize, frate, hi;
    int32_t hist[TJC_QOA_MAX_CH][4], wt[TJC_QOA_MAX_CH][4];
    const uint8_t *p;
    int fch, samples, slices, c, i, start;

    if (size < header_size) {
        return -1;
    }
    hi = (uint32_t) data[0] << 24 | (uint32_t) data[1] << 16 | (uint32_t) data[2] << 8 | data[3];
    fch = (int) (hi >> 24);
    frate = hi & 0xffffff;
    samples = (int) ((uint32_t) data[4] << 8 | data[5]);
    fsize = (uint32_t) data[6] << 8 | data[7];
    slices = (samples + QOA_SLICE_LEN - 1) / QOA_SLICE_LEN;
    if (fch != channels || frate != rate || samples < 1 || slices > 256 || fsize > size ||
            fsize < header_size + 8 * (uint32_t) slices * (uint32_t) channels) {
        return -1;
    }

    p = data + 8;
    for (c = 0; c < channels; c++, p += 16) {
        for (i = 0; i < 4; i++) {
            hist[c][i] = (int16_t) (p[2 * i] << 8 | p[2 * i + 1]);
            wt[c][i] = (int16_t) (p[8 + 2 * i] << 8 | p[8 + 2 * i + 1]);
        }
    }
    for (start = 0; start < samples; start += QOA_SLICE_LEN) {
        int len = samples - start < QOA_SLICE_LEN ? samples - start : QOA_SLICE_LEN;

        for (c = 0; c < channels; c++, p += 8) {
            uint64_t slice = get_be64(p);
            const int16_t *dq = qoa_dequant[slice >> 60];
            int32_t *h = hist[c], *w = wt[c];
            int16_t *o = out + start * channels + c;
            int k;

            slice <<= 4;
            for (k = 0; k < len; k++, o += channels) {
                int64_t pr = (int64_t) w[0] * h[0] + (int64_t) w[1] * h[1] +
                             (int64_t) w[2] * h[2] + (int64_t) w[3] * h[3];
                int32_t predicted, dequantized = dq[(int) (slice >> 61)], rec, delta;

                pr >>= 13;
                /* valid streams stay far inside; only tames hostile input */
                predicted = pr < -(1 << 24) ? -(1 << 24) : (pr > (1 << 24) ? (1 << 24) : (int32_t) pr);
                rec = predicted + dequantized;
                rec = rec < -32768 ? -32768 : (rec > 32767 ? 32767 : rec);
                *o = (int16_t) rec;
                slice <<= 3;

                delta = dequantized >> 4;
                w[0] += h[0] < 0 ? -delta : delta;
                w[1] += h[1] < 0 ? -delta : delta;
                w[2] += h[2] < 0 ? -delta : delta;
                w[3] += h[3] < 0 ? -delta : delta;
                h[0] = h[1];
                h[1] = h[2];
                h[2] = h[3];
                h[3] = rec;
            }
        }
    }
    *consumed = fsize;
    return samples;
}
