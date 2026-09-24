/*
 * usbfat: read files from the USB stick while a program runs, through
 * U-Boot's USB mass-storage driver (needs 'usb start' before 'go').
 *
 * U-Boot 2012.04 can only fatload a whole file, so this is a small
 * read-only FAT16/FAT32 reader on top of U-Boot's raw sector read:
 *   usb_stor_get_dev (index)  link 0x8012484c, returns &usb_dev_desc[index]
 *   block_dev_desc_t (112 B, no LBA48): dev +4, type +11 (0xff = none),
 *   lba +16, blksz +20, vendor +24, product +65,
 *   block_read (dev, start, blkcnt, buffer) +96 (runtime address, set by
 *   usb_stor_scan; returns blocks read).
 * Partition: MBR first FAT partition, or no partition table.
 * Files: root directory only, long (LFN) or 8.3 names, case-insensitive.
 *
 *   if (ufs_mount () < 0) ...
 *   struct ufile f;
 *   if (ufs_open (&f, "badapple.bav") < 0) ...
 *   ufs_seek (&f, pos); n = ufs_read (&f, buf, len);
 *
 * Reads go through a 32 KB, 64-byte aligned buffer per file (whole
 * sectors, neighbouring clusters merged), so callers can use any length
 * and alignment. U-Boot's EHCI transfers block the CPU while they run:
 * ufs_ticks (CP0 Count) and ufs_bytes count the time and data.
 */
#ifndef USBFAT_H
#define USBFAT_H

#include "ubusb.h"

int memcmp (const void *a, const void *b, unsigned int n);   /* libc.c */
void *memcpy (void *dst, const void *src, unsigned int n);

#define UB_USB_STOR_GET_DEV     0x8012484c

#define UFS_SECTOR              512
#define UFS_CHUNK_SECTORS       64                      /* 32 KB per read */
#define UFS_CHUNK               (UFS_CHUNK_SECTORS * UFS_SECTOR)

struct ufile {
    u32 size, pos;
    u32 first, clus, clus_idx;      /* first cluster; cluster holding clus_idx */
    u32 cstart, clen;               /* file bytes held in cbuf */
    unsigned char cbuf[UFS_CHUNK] __attribute__ ((aligned (64)));
};

typedef u32 (*ub_blk_read_t) (int dev, u32 start, u32 blkcnt, void *buffer);

static void *ufs_dev;
static u32 ufs_read_fn;
static int ufs_fat32;
static u32 ufs_spc, ufs_fat_start, ufs_root_start, ufs_root_sectors, ufs_data_start;
static u32 ufs_root_clus, ufs_clus_bytes;
static u32 ufs_fat_cached = 0xffffffffu;
static unsigned char ufs_sec[UFS_SECTOR] __attribute__ ((aligned (64)));
static unsigned char ufs_fatsec[UFS_SECTOR] __attribute__ ((aligned (64)));
static u32 ufs_ticks, ufs_bytes;

static inline u32 ufs_count (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

static inline u32 ufs_le16 (const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static inline u32 ufs_le32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

/* Raw sector read through U-Boot. Returns 0 ok, -1 error. */
static int ufs_sectors (u32 start, u32 count, void *buf) {
    u32 t0 = ufs_count (), got;

    ub_target = ufs_read_fn;
    got = ((ub_blk_read_t) (void *) ub_thunk) (*(int *) ((char *) ufs_dev + 4), start, count, buf);
    ufs_ticks += ufs_count () - t0;
    ufs_bytes += got * UFS_SECTOR;
    return got == count ? 0 : -1;
}

static u32 ufs_fat_next (u32 c) {
    u32 off = ufs_fat32 ? c * 4 : c * 2;
    u32 sec = ufs_fat_start + off / UFS_SECTOR;
    u32 v;

    if (sec != ufs_fat_cached) {
        if (ufs_sectors (sec, 1, ufs_fatsec) < 0) {
            return 0x0fffffffu;
        }
        ufs_fat_cached = sec;
    }
    off %= UFS_SECTOR;
    if (ufs_fat32) {
        v = ufs_le32 (ufs_fatsec + off) & 0x0fffffffu;
        return v >= 0x0ffffff8u ? 0x0fffffffu : v;
    }
    v = ufs_le16 (ufs_fatsec + off);
    return v >= 0xfff8 ? 0x0fffffffu : v;
}

static inline int ufs_eoc (u32 c) {
    return c < 2 || c >= 0x0ffffff0u;
}

static inline u32 ufs_clus_sector (u32 c) {
    return ufs_data_start + (c - 2) * ufs_spc;
}

/* Returns 0, or -1 (message printed) */
static int ufs_mount (void) {
    u32 part = 0, rsvd, nfats, rootents, fatsz, totsec, clusters, i;
    const unsigned char *b = ufs_sec;

    ub_target = UB_USB_STOR_GET_DEV + ub_reloc_off ();
    ufs_dev = ((ub_get_dev_t) (void *) ub_thunk) (0);
    if (!ufs_dev || *((unsigned char *) ufs_dev + 11) == 0xff) {
        printf ("usbfat: no USB storage device (run 'usb start' first)\n");
        return -1;
    }
    if (*(u32 *) ((char *) ufs_dev + 20) != UFS_SECTOR) {
        printf ("usbfat: block size %d not supported\n", *(u32 *) ((char *) ufs_dev + 20));
        return -1;
    }
    ufs_read_fn = *(u32 *) ((char *) ufs_dev + 96);

    if (ufs_sectors (0, 1, ufs_sec) < 0 || b[510] != 0x55 || b[511] != 0xaa) {
        printf ("usbfat: cannot read sector 0\n");
        return -1;
    }
    /* No partition table if sector 0 already is a FAT boot sector */
    if (!((b[0] == 0xeb || b[0] == 0xe9) && ufs_le16 (b + 11) == UFS_SECTOR &&
          (!memcmp (b + 0x36, "FAT", 3) || !memcmp (b + 0x52, "FAT", 3)))) {
        for (i = 0; i < 4; i++) {
            const unsigned char *e = b + 0x1be + 16 * i;
            u32 t = e[4];

            if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b || t == 0x0c || t == 0x0e) {
                part = ufs_le32 (e + 8);
                break;
            }
        }
        if (i == 4 || ufs_sectors (part, 1, ufs_sec) < 0) {
            printf ("usbfat: no FAT partition\n");
            return -1;
        }
    }
    if (ufs_le16 (b + 11) != UFS_SECTOR || b[13] == 0) {
        printf ("usbfat: bad FAT boot sector at %d\n", part);
        return -1;
    }
    ufs_spc = b[13];
    rsvd = ufs_le16 (b + 14);
    nfats = b[16];
    rootents = ufs_le16 (b + 17);
    totsec = ufs_le16 (b + 19) ? ufs_le16 (b + 19) : ufs_le32 (b + 32);
    fatsz = ufs_le16 (b + 22) ? ufs_le16 (b + 22) : ufs_le32 (b + 36);
    ufs_root_clus = ufs_le32 (b + 44);

    ufs_fat_start = part + rsvd;
    ufs_root_start = ufs_fat_start + nfats * fatsz;
    ufs_root_sectors = (rootents * 32 + UFS_SECTOR - 1) / UFS_SECTOR;
    ufs_data_start = ufs_root_start + ufs_root_sectors;
    clusters = (totsec - (ufs_data_start - part)) / ufs_spc;
    ufs_clus_bytes = ufs_spc * UFS_SECTOR;
    if (clusters < 4085) {
        printf ("usbfat: FAT12 not supported\n");
        return -1;
    }
    ufs_fat32 = clusters >= 65525;
    printf ("usbfat: FAT%d at sector %d, %d KB clusters, %s\n", ufs_fat32 ? 32 : 16, part,
            ufs_clus_bytes / 1024, (char *) ufs_dev + 65);
    return 0;
}

/* Load the chunk holding f->pos into f->cbuf. Returns 0, or -1. */
static int ufs_load (struct ufile *f) {
    u32 idx = f->pos / ufs_clus_bytes;
    u32 sec_in = (f->pos % ufs_clus_bytes) / UFS_SECTOR;
    u32 count, need, c;

    if (idx < f->clus_idx) {
        f->clus = f->first;
        f->clus_idx = 0;
    }
    while (f->clus_idx < idx) {
        f->clus = ufs_fat_next (f->clus);
        f->clus_idx++;
        if (ufs_eoc (f->clus)) {
            return -1;
        }
    }
    if (ufs_eoc (f->clus)) {
        return -1;
    }

    /* Rest of this cluster, plus following clusters if contiguous */
    count = ufs_spc - sec_in;
    for (c = f->clus; count < UFS_CHUNK_SECTORS; c++) {
        if (ufs_fat_next (c) != c + 1) {
            break;
        }
        count += ufs_spc;
    }
    if (count > UFS_CHUNK_SECTORS) {
        count = UFS_CHUNK_SECTORS;
    }
    need = (f->size - (f->pos & ~(UFS_SECTOR - 1)) + UFS_SECTOR - 1) / UFS_SECTOR;
    if (count > need) {
        count = need;
    }
    if (ufs_sectors (ufs_clus_sector (f->clus) + sec_in, count, f->cbuf) < 0) {
        return -1;
    }
    f->cstart = f->pos & ~(UFS_SECTOR - 1);
    f->clen = count * UFS_SECTOR;
    return 0;
}

__attribute__ ((unused))
static void ufs_seek (struct ufile *f, u32 pos) {
    f->pos = pos < f->size ? pos : f->size;
}

/* Returns bytes read (0 at end of file or on error) */
static u32 ufs_read (struct ufile *f, void *dst, u32 len) {
    unsigned char *d = dst;
    u32 done = 0;

    if (len > f->size - f->pos) {
        len = f->size - f->pos;
    }
    while (done < len) {
        u32 n;

        if (f->pos < f->cstart || f->pos >= f->cstart + f->clen) {
            if (ufs_load (f) < 0) {
                break;
            }
        }
        n = f->cstart + f->clen - f->pos;
        if (n > len - done) {
            n = len - done;
        }
        memcpy (d + done, f->cbuf + (f->pos - f->cstart), n);
        f->pos += n;
        done += n;
    }
    return done;
}

static int ufs_lower (int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ufs_name_eq (const char *a, const char *b) {
    while (*a && ufs_lower (*a) == ufs_lower (*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Find a file in the root directory. Returns 0, or -1 if not found. */
static int ufs_open (struct ufile *f, const char *name) {
    static const unsigned char lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    char lfn[256], sfn[13];
    int lfn_ok = 0;
    u32 sec_idx = 0, clus = ufs_root_clus;

    for (;;) {
        u32 sec, i;

        if (ufs_fat32) {
            if (sec_idx == ufs_spc) {
                clus = ufs_fat_next (clus);
                sec_idx = 0;
            }
            if (ufs_eoc (clus)) {
                return -1;
            }
            sec = ufs_clus_sector (clus) + sec_idx;
        } else {
            if (sec_idx == ufs_root_sectors) {
                return -1;
            }
            sec = ufs_root_start + sec_idx;
        }
        sec_idx++;
        if (ufs_sectors (sec, 1, ufs_sec) < 0) {
            return -1;
        }

        for (i = 0; i < UFS_SECTOR; i += 32) {
            const unsigned char *e = ufs_sec + i;
            int k, n;

            if (e[0] == 0) {
                return -1;                              /* end of directory */
            }
            if (e[0] == 0xe5) {
                lfn_ok = 0;
                continue;
            }
            if (e[11] == 0x0f) {                        /* long name part */
                int seq = (e[0] & 0x1f) - 1;

                if (e[0] & 0x40) {
                    lfn[(seq + 1) * 13 < 255 ? (seq + 1) * 13 : 255] = 0;
                    lfn_ok = 1;
                }
                for (k = 0; k < 13 && seq * 13 + k < 255; k++) {
                    u32 ch = ufs_le16 (e + lfn_pos[k]);

                    if (ch == 0) {
                        lfn[seq * 13 + k] = 0;
                        break;
                    }
                    lfn[seq * 13 + k] = (ch < 128) ? ch : '?';
                }
                continue;
            }
            if (e[11] & 0x18) {                         /* volume label / dir */
                lfn_ok = 0;
                continue;
            }

            for (k = 0, n = 0; k < 8 && e[k] != ' '; k++) {
                sfn[n++] = e[k];
            }
            if (e[8] != ' ') {
                sfn[n++] = '.';
                for (k = 8; k < 11 && e[k] != ' '; k++) {
                    sfn[n++] = e[k];
                }
            }
            sfn[n] = 0;

            if ((lfn_ok && ufs_name_eq (lfn, name)) || ufs_name_eq (sfn, name)) {
                f->first = (ufs_fat32 ? ufs_le16 (e + 20) << 16 : 0) | ufs_le16 (e + 26);
                f->size = ufs_le32 (e + 28);
                f->pos = 0;
                f->clus = f->first;
                f->clus_idx = 0;
                f->cstart = 0;
                f->clen = 0;
                return 0;
            }
            lfn_ok = 0;
        }
    }
}

#endif
