/*
 * tvapp: stand-alone TV demo, no stock firmware.
 *
 *   U-Boot: source avstart.scr (HDMI on), then  go ${a}
 *
 * Draws text, gradients and a bouncing box on the OSD. The power LED
 * blinks green/red. STANDBY or any serial key exits back to U-Boot.
 */
#include "board.h"
#include "osdsetup.h"

static const u16 bg_colour = RGB (10, 20, 60);

static void draw_static (struct fb *fb) {
    int i;

    fb_clear (fb, bg_colour);
    fb_text (fb, 40, 30, "My own TV app!", 5, WHITE, TRANSPARENT);
    fb_text (fb, 40, 120, "Booted from U-Boot, no stock firmware", 2, YELLOW, TRANSPARENT);
    fb_text (fb, 40, 170, "CPU    : MIPS 24KEc @ ~648 MHz", 2, CYAN, TRANSPARENT);
    fb_text (fb, 40, 205, "Screen : OSD layer 6, 1280x720 ARGB1555", 2, CYAN, TRANSPARENT);
    fb_text (fb, 40, 240, "Board  : PCB-CS8051M", 2, CYAN, TRANSPARENT);

    for (i = 0; i < 1200; i++) {
        int v = i * 247 / 1199;     /* 248+ in blue alone = colour key */
        fb_rect (fb, 40 + i, 290, 1, 20, RGB (v, 0, 0));
        fb_rect (fb, 40 + i, 310, 1, 20, RGB (0, v, 0));
        fb_rect (fb, 40 + i, 330, 1, 20, RGB (0, 0, v));
        fb_rect (fb, 40 + i, 350, 1, 20, RGB (v, v, v));
    }
    fb_text (fb, 40, 670, "Press STANDBY on the box (or a key on serial) to exit", 2, WHITE, TRANSPARENT);
}

int main (int argc, char *argv[]) {
    static const u16 colours[] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE };
    struct fb fb;
    int x = 60, y = 400, dx = 7, dy = 5, hits = 0, frame = 0;
    const int size = 80, top = 390, bottom = 650, left = 40, right = 1240;

    if (osd_setup (&fb) < 0) {
        printf ("Display not running: source avstart.scr first\n");
        return 1;
    }
    printf ("tvapp running. STANDBY or any serial key exits.\n");
    draw_static (&fb);

    while (!standby_pressed () && !tstc ()) {
        fb_rect (&fb, x, y, size, size, bg_colour);
        x += dx;
        y += dy;
        if (x < left || x + size > right) {
            dx = -dx;
            x += 2 * dx;
            hits++;
        }
        if (y < top || y + size > bottom) {
            dy = -dy;
            y += 2 * dy;
            hits++;
        }
        fb_rect (&fb, x, y, size, size, colours[hits % 7]);

        /* Power LED: swap green/red every ~0.5 s */
        frame++;
        led_green ((frame / 25) & 1);
        led_red (!((frame / 25) & 1));
        udelay (20000);
    }
    if (tstc ()) {
        getc ();
    }
    while (standby_pressed ()) {
        udelay (20000);
    }

    fb_clear (&fb, TRANSPARENT);
    led_red (0);
    led_green (1);
    printf ("tvapp done.\n");
    return 0;
}
