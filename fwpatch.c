/*
 * fwpatch: stop the stock firmware's internet update and ad download,
 * without touching flash. The boot script loads the stock firmware to
 * 0x80008000 (loadimg), then runs this (loaded to 0x82000000), then
 * 'go 0x80008000'. It changes one letter of the update folder and the ad
 * list file names in the RAM copy, so the servers answer "not found":
 *   Internet_update_IPTV  -> Xnternet_update_IPTV   (firmware update)
 *   ads.xml               -> xds.xml                (ad list)
 *   ads_iptv.m3u          -> xds_iptv.m3u           (ad list)
 * Channel lists (<vendor>_iptv.m3u, youtube_live.m3u, radio.m3u) on the same
 * servers keep working. Found by searching, so a different firmware
 * version is patched too (or left alone if the names are gone).
 *
 * Build: ./buildc.sh fwpatch.c fwpatch 0x82000000
 */
#include "uboot.h"

#define IMAGE_START     0x80008000u
#define IMAGE_END       0x80808000u     /* 8 MB, app_ram.bin is ~7.5 MB */

static const char *const names[] = { "Internet_update_IPTV", "ads.xml", "ads_iptv.m3u" };
static const char repl[] = { 'X', 'x', 'x' };

static int match (const unsigned char *p, const char *s) {
    while (*s) {
        if (*p++ != (unsigned char) *s++) {
            return 0;
        }
    }
    return 1;
}

int main (int argc, char *argv[]) {
    unsigned char *p;
    int i, total = 0;

    (void) argc;
    (void) argv;
    for (i = 0; i < 3; i++) {
        int n = 0;

        for (p = (unsigned char *) IMAGE_START; p < (unsigned char *) IMAGE_END - 32; p++) {
            if (*p == (unsigned char) names[i][0] && match (p, names[i])) {
                *p = repl[i];
                /* write the changed line back to RAM for the firmware */
                __asm__ volatile ("cache 0x15, 0(%0)" : : "r" (p) : "memory");
                n++;
            }
        }
        printf ("fwpatch: %-22s %d x\n", names[i], n);
        total += n;
    }
    __asm__ volatile ("sync" : : : "memory");
    printf ("fwpatch: %d patches (update + ad download off)\n", total);
    return 0;
}
