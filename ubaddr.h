/*
 * Addresses of internal U-Boot functions, per U-Boot build.
 *
 * The two boxes run different builds of U-Boot 2012.04, so functions that
 * are not in the export jump table sit at different link addresses. The
 * build is recognised by its version string; unknown builds get NULL and
 * callers fail with a message instead of jumping into random code.
 *
 * Satellite addresses found with sat/ubmatch.py (instruction shape match
 * against the IPTV build, 40/40 each). Struct offsets (usb_device,
 * block_dev_desc_t) are the same in both builds.
 *
 * Runtime address = link address + gd->reloc_off (gd in $k0, +0x14).
 */
#ifndef UBADDR_H
#define UBADDR_H

#include "uboot.h"

#define UB_BOX_IPTV     0           /* NC5874 IPTV box (M88CS8051B) */
#define UB_BOX_SAT      1           /* satellite box (M88CS8002B) */

struct ub_build {
    const char *name;
    u32 version_link;           /* link address of the version string */
    const char *version;
    u32 usb_get_dev_index;      /* (i) -> &usb_dev[i] or NULL */
    u32 usb_bulk_msg;           /* (dev, pipe, data, len, &actual, ms) */
    u32 usb_control_msg;        /* (dev, pipe, req, type, value, index, data, size, ms) */
    u32 usb_stor_get_dev;       /* (index) -> &usb_dev_desc[index] */
    u32 ehci_tmo;               /* ehci_submit_async: li s0,10000; li a0,5000 */
    u32 hdmi_set_mode;          /* HDMI dev ops[1]; 0 = not mapped for this build */
    u32 got_32736;              /* GOT entry -32736 (page used by set_mode) */
    int box;                    /* UB_BOX_IPTV / UB_BOX_SAT */
};

static const struct ub_build ub_builds[] = {
    { "IPTV box", 0x8017fc24u, "U-Boot 2012.04 (Nov 21 2022 - 10:42:10)",
      0x80122a78u, 0x80122cb4u, 0x80122d50u, 0x8012484cu, 0x80159c7cu,
      0x8014876cu, 0x80188af0u, UB_BOX_IPTV },
    /* set_mode found (0x80149054, GOT entry 0x80189620), but its data
     * offsets differ from the IPTV build: vicset / regapply stay IPTV-only */
    { "satellite box", 0x801805c8u, "U-Boot 2012.04 (May 09 2026 - 09:21:12)",
      0x80123360u, 0x8012359cu, 0x80123638u, 0x80125134u, 0x8015a564u,
      0, 0, UB_BOX_SAT },
};

static inline u32 ub_reloc_off(void) {
    u32 gd;

    __asm__ volatile("move %0, $26" : "=r" (gd));
    return REG32(gd + 0x14);
}

/* The running U-Boot build, or NULL if unknown */
static __attribute__((unused)) const struct ub_build *ub_build(void) {
    static const struct ub_build *found;
    u32 off, i;

    if (found) {
        return found;
    }
    off = ub_reloc_off();
    for (i = 0; i < sizeof(ub_builds) / sizeof(ub_builds[0]); i++) {
        const char *want = ub_builds[i].version;
        const char *have = (const char *) (ub_builds[i].version_link + off);

        while (*want && *want == *have) {
            want++;
            have++;
        }
        if (*want == 0) {
            found = &ub_builds[i];
            return found;
        }
    }
    return 0;
}

#endif
