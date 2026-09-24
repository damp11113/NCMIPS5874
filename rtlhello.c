/*
 * rtlhello: first contact with the internal WiFi chip (Realtek RTL8188FTV,
 * USB, module BL-M8188FU3 on root port 1) through U-Boot's USB stack.
 *
 *   fatload usb 0 ${a} rtlhello.bin     (while the stick is still visible)
 *   usb port 1
 *   usb reset                           (now only the WiFi chip is seen)
 *   usb tree
 *   go ${a}
 *
 * Prints chip ID/version (REG_SYS_CFG 0xf0), a register dump and the
 * decoded eFuse map. 8188F eFuse layout (Linux struct rtl8188fu_efuse):
 * VID/PID at 0xd0, MAC at 0xd7. Read-only apart from the eFuse power/clock
 * enable bits Linux also sets. Uses rtl8188.h.
 */
#include "uboot.h"
#include "rtl8188.h"

int main (int argc, char *argv[]) {
    u32 cfg, a, i;
    int used;

    if (rtl_open () < 0) {
        printf ("No Realtek (0bda:xxxx) device. Did you run 'usb port 1' + 'usb reset'?\n");
        return 1;
    }
    printf ("Realtek %04x:%04x, devnum %d, speed %d\n",
            UB_DEV_VID (rtl), UB_DEV_PID (rtl), UB_DEV_DEVNUM (rtl), UB_DEV_SPEED (rtl));

    cfg = rd32 (REG_SYS_CFG);
    printf ("REG_SYS_CFG (0xf0) = %08x  (chip version bits 12-15: %d)\n",
            cfg, (cfg >> 12) & 0xf);

    printf ("Registers 0x000-0x0ff:\n");
    for (a = 0; a < 0x100; a += 16) {
        printf ("R %03x: %08x %08x %08x %08x\n", a, rd32 (a), rd32 (a + 4),
                rd32 (a + 8), rd32 (a + 12));
    }

    used = rtl_read_efuse ();
    printf ("eFuse: %d raw bytes used\n", used);
    for (a = 0; a < RTL_EFUSE_LEN; a += 16) {
        int blank = 1;

        for (i = 0; i < 16; i++) {
            if (rtl_efuse[a + i] != 0xff) {
                blank = 0;
            }
        }
        if (blank) {
            continue;
        }
        printf ("E %03x:", a);
        for (i = 0; i < 16; i++) {
            printf (" %02x", rtl_efuse[a + i]);
        }
        printf ("\n");
    }
    printf ("eFuse VID/PID at 0xd0: %02x%02x:%02x%02x (USB says %04x:%04x)\n",
            rtl_efuse[0xd1], rtl_efuse[0xd0], rtl_efuse[0xd3], rtl_efuse[0xd2],
            UB_DEV_VID (rtl), UB_DEV_PID (rtl));
    printf ("MAC address (eFuse 0xd7): %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtl_efuse[0xd7], rtl_efuse[0xd8], rtl_efuse[0xd9], rtl_efuse[0xda],
            rtl_efuse[0xdb], rtl_efuse[0xdc]);
    return 0;
}
