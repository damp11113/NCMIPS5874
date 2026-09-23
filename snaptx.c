/*
 * snaptx: dump the HDMI transmitter's byte registers (U-Boot version).
 *   go ${a}
 */
#include "uboot.h"
#include "snaptx.h"

int main (int argc, char *argv[]) {
    printf ("=== SNAPTX BEGIN (u-boot) ===\n");
    snaptx_dump (printf);
    printf ("=== SNAPTX END ===\n");
    return 0;
}
