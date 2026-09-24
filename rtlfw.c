/*
 * rtlfw: power on the internal RTL8188FTV and start its firmware.
 *
 * Load both files while the USB stick is still visible, then switch to
 * the WiFi port:
 *   fatload usb 0 ${a} rtlfw.bin
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize}
 *
 * Arguments (hex): firmware address, firmware size.
 */
#include "uboot.h"
#include "rtl8188.h"

int main (int argc, char *argv[]) {
    const unsigned char *fw;
    u32 size, t0;
    int r;

    if (argc < 3) {
        printf ("usage: go ${a} <fw-addr> <fw-size>   (hex, e.g. 82000000 ${filesize})\n");
        return 1;
    }
    fw = (const unsigned char *) parse_hex (argv[1]);
    size = parse_hex (argv[2]);

    printf ("firmware: %d bytes, signature %02x%02x, version %d.%d, %02d-%02d %02d:%02d\n",
            size, fw[1], fw[0], fw[4] | (fw[5] << 8), fw[6],
            fw[8], fw[9], fw[10], fw[11]);
    if ((fw[1] << 8 | (fw[0] & 0xf0)) != 0x88f0 || size < 1024 || size > 64 * 1024) {
        printf ("not an RTL8188F firmware (signature should be 88fX)\n");
        return 1;
    }

    if (rtl_open () < 0) {
        printf ("No Realtek chip. Run 'usb port 1' and 'usb reset' first.\n");
        return 1;
    }
    printf ("chip %04x:%04x, SYS_CFG %08x\n", UB_DEV_VID (rtl), UB_DEV_PID (rtl),
            rd32 (REG_SYS_CFG));

    r = rtl_read_efuse ();
    printf ("eFuse %d bytes, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", r,
            rtl_efuse[0xd7], rtl_efuse[0xd8], rtl_efuse[0xd9],
            rtl_efuse[0xda], rtl_efuse[0xdb], rtl_efuse[0xdc]);

    printf ("before: APS_FSMCO %08x CR %04x MCU_FW_DL %08x\n",
            rd32 (REG_APS_FSMCO), rd16 (REG_CR), rd32 (REG_MCU_FW_DL));
    r = rtl_power_on ();
    printf ("power on: %d  (APS_FSMCO %08x CR %04x)\n", r, rd32 (REG_APS_FSMCO), rd16 (REG_CR));
    if (r < 0) {
        return 1;
    }

    t0 = get_timer (0);
    r = rtl_download_firmware (fw, size);
    printf ("download: %d in %d ms  (MCU_FW_DL %08x)\n", r, get_timer (t0), rd32 (REG_MCU_FW_DL));
    if (r < 0) {
        return 1;
    }

    r = rtl_start_firmware ();
    printf ("start: %d  (MCU_FW_DL %08x)\n", r, rd32 (REG_MCU_FW_DL));
    if (r == 0) {
        printf ("FIRMWARE RUNNING (WINT_INIT_READY set)\n");
    } else if (r == -1) {
        printf ("checksum report never came: download went wrong\n");
    } else {
        printf ("checksum OK but firmware did not report ready\n");
    }
    return r ? 1 : 0;
}
