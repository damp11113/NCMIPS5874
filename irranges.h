/*
 * Register blocks around the IR receiver, for comparing U-Boot vs stock
 * firmware state. Shared by irtest.c (U-Boot) and hook_irsnap.c (inside
 * the firmware). Same "S addr: 4 words" lines as snap.c, so diffsnap.py
 * works on the logs.
 */
#ifndef IRRANGES_H
#define IRRANGES_H

struct ir_range {
    unsigned int start, words;
};

static const struct ir_range ir_ranges[] = {
    { 0xbf151000, 0x28 },   /* IR receiver (firmware driver, IRQ 40) */
    { 0xbf151400, 0x4 },
    { 0xbf140000, 0x10 },   /* 0xbf140020 bit 30 picks IR timing table */
    { 0xbf156000, 0x10 },   /* pinmux */
    { 0xbf15b400, 0x4 },    /* pad config */
    { 0xbf500000, 0x20 },   /* clock gates / resets */
};

#define IR_NRANGES (sizeof(ir_ranges) / sizeof(ir_ranges[0]))

#endif
