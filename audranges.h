/*
 * Register blocks around audio out, for comparing stock firmware (playing
 * sound) vs U-Boot. Shared by hook_audsnap.c and later U-Boot tools, so
 * both print the same "S addr: 4 words" lines for diffsnap.py.
 */
#ifndef AUDRANGES_H
#define AUDRANGES_H

struct aud_range {
    unsigned int start, words;
};

static const struct aud_range aud_ranges[] = {
    { 0xbf490000, 0xc0 },   /* audio out: PCM/SPDIF buffers, FIFO (main fw + AV core) */
    { 0xbf490200, 0x10 },
    { 0xbf110000, 0x30 },   /* AV core: SPDIF sample rate code */
    { 0xbf500000, 0x20 },   /* clock gates (aout init touches 0xbf500034) */
    { 0xbf5d0040, 0x10 },   /* chip config (aout init touches 0xbf5d0044) */
    { 0xbf156000, 0x10 },   /* pinmux (aout touches 0xbf15601c) */
};

#define AUD_NRANGES (sizeof (aud_ranges) / sizeof (aud_ranges[0]))

#endif
