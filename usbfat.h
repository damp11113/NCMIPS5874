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
 * Paths: folders separated by '/', long (LFN) or 8.3 names, case-insensitive.
 *
 *   if (ufs_mount () < 0) ...
 *   struct ufile f;
 *   if (ufs_open (&f, "NCAPPS/APPS/DOOM/APP.BIN") < 0) ...
 *   ufs_seek (&f, pos); n = ufs_read (&f, buf, len);
 *
 *   struct udir d; struct udirent e;         list a folder ("" = root)
 *   if (ufs_opendir (&d, "NCAPPS/APPS") == 0)
 *       while (ufs_readdir (&d, &e)) ... e.name, e.is_dir, e.size
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
void *memset (void *dst, int c, unsigned int n);

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

/* Recently used folders, see ufs_lookup () */
#define UFS_DCACHE 8
static struct { char path[128]; u32 clus; } ufs_dcache[UFS_DCACHE];
static int ufs_dcache_next;

static inline u32 ufs_count (void) {
    u32 v = 0;

#ifdef __mips__
    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
#endif
    return v;
}

static inline u32 ufs_le16 (const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static inline u32 ufs_le32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

/*
 * Stick pulled out: U-Boot's EHCI driver would wait ~5 s per transfer (and
 * retry), so every read looked like a freeze. The USB-A socket's EHCI port
 * status (PORTSC, bit 0 = device connected) is checked first and reads fail
 * at once without it. A stick behind a hub keeps the port connected, so
 * any failed read also marks the stick as lost. Callers check ufs_lost.
 */
#define UFS_PORTSC_USB_A    0xbf060084u     /* U-Boot's auto_detect_usb_port reads it */

static int ufs_lost;

static inline int ufs_stick_present (void) {
#ifdef UFS_NO_PORT_CHECK
    return 1;                               /* PC tests on disk images */
#else
    return (*(volatile u32 *) UFS_PORTSC_USB_A) & 1;
#endif
}

/* Raw sector read through U-Boot. Returns 0 ok, -1 error. */
static int ufs_sectors (u32 start, u32 count, void *buf) {
    u32 t0 = ufs_count (), got;

    if (ufs_lost || !ufs_stick_present ()) {
        ufs_lost = 1;
        return -1;
    }
    ub_target = ufs_read_fn;
    got = ((ub_blk_read_t) (void *) ub_thunk) (*(int *) ((char *) ufs_dev + 4), start, count, buf);
    ufs_ticks += ufs_count () - t0;
    ufs_bytes += got * UFS_SECTOR;
    if (got != count) {
        ufs_lost = 1;
        return -1;
    }
    return 0;
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

/*
 * U-Boot's EHCI driver waits 10 s for a bulk transfer (5 s for others)
 * before giving up (ehci_submit_async, link 0x80159c7c: li s0,10000 /
 * li a0,5000; movn picks 5000 for non-bulk pipes). A stick pulled out in
 * the middle of a read therefore froze the box for 10 s per try. A 32 KB
 * read takes < 0.1 s, so the constants are patched in U-Boot's code in RAM
 * to 1.5 s / 1 s, only if the instructions are the expected ones.
 */
#define UFS_EHCI_TMO_LINK   0x80159c7cu

static void ufs_patch_ehci_timeouts (void) {
#ifndef UFS_NO_PORT_CHECK
    volatile u32 *p = (volatile u32 *) (UFS_EHCI_TMO_LINK + ub_reloc_off ());
    u32 a;

    if (p[0] == 0x24102710u && p[1] == 0x24041388u) {     /* li s0,10000; li a0,5000 */
        p[0] = 0x24100000u | 1500;
        p[1] = 0x24040000u | 1000;
        for (a = (u32) p & ~31u; a < (u32) (p + 2); a += 32) {
            __asm__ volatile ("cache 0x15, 0(%0)\n\tcache 0x10, 0(%0)" : : "r" (a) : "memory");
        }
        __asm__ volatile ("sync" : : : "memory");
    }
#endif
}

/* Returns 0, or -1 (message printed) */
static int ufs_mount (void) {
    u32 part = 0, rsvd, nfats, rootents, fatsz, totsec, clusters, i;
    const unsigned char *b = ufs_sec;

    ufs_patch_ehci_timeouts ();
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
    memset (ufs_dcache, 0, sizeof (ufs_dcache));
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

static int ufs_name_eq (const char *a, const char *b, int blen) {
    int i;

    for (i = 0; i < blen; i++) {
        if (!a[i] || ufs_lower (a[i]) != ufs_lower (b[i])) {
            return 0;
        }
    }
    return a[i] == 0;
}

/* ---- directories ---- */

#define UFS_ATTR_DIR    0x10
#define UFS_NAME_MAX    256

/* Folders are read up to 16 sectors (8 KB) per USB command: every U-Boot
 * read costs a few ms, so sector-by-sector folder walks were slow. */
#define UFS_DIR_SECTORS 16

struct udir {
    u32 clus;                       /* current cluster (0 = FAT16 root region) */
    u32 sec_idx;                    /* next sector to load within cluster / root region */
    u32 ent, nent;                  /* entry in buf, entries loaded (nent = load more) */
    int done;
    unsigned char buf[UFS_DIR_SECTORS * UFS_SECTOR] __attribute__ ((aligned (64)));
};

struct udirent {
    char name[UFS_NAME_MAX];        /* long name if present, else 8.3 */
    u32 attr, size, first;
    int is_dir;
};

static void ufs_dir_start (struct udir *d, u32 first_clus) {
    d->clus = first_clus;
    d->sec_idx = 0;
    d->ent = d->nent = 0;
    d->done = 0;
}

/* Next raw 32-byte entry, or NULL at the end of the directory */
static const unsigned char *ufs_dir_raw (struct udir *d) {
    if (d->done) {
        return 0;
    }
    if (d->ent == d->nent) {
        u32 sec, n;

        if (d->clus == 0) {                         /* FAT16 root region */
            if (d->sec_idx == ufs_root_sectors) {
                d->done = 1;
                return 0;
            }
            sec = ufs_root_start + d->sec_idx;
            n = ufs_root_sectors - d->sec_idx;
        } else {
            if (d->sec_idx == ufs_spc) {
                d->clus = ufs_fat_next (d->clus);
                d->sec_idx = 0;
            }
            if (ufs_eoc (d->clus)) {
                d->done = 1;
                return 0;
            }
            sec = ufs_clus_sector (d->clus) + d->sec_idx;
            n = ufs_spc - d->sec_idx;
        }
        if (n > UFS_DIR_SECTORS) {
            n = UFS_DIR_SECTORS;
        }
        d->sec_idx += n;
        if (ufs_sectors (sec, n, d->buf) < 0) {
            d->done = 1;
            return 0;
        }
        d->ent = 0;
        d->nent = n * (UFS_SECTOR / 32);
    }
    return d->buf + 32 * d->ent++;
}

/* Next file or folder (skips ".", "..", deleted entries, volume labels).
 * Returns 1 and fills *e, or 0 at the end. */
static int ufs_readdir (struct udir *d, struct udirent *e) {
    static const unsigned char lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    const unsigned char *r;
    int lfn_ok = 0;

    while ((r = ufs_dir_raw (d))) {
        int k, n;

        if (r[0] == 0) {
            d->done = 1;                            /* end marker */
            return 0;
        }
        if (r[0] == 0xe5) {
            lfn_ok = 0;
            continue;
        }
        if (r[11] == 0x0f) {                        /* long name part */
            int seq = (r[0] & 0x1f) - 1;

            if (seq < 0 || seq >= 20) {
                lfn_ok = 0;
                continue;
            }
            if (r[0] & 0x40) {
                int end = (seq + 1) * 13;

                e->name[end < UFS_NAME_MAX - 1 ? end : UFS_NAME_MAX - 1] = 0;
                lfn_ok = 1;
            }
            for (k = 0; k < 13 && seq * 13 + k < UFS_NAME_MAX - 1; k++) {
                u32 ch = ufs_le16 (r + lfn_pos[k]);

                if (ch == 0) {
                    e->name[seq * 13 + k] = 0;
                    break;
                }
                e->name[seq * 13 + k] = (ch < 128) ? ch : '?';
            }
            continue;
        }
        if (r[11] & 0x08) {                         /* volume label */
            lfn_ok = 0;
            continue;
        }
        if (r[0] == '.' && (r[1] == ' ' || (r[1] == '.' && r[2] == ' '))) {
            lfn_ok = 0;
            continue;                               /* . and .. */
        }
        if (!lfn_ok) {
            for (k = 0, n = 0; k < 8 && r[k] != ' '; k++) {
                e->name[n++] = (k == 0 && r[0] == 0x05) ? 0xe5 : r[k];
            }
            if (r[8] != ' ') {
                e->name[n++] = '.';
                for (k = 8; k < 11 && r[k] != ' '; k++) {
                    e->name[n++] = r[k];
                }
            }
            e->name[n] = 0;
        }
        e->attr = r[11];
        e->is_dir = (r[11] & UFS_ATTR_DIR) != 0;
        e->size = ufs_le32 (r + 28);
        e->first = (ufs_fat32 ? ufs_le16 (r + 20) << 16 : 0) | ufs_le16 (r + 26);
        return 1;
    }
    return 0;
}

/* Walk a path like "NCAPPS/APPS/DOOM/APP.BIN" ('/' separated, leading
 * '/' optional, case-insensitive). The last component may be a file or a
 * folder. Returns 0 and fills *e ("" or "/" = the root folder), or -1. */
/* Recently used folders (path -> first cluster), so looking up many files
 * in the same folder does not walk from the root each time. The stick is
 * read-only here, so entries never go stale while mounted. */

static void ufs_dcache_put (const char *path, int len, u32 clus) {
    int i;

    if (len <= 0 || len >= (int) sizeof (ufs_dcache[0].path)) {
        return;
    }
    for (i = 0; i < UFS_DCACHE; i++) {
        if (ufs_name_eq (ufs_dcache[i].path, path, len)) {
            return;
        }
    }
    memcpy (ufs_dcache[ufs_dcache_next].path, path, len);
    ufs_dcache[ufs_dcache_next].path[len] = 0;
    ufs_dcache[ufs_dcache_next].clus = clus;
    ufs_dcache_next = (ufs_dcache_next + 1) % UFS_DCACHE;
}

/* Longest cached folder that is a whole-component prefix of path */
static int ufs_dcache_get (const char *path, u32 *clus) {
    int i, best = 0;

    for (i = 0; i < UFS_DCACHE; i++) {
        int len = 0;

        while (ufs_dcache[i].path[len]) {
            len++;
        }
        if (len > best && (path[len] == '/' || path[len] == 0) &&
            ufs_name_eq (ufs_dcache[i].path, path, len)) {
            best = len;
            *clus = ufs_dcache[i].clus;
        }
    }
    return best;
}

static int ufs_lookup (const char *path, struct udirent *e) {
    static struct udir d;
    u32 clus = ufs_fat32 ? ufs_root_clus : 0;
    const char *full;
    int skip;

    while (*path == '/') {
        path++;
    }
    full = path;
    e->name[0] = 0;
    e->is_dir = 1;
    e->first = clus;
    e->size = 0;
    e->attr = UFS_ATTR_DIR;
    skip = ufs_dcache_get (path, &clus);
    if (skip) {
        e->first = clus;
        path += skip;
        while (*path == '/') {
            path++;
        }
    }
    while (*path) {
        const char *end = path;
        int len, found = 0;

        while (*end && *end != '/') {
            end++;
        }
        len = end - path;
        if (!e->is_dir) {
            return -1;                              /* file used as a folder */
        }
        ufs_dir_start (&d, clus);
        while (ufs_readdir (&d, e)) {
            if (ufs_name_eq (e->name, path, len)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return -1;
        }
        clus = e->first;
        if (e->is_dir && clus == 0) {
            clus = ufs_fat32 ? ufs_root_clus : 0;   /* ".." style link to root */
        }
        if (e->is_dir) {
            ufs_dcache_put (full, end - full, clus);
        }
        path = end;
        while (*path == '/') {
            path++;
        }
    }
    return 0;
}

/* Open a folder for ufs_readdir (). Returns 0, or -1. */
__attribute__ ((unused))
static int ufs_opendir (struct udir *d, const char *path) {
    struct udirent e;

    if (ufs_lookup (path, &e) < 0 || !e.is_dir) {
        return -1;
    }
    ufs_dir_start (d, e.first == 0 && ufs_fat32 ? ufs_root_clus : e.first);
    return 0;
}

/* Open a file by path. Returns 0, or -1 if not found or a folder. */
static int ufs_open (struct ufile *f, const char *path) {
    struct udirent e;

    if (ufs_lookup (path, &e) < 0 || e.is_dir) {
        return -1;
    }
    f->first = e.first;
    f->size = e.size;
    f->pos = 0;
    f->clus = f->first;
    f->clus_idx = 0;
    f->cstart = 0;
    f->clen = 0;
    return 0;
}

/*
 * The only write: replace the first sector of an existing file in place
 * (settings files). The FAT, folders and file size are never touched, so
 * the file system cannot be damaged, only this one sector. Safety checks:
 * the file must be exactly one sector long, the sector on the stick must
 * start with magic (so a wrong sector number is never written), and the
 * sector is read back and compared after the write. Uses U-Boot's
 * usb_stor_write (block_dev_desc_t +100). data = 512 bytes.
 * Returns 0, or -1 (message printed).
 */
__attribute__ ((unused))
static int ufs_overwrite (const char *path, const void *data, const char *magic) {
    typedef u32 (*ub_blk_write_t) (int dev, u32 start, u32 blkcnt, const void *buffer);
    struct udirent e;
    u32 sec, write_fn, got;
    int mlen = 0;

    while (magic[mlen]) {
        mlen++;
    }
    if (ufs_lookup (path, &e) < 0 || e.is_dir) {
        printf ("usbfat: %s not found\n", path);
        return -1;
    }
    if (e.size != UFS_SECTOR || ufs_eoc (e.first)) {
        printf ("usbfat: %s must be %d bytes (is %d)\n", path, UFS_SECTOR, e.size);
        return -1;
    }
    if (memcmp (data, magic, mlen)) {
        printf ("usbfat: new data for %s does not start with the magic\n", path);
        return -1;
    }
    write_fn = *(u32 *) ((char *) ufs_dev + 100);
    if (write_fn < 0x80000000u || write_fn >= 0x82000000u) {
        printf ("usbfat: U-Boot has no USB write (0x%08x)\n", write_fn);
        return -1;
    }
    sec = ufs_clus_sector (e.first);
    if (ufs_sectors (sec, 1, ufs_sec) < 0 || memcmp (ufs_sec, magic, mlen)) {
        printf ("usbfat: sector %d of %s does not hold the expected data, not writing\n", sec,
                path);
        return -1;
    }
    memcpy (ufs_sec, data, UFS_SECTOR);
#ifdef __mips__
    {
        u32 a;

        /* DMA reads RAM: write the cached copy back first */
        for (a = (u32) ufs_sec; a < (u32) ufs_sec + UFS_SECTOR; a += 32) {
            __asm__ volatile ("cache 0x15, 0(%0)" : : "r" (a) : "memory");
        }
        __asm__ volatile ("sync" : : : "memory");
    }
#endif
    ub_target = write_fn;
    got = ((ub_blk_write_t) (void *) ub_thunk) (*(int *) ((char *) ufs_dev + 4), sec, 1,
                                                ufs_sec);
    memset (ufs_sec, 0, UFS_SECTOR);
    if (got != 1 || ufs_sectors (sec, 1, ufs_sec) < 0 || memcmp (ufs_sec, data, UFS_SECTOR)) {
        printf ("usbfat: writing %s (sector %d) failed\n", path, sec);
        return -1;
    }
    return 0;
}

#endif
