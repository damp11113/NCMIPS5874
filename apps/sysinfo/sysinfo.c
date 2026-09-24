/*
 * System info + benchmarks for the NC5874 box (SDK app, NCAPPS/APPS/SYSINFO).
 *
 * Pages (LEFT / RIGHT): SYSTEM, CPU, DISPLAY+AUDIO, STORAGE, MEMORY, BENCH.
 * BENCH: OK runs the benchmarks (a few seconds each). EXIT quits.
 * Everything shown is also printed on the serial console.
 *
 * CPU clock is measured two ways: against U-Boot's millisecond timer (the
 * way cpuinfo did; known to be ~9 % off) and against the audio hardware,
 * which consumes samples at exactly 48 kHz from its own crystal.
 */
#define BOX_WANT_AUDIO
#include "sdk.h"
#include "strbuf.h"

#define BG          RGB (12, 18, 40)
#define PANEL       RGB (24, 34, 72)
#define HILITE      RGB (60, 110, 200)
#define LINE_H      34
#define BODY_Y      130
#define MAX_LINES   16

static struct fb fb;
static char lines[MAX_LINES][82];
static u16 line_color[MAX_LINES];
static int nlines;

static inline u32 count (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

#define mfc0(reg, sel) ({ u32 __v; \
    __asm__ volatile ("mfc0 %0, $" #reg ", " #sel : "=r" (__v)); __v; })

static void add (u16 color, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));

static void add (u16 color, const char *fmt, ...) {
    va_list ap;

    if (nlines == MAX_LINES) {
        return;
    }
    va_start (ap, fmt);
    vsnprintf (lines[nlines], sizeof (lines[0]), fmt, ap);
    va_end (ap);
    line_color[nlines] = color;
    printf ("%s\n", lines[nlines]);
    nlines++;
}

/* ---- measurements shared by pages ---- */

static u32 count_hz_timer, count_hz_audio, ccres = 2;

static void measure_clock_timer (void) {
    u32 t0, c0;

    t0 = ub_get_timer (0);
    while (ub_get_timer (0) == t0) {
    }
    t0 = ub_get_timer (0);
    c0 = count ();
    while (ub_get_timer (t0) < 500) {
    }
    count_hz_timer = (count () - c0) * 2;
}

/* Feed silence and time how fast the 48 kHz audio clock eats it */
static void measure_clock_audio (void) {
    static short zeros[2 * 512];
    u32 written = 0, q0, q1, w0, c0, c1, frames;

    audio_start ();
    while (written < 12000) {                       /* prime the ring */
        u32 n = audio_space ();

        n = n > 512 ? 512 : n;
        audio_write (zeros, n);
        written += n;
    }
    q0 = ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME;
    w0 = written;
    c0 = count ();
    while (count () - c0 < 200000000u) {            /* ~0.6 s */
        u32 n = audio_space ();

        if (n) {
            n = n > 512 ? 512 : n;
            audio_write (zeros, n);
            written += n;
        }
    }
    q1 = ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME;
    c1 = count ();
    audio_stop ();
    frames = (written - w0) - (q1 - q0);
    count_hz_audio = frames ? (u32) ((unsigned long long) (c1 - c0) * AUD_RATE / frames) : 0;
}

static const char *uboot_version (void) {
    static char v[64];
    const char *p;

    if (v[0]) {
        return v;
    }
    for (p = (const char *) 0x81300000; p < (const char *) 0x81500000; p++) {
        if (p[0] == 'U' && !memcmp (p, "U-Boot 20", 9)) {
            int i;

            for (i = 0; i < 63 && p[i] >= 32 && p[i] < 127; i++) {
                v[i] = p[i];
            }
            v[i] = 0;
            return v;
        }
    }
    strcpy (v, "U-Boot (version string not found)");
    return v;
}

/* ---- pages ---- */

static void page_system (void) {
    u32 prid = mfc0 (15, 0);

    add (YELLOW, "SYSTEM");
    add (WHITE, "Board      PCB-CS8051M set-top box (2022-10)");
    add (WHITE, "SoC        Montage M88CS8051B (NationalChip 5874 family)");
    add (WHITE, "CPU        MIPS 24KEc, PRId 0x%08x (rev %d.%d.%d)", prid,
         (prid >> 5) & 7, (prid >> 2) & 7, prid & 3);
    add (WHITE, "Clock      %d MHz (audio-referenced), %d MHz (U-Boot timer)",
         count_hz_audio / 1000000 * ccres, count_hz_timer / 1000000 * ccres);
    add (WHITE, "2nd core   AV core (video/audio firmware, avcpu.bin)");
    add (WHITE, "RAM        128 MB DDR");
    add (WHITE, "Flash      XMC XM25QH64A, 8 MB SPI NOR");
    add (WHITE, "Boot       %s", uboot_version ());
    add (WHITE, "WiFi       RTL8188FTV on USB port 1 (own driver: scan, TX)");
    add (WHITE, "I/O        HDMI, CVBS + stereo RCA, USB-A, IR, STANDBY, LED");
    add (GREY, "Runs from  NCAPPS launcher at 0x%08x, app at 0x%08x",
         SDK_LAUNCHER_ADDR, SDK_APP_ADDR);
}

static void cache_line (const char *name, u32 sets_f, u32 line_f, u32 assoc_f) {
    u32 line, sets, ways;

    if (line_f == 0) {
        add (WHITE, "%-10s none", name);
        return;
    }
    line = 2u << line_f;
    sets = (sets_f == 7) ? 32 : (64u << sets_f);
    ways = assoc_f + 1;
    add (WHITE, "%-10s %d KB, %d-way, %d-byte lines", name, sets * line * ways / 1024, ways, line);
}

static void page_cpu (void) {
    u32 c0 = mfc0 (16, 0), c1 = mfc0 (16, 1), c3 = mfc0 (16, 3);

    add (YELLOW, "CPU");
    add (WHITE, "ISA        MIPS32 release %d, %s endian", ((c0 >> 10) & 7) + 1,
         (c0 & (1u << 15)) ? "big" : "little");
    add (WHITE, "MMU        TLB, %d entries (4 KB .. 256 MB pages)", ((c1 >> 25) & 63) + 1);
    cache_line ("L1 I", (c1 >> 22) & 7, (c1 >> 19) & 7, (c1 >> 16) & 7);
    cache_line ("L1 D", (c1 >> 13) & 7, (c1 >> 10) & 7, (c1 >> 7) & 7);
    add (WHITE, "L2         %s", (c1 & (1u << 31)) ? "yes" : "none");
    add (WHITE, "FPU        %s", (c1 & 1) ? "yes" : "none (soft-float)");
    add (WHITE, "MIPS16e    %s    DSP ASE %s    MT %s", (c1 & 4) ? "yes" : "no",
         (c3 & (1u << 10)) ? "yes" : "no", (c3 & 4) ? "yes" : "no");
    add (WHITE, "Count      %d ticks/s, 1 tick = %d cycles", count_hz_audio, ccres);
    add (WHITE, "Status     0x%08x   EBase 0x%08x", mfc0 (12, 0), mfc0 (15, 1));
}

static void page_display (void) {
    u32 out = REG32 (0xbf4400b8), w = out & 0xffff, h = out >> 16;

    add (YELLOW, "DISPLAY + AUDIO");
    add (WHITE, "Output     %dx%d per field -> %s", w, h,
         h <= 576 ? "interlaced (1080i50)" : "progressive");
    add (WHITE, "OSD        layer 6, %dx%d ARGB1555, pitch %d px", fb.w, fb.h, fb.pitch);
    add (WHITE, "OSD RAM    0x%08x (uncached view)", (u32) fb.pix);
    add (WHITE, "Colour key 0x801f (pure blue shows as transparent)");
    add (WHITE, "Audio out  48000 Hz stereo 16-bit, HDMI (I2S SD0) + RCA");
    add (WHITE, "Audio ring %d KB at phys 0x%08x", AUD_BUF_SIZE / 1024, AUD_BUF_PHYS);
    add (WHITE, "Decoders   MP3 (Helix, software), OPL2 FM (software)");
    add (WHITE, "HW video   H.264 / HEVC on the AV core (not used yet)");
}

static void page_storage (void) {
    struct sdk_storage st;

    add (YELLOW, "STORAGE");
    if (sdk_storage_info (&st) < 0) {
        add (RED, "No USB storage (run 'usb start')");
        return;
    }
    add (WHITE, "USB stick  %s %s (rev %s)", st.vendor, st.product, st.revision);
    add (WHITE, "Size       %d MB (%d blocks of %d bytes)",
         (u32) ((unsigned long long) st.blocks * st.block_size >> 20), st.blocks, st.block_size);
    add (WHITE, "File sys   FAT%d, %d KB clusters", st.fat_bits, st.cluster_bytes / 1024);
    add (WHITE, "Driver     U-Boot EHCI + usbfat.h (read-only, folders, LFN)");
    add (WHITE, "App folder %s", sdk_app_dir[0] ? sdk_app_dir : "(stick root)");
    add (WHITE, "Data       %s", sdk_data_dir[0] ? sdk_data_dir : "(none)");
    add (WHITE, "USB read   %d KB so far", sdk_usb_bytes () / 1024);
}

extern size_t heap_in_use, heap_total;
extern char __bss_end[];

static void page_memory (void) {
    add (YELLOW, "MEMORY MAP (physical)");
    add (WHITE, "00008000   app image + bss   (this app ends at 0x%08x)", (u32) __bss_end);
    add (WHITE, "00800000   launcher");
    add (WHITE, "00f00000   U-Boot stack, data, code (to ~01580000)");
    add (WHITE, "01600000   heap region 1, 45 MB");
    add (WHITE, "04400000   OSD plane (1.8 MB), 045d0000 audio buffers");
    add (WHITE, "0469dc00   AV core video buffers + firmware");
    if (sdk_bigmem_bytes) {
        add (GREEN, "           big memory: %d MB of AV video memory used as heap",
             sdk_bigmem_bytes >> 20);
    }
    add (WHITE, "07e10000   AV core code");
    add (CYAN, "heap total %d MB, %d KB in use (app area rest included)",
         (u32) (heap_total >> 20), (u32) heap_in_use / 1024);
    add (GREY, "kseg0 0x80000000 = cached, kseg1 0xa0000000 = uncached");
}

/* ---- benchmarks ---- */

static u32 ms_of (u32 ticks) {
    return (u32) ((unsigned long long) ticks * 1000 / count_hz_audio);
}

static u32 mb_s (u32 bytes, u32 ticks) {
    u32 ms = ms_of (ticks);

    return ms ? (u32) ((unsigned long long) bytes * 1000 / ms >> 20) : 0;
}

static void bench (void) {
    u32 t0, t1, i, n, sum = 0;
    unsigned char *buf = malloc (4 << 20);
    volatile u32 *p;

    nlines = 0;
    add (YELLOW, "BENCHMARKS (clock %d MHz)", count_hz_audio / 1000000 * ccres);
    if (!buf) {
        add (RED, "no memory");
        return;
    }

    n = 50000000;
    t0 = count ();
    __asm__ volatile (".set push\n\t.set noreorder\n1:\n\tbnez %0, 1b\n\t"
                      "addiu %0, %0, -1\n\t.set pop" : "+r" (n));
    t1 = count ();
    add (WHITE, "Integer    %d MIPS (branch loop, 100 M instructions)",
         (u32) (100000ull * count_hz_audio / (t1 - t0) / 1000));

    {
        u32 a = 3, b = 5, c = 7, d = 11;

        t0 = count ();
        for (i = 0; i < 10000000; i++) {
            __asm__ volatile ("mul %0,%0,%4\n\tmul %1,%1,%4\n\tmul %2,%2,%4\n\tmul %3,%3,%4"
                              : "+r" (a), "+r" (b), "+r" (c), "+r" (d) : "r" (i | 1));
        }
        t1 = count ();
        add (WHITE, "Multiply   %d M mul/s", 40000 / (ms_of (t1 - t0) ? ms_of (t1 - t0) : 1));
        sum += a ^ b ^ c ^ d;
    }

    t0 = count ();
    memset (buf, 0x55, 4 << 20);
    t1 = count ();
    add (WHITE, "memset     %d MB/s (4 MB, cached)", mb_s (4 << 20, t1 - t0));
    t0 = count ();
    memcpy (buf + (2 << 20), buf, 2 << 20);
    t1 = count ();
    add (WHITE, "memcpy     %d MB/s (2 MB)", mb_s (2 << 20, t1 - t0));

    p = (volatile u32 *) buf;
    t0 = count ();
    for (i = 0; i < (1 << 20); i += 4) {
        sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
    }
    t1 = count ();
    add (WHITE, "RAM read   %d MB/s cached", mb_s (4 << 20, t1 - t0));
    p = (volatile u32 *) (0x20000000u | (u32) buf);         /* kseg1 alias */
    t0 = count ();
    for (i = 0; i < (256 << 10) / 4; i += 4) {
        sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
    }
    t1 = count ();
    add (WHITE, "RAM read   %d MB/s uncached", mb_s (256 << 10, t1 - t0));

    {
        volatile float fa = 1.0001f, fb = 0.9999f, fr = 1;
        volatile double da = 1.000001, dr = 1;

        t0 = count ();
        for (i = 0; i < 200000; i++) {
            fr = fr * fa + fb;
        }
        t1 = count ();
        add (WHITE, "Float      %d k flop/s (soft-float single)", 200000 / (ms_of (t1 - t0) ? ms_of (t1 - t0) : 1));
        t0 = count ();
        for (i = 0; i < 100000; i++) {
            dr = dr * da;
        }
        t1 = count ();
        add (WHITE, "Double     %d k flop/s (soft-float)", 100000 / (ms_of (t1 - t0) ? ms_of (t1 - t0) : 1));
        sum += (u32) fr + (u32) dr;
    }

    t0 = count ();
    for (i = 0; i < 10; i++) {
        fb_rect (&fb, 0, 0, fb.w, fb.h, (i & 1) ? BG : PANEL);
    }
    t1 = count ();
    add (WHITE, "OSD fill   %d full screens/s (%d MB/s uncached)",
         10000 / (ms_of (t1 - t0) ? ms_of (t1 - t0) : 1),
         mb_s (10 * fb.w * fb.h * 2, t1 - t0));

    {
        /* Biggest file among the app folders */
        static const char *tries[] = {
            "/NCAPPS/APPS/DOOM/DOOM2.WAD", "/NCAPPS/APPS/DOOM/DOOM.WAD",
            "/NCAPPS/APPS/BADAPPLE/BADAPPLE.BAV", "/NCAPPS/LAUNCHER.BIN",
        };
        int h = -1;
        u32 k, got = 0;

        for (k = 0; k < sizeof (tries) / sizeof (tries[0]) && h < 0; k++) {
            h = sdk_open (tries[k]);
        }
        if (h >= 0) {
            t0 = count ();
            while (got < (2 << 20)) {
                long r = sdk_read (h, buf, 256 << 10);

                if (r <= 0) {
                    break;
                }
                got += r;
            }
            t1 = count ();
            sdk_close (h);
            add (WHITE, "USB read   %d KB/s (%d KB of %s)",
                 ms_of (t1 - t0) ? got / ms_of (t1 - t0) * 1000 / 1024 : 0, got / 1024,
                 tries[k - 1] + 13);
        }
    }
    add (GREY, "(checksum %08x)", sum);
    free (buf);
}

/* ---- screen ---- */

static const char *tabs[] = { "SYSTEM", "CPU", "DISPLAY", "STORAGE", "MEMORY", "BENCH" };
#define NTABS 6

static void draw (int page) {
    int i, x = 40;

    fb_clear (&fb, BG);
    fb_rect (&fb, 0, 0, fb.w, 100, PANEL);
    fb_text (&fb, 40, 16, "SYSTEM INFO", 2, GREY, TRANSPARENT);
    for (i = 0; i < NTABS; i++) {
        int w = strlen (tabs[i]) * 16 + 24;

        if (i == page) {
            fb_rect (&fb, x, 52, w, 40, HILITE);
        }
        fb_text (&fb, x + 12, 56, tabs[i], 2, i == page ? WHITE : GREY, TRANSPARENT);
        x += w + 12;
    }
    for (i = 0; i < nlines; i++) {
        fb_text (&fb, 40, BODY_Y + i * LINE_H, lines[i], 2, line_color[i], TRANSPARENT);
    }
    fb_rect (&fb, 0, 680, fb.w, 40, PANEL);
    fb_text (&fb, 40, 688, page == NTABS - 1 ? "LEFT/RIGHT page   OK run benchmarks   EXIT quit" :
             "LEFT/RIGHT page   EXIT quit", 2, GREY, TRANSPARENT);
}

static void build (int page) {
    nlines = 0;
    printf ("\n");
    switch (page) {
    case 0: page_system (); break;
    case 1: page_cpu (); break;
    case 2: page_display (); break;
    case 3: page_storage (); break;
    case 4: page_memory (); break;
    default:
        add (YELLOW, "BENCHMARKS");
        add (WHITE, "Press OK to run (about 10 s): integer, multiply, memory,");
        add (WHITE, "soft-float, OSD fill rate, USB read speed.");
        break;
    }
}

int main (int argc, char *argv[]) {
    struct sdk_key k;
    int page = 0;

    (void) argc;
    (void) argv;
    if (osd_setup (&fb) < 0) {
        printf ("sysinfo: display not running\n");
        return 1;
    }
    fb_clear (&fb, BG);
    fb_text (&fb, 40, 340, "Measuring the clock...", 3, WHITE, TRANSPARENT);
    measure_clock_timer ();
    measure_clock_audio ();
    printf ("sysinfo: Count %d Hz (audio), %d Hz (U-Boot timer)\n", count_hz_audio,
            count_hz_timer);

    build (page);
    draw (page);
    for (;;) {
        if (!sdk_key_poll (&k) || k.btn == BTN_NONE) {
            sdk_idle (2000);
            continue;
        }
        if (k.btn == BTN_RIGHT || k.btn == BTN_LEFT) {
            page = (page + (k.btn == BTN_RIGHT ? 1 : NTABS - 1)) % NTABS;
            build (page);
            draw (page);
        } else if (k.btn == BTN_OK && page == NTABS - 1 && !k.repeat) {
            fb_text (&fb, 40, 640, "Running...", 2, YELLOW, TRANSPARENT);
            bench ();
            draw (page);
        } else if (k.btn == BTN_BACK || k.btn == BTN_POWER || k.btn == BTN_HOME) {
            break;
        }
    }
    fb_clear (&fb, TRANSPARENT);
    return 0;
}
