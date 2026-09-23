/*
 * osdinit: test OSD layer 6 setup from plain U-Boot (no stock firmware).
 *
 *   source avstart.scr first (HDMI on via U-Boot's own av_launch), then:
 *   go ${a}
 *
 * Test screen: white frame on the 1280x720 edges, 8 colour bars and a
 * 100 x 100 square that must look square when the scaling is right.
 */
#include "uboot.h"
#include "osdsetup.h"

int main (int argc, char *argv[]) {
    static const u16 c[8] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE, GREY };
    struct fb fb;
    int i;

    if (osd_setup (&fb) < 0) {
        printf ("Display not running: source avstart.scr first\n");
        return 1;
    }
    printf ("OSD on: output %dx%d per field, scale h 0x%x v 0x%x\n",
            REG32 (0xbf4400b8) & 0xffff, REG32 (0xbf4400b8) >> 16,
            REG32 (0xbf440114), REG32 (0xbf44012c));

    fb_clear (&fb, RGB (40, 0, 60));
    fb_rect (&fb, 0, 0, fb.w, 8, WHITE);
    fb_rect (&fb, 0, fb.h - 8, fb.w, 8, WHITE);
    fb_rect (&fb, 0, 0, 8, fb.h, WHITE);
    fb_rect (&fb, fb.w - 8, 0, 8, fb.h, WHITE);
    for (i = 0; i < 8; i++) {
        fb_rect (&fb, 40 + i * 150, 300, 140, 200, c[i]);
    }
    fb_rect (&fb, 1100, 40, 100, 100, WHITE);
    fb_text (&fb, 40, 60, "OSD from plain U-Boot!", 5, WHITE, TRANSPARENT);
    fb_text (&fb, 40, 200, "No stock firmware running", 3, YELLOW, TRANSPARENT);
    fb_text (&fb, 40, 560, "White frame = full 1280x720 area", 2, WHITE, TRANSPARENT);
    fb_text (&fb, 1080, 150, "square?", 2, WHITE, TRANSPARENT);
    printf ("Test screen drawn. Reboot to undo.\n");
    return 0;
}
