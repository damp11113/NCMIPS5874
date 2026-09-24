/*
 * Audio out (block 0xbf490000), streaming PCM from U-Boot programs.
 * Worked out from stock-firmware snapshots while it played an mp3
 * (hook_audsnap) and the AV core's add-data routine (0x87e18180).
 *
 * Address / size / pointer registers hold physical byte address >> 3.
 * Output rate 48 kHz.
 *
 * Buffer 0 (+0x18 base, +0x1c size, +0x20 reserve, +0x24 commit,
 * +0x100 read ptr, +0x104 fill level, +0x10c write ptr readback):
 * frame = 16 bytes, word 0 = two 16-bit samples (hi, lo), words 1-3 zero
 * (as the stock firmware). VERIFIED: clean sine on RCA at the right pitch.
 * Adding data: copy into the ring at the write pointer, then
 * +0x24 = bytes >> 3 in bits 0-24, then set +0x24 bit 31.
 * Free space = (size - level - reserve) << 3 bytes.
 *
 * Buffer 1 (+0x28 base, +0x2c size, +0x110 read ptr, +0x114 level):
 * packed 16-bit stereo (stock firmware "PP buf", filled by the AV core's
 * DMA engine at 0xbe900000). Feeding it via a guessed +0x34 commit made
 * RCA silent and HDMI worse, so it is only configured, not fed.
 * +0x80 = reset: AV core init writes 3, 0, 0x10, 0 with delays
 * (0x87e16e70); audio_start () does the same.
 *
 * HDMI: the HDMI TX (byte regs at 0xbf480000) takes I2S lanes SD0-SD3
 * per reg 0x51 bits 2-5. U-Boot leaves 0xbc (all 4 lanes, = the 12 kHz
 * junk: frame words 1-3 mixed in); the stock fw switches it to 0x84
 * (SD0 only) when playback starts. audio_start () does the same.
 * VERIFIED 2026-09-24 (audplay, PC analyser): HDMI and RCA both clean
 * 440.03 Hz, both silent after audio_stop (). TX reg 0x50 left as is.
 *
 *   audio_start ();
 *   while (...) {
 *       n = audio_space ();            frames that can be written now
 *       audio_write (samples, n);      interleaved hi/lo 16-bit pairs
 *   }
 *   audio_stop ();
 */
#ifndef AUDIO_H
#define AUDIO_H

#include "uboot.h"

#define AUD_BASE        0xbf490000
#define AUD_REG(off)    REG32 (AUD_BASE + (off))

#define AUD_RATE        48000
#define AUD_FRAME       16                  /* bytes per frame, buffer 0 */
#define AUD_FRAME1      4                   /* bytes per frame, buffer 1 */
/* Buffers take 0x90000 bytes of free RAM from AUD_BUF_PHYS; a program
 * can #define AUD_BUF_PHYS before including audio.h to move them. */
#ifndef AUD_BUF_PHYS
#define AUD_BUF_PHYS    0x03800000          /* buffer 0, free RAM */
#endif
#define AUD_BUF_SIZE    0x40000             /* 256 KB = 16384 frames */
#define AUD_BUF1_PHYS   (AUD_BUF_PHYS + 0x40000)    /* buffer 1 */
#define AUD_BUF1_SIZE   0x10000             /* 64 KB = 16384 frames */
#define AUD_BUF2_PHYS   (AUD_BUF_PHYS + 0x50000)    /* buffer 2: unused, silent */
#define AUD_BUF2_SIZE   0x40000

#define HDMI_TX_I2S_CH  0xbf480051          /* HDMI TX reg 0x51 */

#define AUD_UNCACHED(phys) ((volatile u32 *) (0xa0000000u | (phys)))
#define AUD_MASK        0x1ffffff           /* 25-bit unit fields */

static u32 aud_wr;      /* byte offset of the next frame, buffer 0 */

/* Registers copied from the stock firmware while playing (snapshot #6).
 * Buffer, pointer and status registers are set separately. */
static const struct { unsigned short off; u32 val; } aud_play_regs[] = {
    { 0x04, 0x00305254 }, { 0x0c, 0x00000182 },
    { 0x10, 0x001807a5 }, { 0x14, 0x00200000 },
    { 0x4c, 0x9fe00000 }, { 0x50, 0x32f6420a }, { 0x54, 0x000f3fff },
    { 0x58, 0x006f0020 }, { 0x5c, 0x01008006 },
    { 0x60, 0x00007ff0 }, { 0x64, 0x7ff00000 }, { 0x68, 0x20002000 },
    { 0x70, 0x00002000 }, { 0x74, 0x20000000 }, { 0x78, 0x00001000 },
    { 0x7c, 0x10000000 }, { 0x88, 0x00189374 }, { 0x8c, 0x00200000 },
    { 0x90, 0x00000480 }, { 0x94, 0x00400000 }, { 0x98, 0x00400000 },
    { 0x9c, 0x00000bb8 }, { 0xa0, 0x00211e00 },
};

/* Free units of a buffer: size - level - reserve (0 if negative) */
static inline u32 aud_free_units (u32 size_reg, u32 level_reg, u32 reserve_reg) {
    u32 units = (AUD_REG (size_reg) & AUD_MASK) - (AUD_REG (level_reg) & AUD_MASK) -
                (AUD_REG (reserve_reg) & AUD_MASK);

    return (units & 0x80000000) ? 0 : units;
}

/* Tell the hardware how many bytes were added, then commit with bit 31 */
static inline void aud_commit (u32 reg, u32 bytes) {
    AUD_REG (reg) = (AUD_REG (reg) & 0xfe000000) | (bytes >> 3);
    AUD_REG (reg) = (AUD_REG (reg) & 0x7fffffff) | 0x80000000;
}

/* Frames that can be added now */
static inline u32 audio_space (void) {
    return (aud_free_units (0x1c, 0x104, 0x20) << 3) / AUD_FRAME;
}

/* Write n stereo frames: samples[2*i] = hi half, samples[2*i+1] = lo half */
static inline void audio_write (const short *samples, u32 n) {
    volatile u32 *buf0 = AUD_UNCACHED (AUD_BUF_PHYS);
    u32 i;

    for (i = 0; i < n; i++) {
        u32 w = ((u32) (unsigned short) samples[2 * i] << 16) |
                (unsigned short) samples[2 * i + 1];

        buf0[aud_wr / 4] = w;
        aud_wr = (aud_wr + AUD_FRAME) & (AUD_BUF_SIZE - 1);
    }
    aud_commit (0x24, n * AUD_FRAME);
}

static inline void aud_clear (u32 phys, u32 size) {
    volatile u32 *p;

    for (p = AUD_UNCACHED (phys); p < AUD_UNCACHED (phys + size); p++) {
        *p = 0;
    }
}

/* Reset pulse as the AV core does at init (0x87e16e70) */
static inline void aud_reset (void) {
    AUD_REG (0x80) = 3;
    udelay (100);
    AUD_REG (0x80) = 0;
    udelay (100);
    AUD_REG (0x80) = 0x10;
    udelay (100);
    AUD_REG (0x80) = 0;
}

static inline void audio_start (void) {
    u32 i;

    AUD_REG (0x00) = 0x115;                         /* idle value */
    aud_reset ();
    aud_clear (AUD_BUF_PHYS, AUD_BUF_SIZE);
    aud_clear (AUD_BUF1_PHYS, AUD_BUF1_SIZE);
    aud_clear (AUD_BUF2_PHYS, AUD_BUF2_SIZE);

    for (i = 0; i < sizeof (aud_play_regs) / sizeof (aud_play_regs[0]); i++) {
        AUD_REG (aud_play_regs[i].off) = aud_play_regs[i].val;
    }
    AUD_REG (0x18) = AUD_BUF_PHYS >> 3;
    AUD_REG (0x1c) = AUD_BUF_SIZE >> 3;
    AUD_REG (0x28) = AUD_BUF1_PHYS >> 3;
    AUD_REG (0x2c) = AUD_BUF1_SIZE >> 3;
    AUD_REG (0x38) = AUD_BUF2_PHYS >> 3;
    AUD_REG (0x3c) = AUD_BUF2_SIZE >> 3;

    /* Hardware keeps the write pointer; start writing where it points */
    aud_wr = (((AUD_REG (0x10c) & 0x3ffffff) << 3) - AUD_BUF_PHYS) & (AUD_BUF_SIZE - 1);

    REG8 (HDMI_TX_I2S_CH) = 0x84;                   /* HDMI: SD0 only */

    AUD_REG (0x00) = 0x305;                         /* playing value */
}

static inline void audio_stop (void) {
    aud_clear (AUD_BUF_PHYS, AUD_BUF_SIZE);         /* no leftovers on HDMI */
    aud_clear (AUD_BUF1_PHYS, AUD_BUF1_SIZE);
    AUD_REG (0x00) = 0x115;
    AUD_REG (0x60) = 0x00002000;
    AUD_REG (0x64) = 0x20000000;
}

#endif
