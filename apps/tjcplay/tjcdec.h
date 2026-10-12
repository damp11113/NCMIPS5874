/*
 * tjcdec: decoder for TJC streams (tjc/tjc.h, "Tiled-JPEG Codec") in plain
 * freestanding C for the box: integer only, no C++ runtime.
 *
 * Bit-exact port of tjc::Decoder (TJC1 + TJC2): the same stream gives the
 * same planes and the same audio samples as `tjc decode` on the PC. Frame
 * records are parsed by the caller (tjcplay.c); this file decodes the
 * pieces: stream header, one tile payload into the YUV planes, one QOA
 * audio frame.
 */
#ifndef TJCDEC_H
#define TJCDEC_H

#include <stdint.h>

#define TJC_HDR_SIZE_V1     18
#define TJC_HDR_SIZE        34
#define TJC_FRAME_HDR       5           /* frame_num (4) + force_refresh (1) */
#define TJC_QOA_MAX_FRAME   5120        /* samples per channel per QOA frame */
#define TJC_QOA_MAX_CH      8

struct tjc_header {
    int version;                        /* 1 = TJC1 (30 fps, no audio), 2 */
    int width, height;
    int tile_w, tile_h;
    int refresh_mode, refresh_param;
    int quality;
    uint32_t fps_num, fps_den;
    int audio_codec;                    /* 0 = none, 1 = QOA */
    int audio_channels;
    uint32_t audio_rate;
};

struct tjc_dec {
    struct tjc_header h;
    /* layout */
    int cwidth, cheight;                /* visible chroma size */
    int cols, rows, tiles;
    int pwidth, pheight;                /* padded luma size (whole tiles) */
    int pcwidth, pcheight;              /* padded chroma size */
    /* padded planes, 0 = Y, 1 = Cb, 2 = Cr (black at start) */
    uint8_t *plane[3];
    int stride[3];
    uint16_t dq[2][64];                 /* dequant tables, zigzag order */
};

/* Parses a stream header. n = bytes available (>= 18; 34 for TJC2).
 * Returns the header size (18 / 34), 0 = need more bytes, -1 = bad. */
int tjc_parse_header(const uint8_t *in, int n, struct tjc_header *h);

/* Allocates the planes (malloc) and builds the tables. 0 = ok. */
int tjc_dec_init(struct tjc_dec *d, const struct tjc_header *h);
void tjc_dec_free(struct tjc_dec *d);

/* Decodes one tile payload into the planes. 0 = ok, -1 = corrupt. */
int tjc_decode_tile(struct tjc_dec *d, const uint8_t *data, uint32_t size, int tile);

/* Rectangle of component c (0 = Y) of a tile in its padded plane; w or h
 * is 0 when the tile owns no chroma there (odd tile sizes). */
void tjc_tile_rect(const struct tjc_dec *d, int tile, int c, int *x, int *y, int *w, int *h);

/* Decodes one QOA frame at data into out (interleaved, room for
 * TJC_QOA_MAX_FRAME * channels samples). Returns samples per channel and
 * sets *consumed, -1 = malformed / other channel count or rate. */
int tjc_qoa_decode(const uint8_t *data, uint32_t size, int channels, uint32_t rate, int16_t *out,
                   uint32_t *consumed);

static inline uint32_t tjc_u16(const uint8_t *p) {
    return p[0] | ((uint32_t) p[1] << 8);
}

static inline uint32_t tjc_u32(const uint8_t *p) {
    return p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

#endif
