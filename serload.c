/*
 * serload: receive a program over the serial console into RAM.
 *
 *   go 0x81f00000 <dest-hex> <size-hex>
 *
 * The vendor U-Boot's loady / loadb never write RAM on these boxes (they
 * go to a flash writer, see sat/NOTES.md), so SSerHial puts this loader at
 * 0x81f00000 with mw.l (uncached, 0xa1f00000) and starts it. It prints
 * "SERLOAD READY", reads exactly <size> raw bytes with U-Boot's getc, and
 * prints "SERLOAD OK <crc32>" (or "SERLOAD TIMEOUT <count>" after 3 s
 * without a byte), then returns to U-Boot; the host checks the CRC and
 * runs "go <dest>".
 *
 * Caches: stale dirty D-cache lines over <dest> are written back and
 * dropped first, the bytes go through the uncached alias (dest | 0xa0000000),
 * then the I-cache lines of <dest> are invalidated, so "go" runs exactly
 * what was received.
 */
#include "uboot.h"

#define LINE        32
#define TIMEOUT_MS  3000

static void cache_writeback_invalidate(u32 start, u32 len) {
    u32 a;

    for (a = start & ~(LINE - 1); a < start + len; a += LINE) {
        __asm__ volatile("cache 0x15, 0(%0)" : : "r" (a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
}

static void icache_invalidate(u32 start, u32 len) {
    u32 a;

    for (a = start & ~(LINE - 1); a < start + len; a += LINE) {
        __asm__ volatile("cache 0x10, 0(%0)" : : "r" (a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
}

static u32 crc32_update(u32 crc, unsigned char b) {
    int i;

    crc ^= b;
    for (i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return crc;
}

int main(int argc, char *argv[]) {
    u32 dest, size, i, crc = 0xffffffffu;
    volatile unsigned char *p;

    if (argc < 3) {
        printf("usage: go 0x81f00000 <dest-hex> <size-hex>\n");
        return 1;
    }
    dest = parse_hex(argv[1]);
    size = parse_hex(argv[2]);
    cache_writeback_invalidate(dest, size);
    p = (volatile unsigned char *) ((dest & 0x1fffffffu) | 0xa0000000u);

    printf("SERLOAD READY %08x %08x\n", dest, size);
    for (i = 0; i < size; i++) {
        unsigned long t0 = get_timer(0);
        unsigned char b;

        while (!tstc()) {
            if (get_timer(t0) > TIMEOUT_MS) {
                printf("SERLOAD TIMEOUT %08x\n", i);
                return 1;
            }
        }
        b = (unsigned char) getc();
        p[i] = b;
        crc = crc32_update(crc, b);
    }
    icache_invalidate(dest, size);
    printf("SERLOAD OK %08x\n", ~crc);
    return 0;
}
