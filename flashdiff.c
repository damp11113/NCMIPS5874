/*
 * flashdiff: compare a flash copy with a reference (backup.bin) in RAM and
 * print only the ranges that differ (gaps < 16 bytes merged), with the
 * first bytes of each side.
 *
 * In U-Boot:
 *   usb start
 *   fatload usb 0 0x83000000 backup.bin
 *   sf probe 0; sf read 0x82000000 0 0x800000
 *   fatload usb 0 ${a} flashdiff.bin
 *   go ${a} 82000000 83000000 800000
 */
#include "uboot.h"

static void dump (const char *tag, const unsigned char *p, u32 n) {
    u32 i;

    printf ("    %s", tag);
    for (i = 0; i < n; i++) {
        printf ("%02x", p[i]);
    }
    printf ("\n");
}

int main (int argc, char *argv[]) {
    const unsigned char *a, *b;
    u32 len, i = 0, ranges = 0, total = 0;

    if (argc < 4) {
        printf ("usage: go ${a} <flash copy> <reference> <length> (hex)\n");
        return 1;
    }
    a = (const unsigned char *) parse_hex (argv[1]);
    b = (const unsigned char *) parse_hex (argv[2]);
    len = parse_hex (argv[3]);
    while (i < len) {
        u32 start, end, gap;

        if (a[i] == b[i]) {
            i++;
            continue;
        }
        start = i;
        end = i + 1;
        for (gap = 0, i++; i < len && gap < 16; i++) {
            if (a[i] != b[i]) {
                end = i + 1;
                gap = 0;
            } else {
                gap++;
            }
        }
        i = end;
        ranges++;
        total += end - start;
        if (ranges <= 40) {
            u32 n = end - start < 32 ? end - start : 32;

            printf ("diff 0x%06x-0x%06x (%d bytes)\n", start, end, end - start);
            dump ("now: ", a + start, n);
            dump ("was: ", b + start, n);
        }
    }
    printf ("%d ranges, %d bytes differ\n", ranges, total);
    return 0;
}
