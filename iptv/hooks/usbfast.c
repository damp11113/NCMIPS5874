/*
 * usbfast: make U-Boot's 'usb start' / 'usb reset' try once instead of 4
 * times when no USB storage device answers. On the WiFi port (usb port 1)
 * there never is one, so every switch to the WiFi chip ran 4 x 2
 * controller inits and ended with the usage text.
 *
 * The loop in U-Boot's do_usb (link 0x8011a878: li s2,4 = attempts, retry
 * while usb_stor_scan returned -1) is patched in RAM to li s2,1, only if
 * the instruction is the expected one. Lasts until the next reset.
 *
 * Build: ./buildc.sh usbfast.c usbfast 0x83c00000
 *   fatload usb 0 83c00000 usbfast.bin; go 83c00000
 */
#include "uboot.h"

#define USB_TRIES_LINK  0x8011a878u
#define LI_S2_4         0x24120004u
#define LI_S2_1         0x24120001u

static u32 reloc_off(void) {
    u32 gd;

    __asm__ volatile("move %0, $26" : "=r" (gd));
    return REG32(gd + 0x14);
}

int main(int argc, char *argv[]) {
    volatile u32 *p = (volatile u32 *) (USB_TRIES_LINK + reloc_off());

    (void) argc;
    (void) argv;
    if (*p == LI_S2_1) {
        printf("usbfast: already patched\n");
        return 0;
    }
    if (*p != LI_S2_4) {
        printf("usbfast: unexpected instruction %08x at %08x, not patched\n", *p, (u32) p);
        return 1;
    }
    *p = LI_S2_1;
    __asm__ volatile("cache 0x15, 0(%0)\n\tsync\n\tcache 0x10, 0(%0)\n\tsync" : : "r" (p) : "memory");
    printf("usbfast: usb start/reset now try once (%08x)\n", (u32) p);
    return 0;
}
