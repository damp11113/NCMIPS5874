/*
 * Register blocks to snapshot for the 1080i (U-Boot) vs 1080p (stock
 * firmware) comparison. Shared by snap.c (U-Boot) and hook_snap.c
 * (inside the firmware) so both print the same lines for diffing.
 */
#ifndef SNAPRANGES_H
#define SNAPRANGES_H

struct snap_range {
    unsigned int start, words;
};

static const struct snap_range snap_ranges[] = {
    { 0xbf500000, 0x100 },  /* clocks / resets / PLLs */
    { 0xbf510000, 0x40 },
    { 0xbf5d0000, 0x80 },   /* chip config (clock init reads 0x5c/0x68) */
    { 0xbf157000, 0x10 },   /* HDMI analog */
    { 0xbf440000, 0x80 },   /* display mixer / OSD */
    { 0xbf441000, 0x40 },
    { 0xbf442000, 0x20 },
    { 0xbf443000, 0x20 },
    { 0xbf460000, 0x40 },   /* CSC / encoder tables */
    { 0xbf470000, 0x40 },   /* HDMI timing */
    { 0xbf260000, 0x100 },  /* HDMI TX? */
    { 0xbf261000, 0x180 },
    { 0xbf270000, 0x40 },
    { 0xbf410000, 0x68 },
};

#define SNAP_NRANGES (sizeof(snap_ranges) / sizeof(snap_ranges[0]))

#endif
