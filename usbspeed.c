/*
 * usbspeed: why is reading the USB stick only ~0.8 MB/s? Compares
 *   A: U-Boot's usb_stor read (block_dev_desc_t +96), what usbfat uses,
 *      at several sector counts per call
 *   B: own Bulk-Only Transport: CBW + READ(10) + data + CSW sent with
 *      U-Boot's usb_bulk_msg directly (no usb_storage.c in between), with
 *      different command sizes and data transfer sizes
 * and checks that B returns the same data as A. Read-only.
 *
 * In U-Boot (stick in the box):
 *   usb start
 *   fatload usb 0 ${a} usbspeed.bin
 *   go ${a}
 */
#include "uboot.h"
#include "ubusb.h"

int memcmp(const void *a, const void *b, unsigned int n);
void *memset(void *dst, int c, unsigned int n);

#define TEST_LBA                200000u         /* somewhere in the data area */
#define TEST_BYTES              (2u << 20)      /* 2 MB per test */
#define TICKS_PER_MS            324000u

typedef u32(*ub_blk_read_t) (int dev, u32 start, u32 blkcnt, void *buffer);

static unsigned char buf[256 * 1024] __attribute__((aligned(4096)));
static unsigned char ref[64 * 1024] __attribute__((aligned(4096)));
static unsigned char cbw[32] __attribute__((aligned(64)));
static unsigned char csw[64] __attribute__((aligned(64)));
static unsigned char desc[256] __attribute__((aligned(64)));

static void *stor_desc;             /* U-Boot block_dev_desc_t */
static void *msd;                   /* usb_device of the stick */
static u32 ep_in, ep_out, tag = 1;
static int n_bulk;

static inline u32 count(void) {
    u32 v;

    __asm__ volatile("mfc0 %0, $9" : "=r" (v));
    return v;
}

static u32 uboot_read(u32 lba, u32 n, void *dst) {
    ub_target = *(u32 *) ((char *) stor_desc + 96);
    return ((ub_blk_read_t) (void *) ub_thunk) (*(int *) ((char *) stor_desc + 4), lba, n, dst);
}

/* Find the first device with a mass-storage bulk-only interface */
static int find_msd(void) {
    int i;

    for (i = 0; i < UB_USB_MAX_DEVICE; i++) {
        void *dev = ub_usb_dev(i);
        int len, p, in = 0, out = 0, is_msd = 0;

        if (!dev) {
            continue;
        }
        /* GET_DESCRIPTOR (configuration) */
        len = ub_control(dev, 6, 0x80, 0x0200, 0, desc, sizeof(desc), 1000);
        if (len < 9) {
            continue;
        }
        for (p = 0; p + 2 <= len && desc[p] >= 2; p += desc[p]) {
            if (desc[p + 1] == 4) {                                     /* interface */
                is_msd = desc[p + 5] == 8 && desc[p + 7] == 0x50;
            } else if (desc[p + 1] == 5 && is_msd && (desc[p + 3] & 3) == 2) {   /* bulk ep */
                if (desc[p + 2] & 0x80) {
                    in = desc[p + 2];
                } else {
                    out = desc[p + 2];
                }
            }
        }
        if (in && out) {
            msd = dev;
            ep_in = in;
            ep_out = out;
            printf("stick: usb_dev[%d] %04x:%04x speed %d, bulk in 0x%02x out 0x%02x\n", i,
                    UB_DEV_VID(dev), UB_DEV_PID(dev), UB_DEV_SPEED(dev), in, out);
            return 0;
        }
    }
    return -1;
}

static void put32(unsigned char *p, u32 v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* One READ(10) command: n sectors, data moved in pieces of chunk bytes */
static int bot_read(u32 lba, u32 n, void *dst, u32 chunk) {
    u32 len = n * 512, done = 0;
    int actual;

    memset(cbw, 0, 31);
    put32(cbw, 0x43425355);
    put32(cbw + 4, tag);
    put32(cbw + 8, len);
    cbw[12] = 0x80;
    cbw[14] = 10;
    cbw[15] = 0x28;
    cbw[17] = lba >> 24;
    cbw[18] = lba >> 16;
    cbw[19] = lba >> 8;
    cbw[20] = lba;
    cbw[22] = n >> 8;
    cbw[23] = n;
    n_bulk++;
    if (ub_bulk(msd, ep_out, cbw, 31, &actual, 2000) < 0) {
        printf("  CBW failed\n");
        return -1;
    }
    while (done < len) {
        u32 k = len - done < chunk ? len - done : chunk;

        n_bulk++;
        if (ub_bulk(msd, ep_in, (char *) dst + done, k, &actual, 2000) < 0 || actual != (int) k) {
            printf("  data failed at %d (actual %d)\n", done, actual);
            return -1;
        }
        done += k;
    }
    n_bulk++;
    if (ub_bulk(msd, ep_in, csw, 13, &actual, 2000) < 0 || actual != 13 ||
        csw[0] != 0x55 || csw[1] != 0x53 || csw[12] != 0 ||
        (csw[4] | csw[5] << 8 | csw[6] << 16 | (u32) csw[7] << 24) != tag) {
        printf("  bad CSW (actual %d, status %d)\n", actual, csw[12]);
        return -1;
    }
    tag++;
    return 0;
}

static void report(u32 ticks) {
    u32 ms = ticks / TICKS_PER_MS;

    printf("  %5d ms  %5d KB/s\n", ms, ms ? (TEST_BYTES / 1024) * 1000 / ms : 0);
}

int main(int argc, char *argv[]) {
    static const u32 a_counts[] = { 64, 256 };
    static const u32 b_cmd[] = { 64, 128, 256, 256, 512 };        /* sectors per READ(10) */
    static const u32 b_chunk[] = { 16384, 16384, 16384, 20480, 16384 };
    u32 t0, i, j, lba;

    (void) argc;
    (void) argv;
    if (!ub_build()) {
        printf("unknown U-Boot build (addresses in ubaddr.h)\n");
        return 1;
    }
    printf("U-Boot build: %s\n", ub_build()->name);
    ub_target = ub_build()->usb_stor_get_dev + ub_reloc_off();
    stor_desc = ((ub_get_dev_t) (void *) ub_thunk) (0);
    if (!stor_desc || *((unsigned char *) stor_desc + 11) == 0xff) {
        printf("no USB storage (run usb start)\n");
        return 1;
    }
    printf("U-Boot read fn 0x%08x\n", *(u32 *) ((char *) stor_desc + 96));
    if (find_msd() < 0) {
        printf("no bulk-only mass storage device found\n");
        return 1;
    }

    /* A: U-Boot usb_stor read */
    for (i = 0; i < 2; i++) {
        u32 n = a_counts[i];

        t0 = count();
        for (lba = TEST_LBA; lba < TEST_LBA + TEST_BYTES / 512; lba += n) {
            if (uboot_read(lba, n, buf) != n) {
                printf("U-Boot read failed at %d\n", lba);
                return 1;
            }
        }
        j = count() - t0;
        printf("A U-Boot read, %3d sectors per call:          ", n);
        report(j);
    }

    /* Reference data for the check */
    if (uboot_read(TEST_LBA + 4096, sizeof(ref) / 512, ref) != sizeof(ref) / 512) {
        printf("reference read failed\n");
        return 1;
    }
    memset(buf, 0xa5, sizeof(ref));
    if (bot_read(TEST_LBA + 4096, sizeof(ref) / 512, buf, 16384) < 0) {
        printf("own READ(10) failed - stopping (usb reset may be needed)\n");
        return 1;
    }
    printf("own READ(10) data %s U-Boot's\n", memcmp(buf, ref, sizeof(ref)) ? "DIFFERS from" :
            "same as");

    /* B: own bulk-only reads */
    for (i = 0; i < sizeof(b_cmd) / sizeof(b_cmd[0]); i++) {
        n_bulk = 0;
        t0 = count();
        for (lba = TEST_LBA; lba < TEST_LBA + TEST_BYTES / 512; lba += b_cmd[i]) {
            if (bot_read(lba, b_cmd[i], buf, b_chunk[i]) < 0) {
                printf("stopped\n");
                return 1;
            }
        }
        j = count() - t0;
        printf("B own, %3d KB per command, %2d KB transfers:", b_cmd[i] / 2, b_chunk[i] / 1024);
        report(j);
        printf("    %d bulk transfers, %d us each\n", n_bulk, j / (TICKS_PER_MS / 1000) / n_bulk);
    }
    return 0;
}
