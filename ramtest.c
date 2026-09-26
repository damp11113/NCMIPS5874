/*
 * ramtest: test the upper 64 MB of DDR (physical 0x04000000-0x07ffffff).
 *
 *   go ${a} [passes]        passes in hex, default 1 (~6 s each)
 *
 * The satellite box has 128 MB (U-Boot ddrsize=128) but its stock
 * firmware uses only the low 64 MB. The low half (U-Boot, this program)
 * is not touched except one saved + restored word for the mirror check.
 *
 * All accesses are uncached (kseg1, 0xa4000000..), so every read comes
 * from the chip, not from the cache. Tests:
 *   0 mirror: is 0x04000000 just the low 64 MB again? (then stop)
 *   1 data bus: walking 1 / walking 0 at one address
 *   2 address bus: one word per address line, checks for shorts
 *   3 address in address, then its inverse
 *   4 pseudo-random fill + verify
 * Serial shows progress and the first errors; the front panel (if any)
 * shows the test number, then "0" + LED on for pass or "EEE" for fail.
 * A serial key stops between tests.
 */
#include "uboot.h"
#include "fd650.h"

#define LOW_WORD    0xa2000000u     /* free low RAM (phys 0x02000000) */
#define HI_BASE     0xa4000000u     /* phys 0x04000000, uncached */
#define HI_SIZE     0x04000000u     /* 64 MB */
#define MAX_REPORT  10

/* CP0 Count: 324 MHz on both boxes (cpuinfo); wraps after ~13 s, a pass
 * takes ~6 s. U-Boot's get_timer printed 0 ms here. */
#define COUNT_PER_MS 324000u

static inline u32 count(void) {
    u32 v;

    __asm__ volatile("mfc0 %0, $9" : "=r" (v));
    return v;
}

static u32 errors;
static int panel;

static void show(const char *s) {
    if (panel) {
        fd650_show(s);
    }
}

static void fail(u32 addr, u32 want, u32 got) {
    if (errors < MAX_REPORT) {
        printf("    ERROR at 0x%08x (phys 0x%08x): wrote %08x read %08x (diff %08x)\n",
                addr, addr & 0x1fffffff, want, got, want ^ got);
    }
    errors++;
}

/* 0: returns 1 if the upper half mirrors the lower half */
static int mirror_check(void) {
    volatile u32 *lo = (volatile u32 *) LOW_WORD;
    volatile u32 *hi = (volatile u32 *) (LOW_WORD + HI_SIZE);
    u32 saved = *lo;
    int mirrored;

    *lo = 0x11111111u;
    *hi = 0xeeeeeeeeu;
    mirrored = (*lo == 0xeeeeeeeeu);
    *lo = saved;
    return mirrored;
}

/* 1: walking bits on the data bus */
static void data_bus(void) {
    volatile u32 *p = (volatile u32 *) HI_BASE;
    int b;

    for (b = 0; b < 32; b++) {
        u32 v = 1u << b;

        *p = v;
        if (*p != v) {
            fail((u32) p, v, *p);
        }
        *p = ~v;
        if (*p != ~v) {
            fail((u32) p, ~v, *p);
        }
    }
}

/* 2: address lines 2..25 (word offsets 1 << n inside 64 MB) */
static void addr_bus(void) {
    volatile u32 *base = (volatile u32 *) HI_BASE;
    u32 off, off2;

    for (off = 4; off < HI_SIZE; off <<= 1) {
        base[off / 4] = 0xaaaaaaaau;
    }
    base[0] = 0x55555555u;
    for (off = 4; off < HI_SIZE; off <<= 1) {
        if (base[off / 4] != 0xaaaaaaaau) {
            fail((u32) &base[off / 4], 0xaaaaaaaau, base[off / 4]);
        }
    }
    base[0] = 0xaaaaaaaau;
    for (off = 4; off < HI_SIZE; off <<= 1) {
        base[off / 4] = 0x55555555u;
        if (base[0] != 0xaaaaaaaau) {
            fail((u32) base, 0xaaaaaaaau, base[0]);
        }
        for (off2 = 4; off2 < HI_SIZE; off2 <<= 1) {
            u32 want = off2 == off ? 0x55555555u : 0xaaaaaaaau;

            if (base[off2 / 4] != want) {
                fail((u32) &base[off2 / 4], want, base[off2 / 4]);
            }
        }
        base[off / 4] = 0xaaaaaaaau;
    }
}

/* 3: every word holds its own address (inv = 0) or the inverse */
static void addr_in_addr(u32 inv) {
    volatile u32 *p;
    volatile u32 *end = (volatile u32 *) (HI_BASE + HI_SIZE);

    for (p = (volatile u32 *) HI_BASE; p < end; p++) {
        *p = (u32) p ^ inv;
    }
    for (p = (volatile u32 *) HI_BASE; p < end; p++) {
        u32 want = (u32) p ^ inv;
        u32 got = *p;

        if (got != want) {
            fail((u32) p, want, got);
        }
    }
}

static u32 lfsr_next(u32 x) {
    /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* 4: pseudo-random data */
static void random_fill(u32 seed) {
    volatile u32 *p;
    volatile u32 *end = (volatile u32 *) (HI_BASE + HI_SIZE);
    u32 x = seed;

    for (p = (volatile u32 *) HI_BASE; p < end; p++) {
        x = lfsr_next(x);
        *p = x;
    }
    x = seed;
    for (p = (volatile u32 *) HI_BASE; p < end; p++) {
        u32 got = *p;

        x = lfsr_next(x);
        if (got != x) {
            fail((u32) p, x, got);
        }
    }
}

static int stop_requested(void) {
    if (tstc()) {
        (void) getc();
        printf("stopped\n");
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    u32 passes = argc > 1 ? parse_hex(argv[1]) : 1;
    u32 pass, t0;

    panel = fd650_init(0x200) == 0;
    if (panel) {
        fd650_led(0);
    }
    printf("ramtest: phys 0x04000000-0x07ffffff (64 MB, uncached), %u pass(es)\n", passes);

    show("0");
    if (mirror_check()) {
        printf("0 mirror: upper 64 MB mirrors the lower 64 MB -> only 64 MB usable\n");
        show("EEE");
        return 1;
    }
    printf("0 mirror: ok (upper half is separate memory)\n");

    for (pass = 0; pass < passes; pass++) {
        u32 before = errors;

        t0 = count();
        printf("pass %u\n", pass + 1);

        show("1");
        data_bus();
        printf("  1 data bus:        %u errors\n", errors - before);
        if (stop_requested()) {
            break;
        }

        show("2");
        before = errors;
        addr_bus();
        printf("  2 address bus:     %u errors\n", errors - before);
        if (stop_requested()) {
            break;
        }

        show("3");
        before = errors;
        addr_in_addr(0);
        addr_in_addr(0xffffffffu);
        printf("  3 address in addr: %u errors\n", errors - before);
        if (stop_requested()) {
            break;
        }

        show("4");
        before = errors;
        random_fill(0x12345678u + pass * 0x9e3779b9u);
        printf("  4 random:          %u errors\n", errors - before);
        printf("  pass time %u ms\n", (count() - t0) / COUNT_PER_MS);
        if (stop_requested()) {
            break;
        }
    }

    if (errors) {
        printf("ramtest: FAIL, %u errors\n", errors);
        show("EEE");
        return 1;
    }
    printf("ramtest: PASS, upper 64 MB usable\n");
    show("0");
    if (panel) {
        fd650_led(1);
    }
    return 0;
}
