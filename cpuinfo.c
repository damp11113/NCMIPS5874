/*
 * cpuinfo: identify the CPU core and benchmark it.
 *
 *   go ${a}
 *
 * Reads the MIPS32 CP0 identification registers (PRId, Config0-3),
 * measures the clock with the CP0 Count register, then times integer,
 * multiply and memory loops. Uses RAM at 0x82000000 (32 MB into DDR,
 * above U-Boot's 20 MB area) as a scratch buffer.
 */
#include "uboot.h"

#define read_c0(reg, sel) ({ u32 __v; \
    __asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r" (__v)); __v; })

#define BUF_CACHED   0x82000000u
#define BUF_UNCACHED 0xa2000000u

static u32 count_hz;        /* CP0 Count ticks per second */
static u32 ccres;           /* CPU cycles per Count tick */

static inline u32 count(void) {
    return read_c0(9, 0);
}

static u32 read_ccres(void) {
    u32 v;
    __asm__ volatile("rdhwr %0, $3" : "=r" (v));
    return v;
}

/* Count ticks -> microseconds */
static u32 ticks_to_us(u32 ticks) {
    return ticks / (count_hz / 1000000);
}

/* Count ticks -> CPU cycles */
static u32 ticks_to_cycles(u32 ticks) {
    return ticks * ccres;
}

static void print_cache(const char *name, u32 sets_f, u32 line_f, u32 assoc_f) {
    u32 line, sets, ways;

    if (line_f == 0) {
        printf("  %s: none\n", name);
        return;
    }
    line = 2u << line_f;
    sets = (sets_f == 7) ? 32 : (64u << sets_f);
    ways = assoc_f + 1;
    printf("  %s: %d KB, %d-way, %d-byte lines, %d sets\n",
            name, sets * line * ways / 1024, ways, line, sets);
}

static const char *core_name(u32 imp) {
    switch (imp) {
    case 0x93: return "24K";
    case 0x95: return "34K";
    case 0x96: return "24KE";
    case 0x97: return "74K";
    case 0x99: return "1004K";
    case 0x9a: return "1074K";
    default:   return "unknown";
    }
}

static void show_cpu(void) {
    u32 prid = read_c0(15, 0);
    u32 c0 = read_c0(16, 0);
    u32 c1 = read_c0(16, 1);
    u32 c2 = read_c0(16, 2);
    u32 c3 = read_c0(16, 3);
    static const char *kseg0_mode[] = {
        "write-through no-alloc", "write-through alloc", "uncached", "write-back",
        "?", "?", "?", "uncached accelerated",
    };

    printf("=== CPU identification ===\n");
    printf("  PRId    0x%08x: company %d (%s), core 0x%02x = MIPS %s, rev %d.%d.%d\n",
            prid, (prid >> 16) & 0xff, ((prid >> 16) & 0xff) == 1 ? "MIPS Technologies" : "other",
            (prid >> 8) & 0xff, core_name((prid >> 8) & 0xff),
            (prid >> 5) & 7, (prid >> 2) & 7, prid & 3);
    printf("  Config0 0x%08x Config1 0x%08x Config2 0x%08x Config3 0x%08x\n", c0, c1, c2, c3);
    printf("  ISA: MIPS32 release %d, %s endian, MMU type %d\n",
            ((c0 >> 10) & 7) + 1, (c0 & (1u << 15)) ? "big" : "little", (c0 >> 7) & 7);
    printf("  kseg0 (0x80000000) caching: %s\n", kseg0_mode[c0 & 7]);
    printf("  TLB entries: %d\n", ((c1 >> 25) & 0x3f) + 1);

    printf("=== Caches ===\n");
    print_cache("L1 I-cache", (c1 >> 22) & 7, (c1 >> 19) & 7, (c1 >> 16) & 7);
    print_cache("L1 D-cache", (c1 >> 13) & 7, (c1 >> 10) & 7, (c1 >> 7) & 7);
    if (c1 & (1u << 31)) {
        print_cache("L2 cache  ", (c2 >> 8) & 15, (c2 >> 4) & 15, c2 & 15);
    }

    printf("=== Features ===\n");
    printf("  FPU (hardware float) : %s\n", (c1 & 1) ? "yes" : "no");
    printf("  MIPS16e (compact ISA): %s\n", (c1 & 4) ? "yes" : "no");
    printf("  EJTAG debug          : %s\n", (c1 & 2) ? "yes" : "no");
    printf("  Perf counters        : %s\n", (c1 & 16) ? "yes" : "no");
    printf("  DSP ASE              : %s%s\n", (c3 & (1u << 10)) ? "yes" : "no",
            (c3 & (1u << 11)) ? " (rev 2)" : "");
    printf("  MT ASE (multithread) : %s\n", (c3 & 4) ? "yes" : "no");
    printf("  Vectored interrupts  : %s\n", (c3 & 32) ? "yes" : "no");
}

static void measure_clock(void) {
    u32 t0, c_start, c_end;

    ccres = read_ccres();
    if (ccres == 0) {
        ccres = 2;
    }

    /* Align to a millisecond edge, then count ticks over 1000 ms */
    t0 = get_timer(0);
    while (get_timer(0) == t0) {
    }
    t0 = get_timer(0);
    c_start = count();
    while (get_timer(t0) < 1000) {
    }
    c_end = count();

    count_hz = c_end - c_start;
    printf("=== Clock ===\n");
    printf("  Count register: %d ticks/s, 1 tick = %d CPU cycles\n", count_hz, ccres);
    printf("  CPU clock: about %d MHz\n", (count_hz / 1000000) * ccres);
}

static void bench_loop(void) {
    u32 n = 50000000, t0, t1, cyc;

    /* 2 instructions per iteration: branch + decrement in delay slot */
    t0 = count();
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n"
        "1:\n\t"
        "bnez %0, 1b\n\t"
        "addiu %0, %0, -1\n\t"
        ".set pop"
        : "+r" (n));
    t1 = count();

    cyc = ticks_to_cycles(t1 - t0);
    printf("  Branch loop : 100M instructions in %d ms -> %d MIPS, %d.%02d cycles/instr\n",
            ticks_to_us(t1 - t0) / 1000,
            100000000 / ticks_to_us(t1 - t0),
            cyc / 100000000, (cyc % 100000000) / 1000000);
}

static void bench_mul(void) {
    u32 i, a = 3, b = 5, c = 7, d = 11, t0, t1;
    const u32 n = 10000000;

    /* 4 independent multiplies per iteration */
    t0 = count();
    for (i = 0; i < n; i++) {
        __asm__ volatile(
            "mul %0, %0, %4\n\t"
            "mul %1, %1, %4\n\t"
            "mul %2, %2, %4\n\t"
            "mul %3, %3, %4"
            : "+r" (a), "+r" (b), "+r" (c), "+r" (d)
            : "r" (i | 1));
    }
    t1 = count();

    printf("  Multiply    : 40M mul in %d ms -> %d M mul/s  (checksum %08x)\n",
            ticks_to_us(t1 - t0) / 1000,
            40000 / (ticks_to_us(t1 - t0) / 1000), a ^ b ^ c ^ d);
}

static void bench_div(void) {
    u32 i, acc = 0xffffffff, t0, t1;
    const u32 n = 2000000;

    t0 = count();
    for (i = 1; i <= n; i++) {
        acc += 0xfffffff0u / i;
    }
    t1 = count();

    printf("  Divide      : 2M div in %d ms -> about %d cycles each (checksum %08x)\n",
            ticks_to_us(t1 - t0) / 1000, ticks_to_cycles(t1 - t0) / n, acc);
}

static u32 mem_read(u32 base, u32 bytes, u32 reps) {
    volatile u32 *p;
    u32 r, i, sum = 0, words = bytes / 4, t0;

    t0 = count();
    for (r = 0; r < reps; r++) {
        p = (volatile u32 *) base;
        for (i = 0; i < words; i += 4) {
            sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
        }
    }
    __asm__ volatile("" : : "r" (sum));
    return count() - t0;
}

static u32 mem_write(u32 base, u32 bytes, u32 reps) {
    volatile u32 *p;
    u32 r, i, words = bytes / 4, t0;

    t0 = count();
    for (r = 0; r < reps; r++) {
        p = (volatile u32 *) base;
        for (i = 0; i < words; i += 4) {
            p[i] = i;
            p[i + 1] = i;
            p[i + 2] = i;
            p[i + 3] = i;
        }
    }
    return count() - t0;
}

/* MB/s (1 MB = 1,000,000 bytes): bytes per microsecond */
static u32 mbps(u32 bytes, u32 reps, u32 ticks) {
    u32 us = ticks_to_us(ticks);

    return us ? bytes * reps / us : 0;
}

static void bench_mem(void) {
    struct {
        const char *name;
        u32 base, bytes, reps;
    } t[] = {
        { "8 KB, cached (fits L1)  ", BUF_CACHED,   8 * 1024,        2000 },
        { "4 MB, cached (DDR)      ", BUF_CACHED,   4 * 1024 * 1024, 4 },
        { "1 MB, uncached (DDR raw)", BUF_UNCACHED, 1 * 1024 * 1024, 2 },
    };
    int i;

    printf("=== Memory bandwidth (scratch RAM 0x%08x) ===\n", BUF_CACHED);
    for (i = 0; i < 3; i++) {
        u32 w = mem_write(t[i].base, t[i].bytes, t[i].reps);
        u32 r = mem_read(t[i].base, t[i].bytes, t[i].reps);

        printf("  %s  read %4d MB/s   write %4d MB/s\n", t[i].name,
                mbps(t[i].bytes, t[i].reps, r), mbps(t[i].bytes, t[i].reps, w));
    }
}

int main(int argc, char *argv[]) {
    show_cpu();
    measure_clock();

    printf("=== CPU benchmarks ===\n");
    bench_loop();
    bench_mul();
    bench_div();
    bench_mem();

    printf("Done.\n");
    return 0;
}
