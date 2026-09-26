/*
 * hook_demo: first real graphics on the TV from our own code.
 * Runs inside the original firmware (hook_entry.S + hookpatch), draws text
 * and a bouncing box on OSD layer 6 until STANDBY is pressed, then clears
 * the layer and returns so the firmware shows its normal menu.
 */
#include "osd.h"

typedef int(*printf_t) (const char *fmt, ...);

#define REG32(addr)     (*(volatile u32 *) (addr))
#define GPIO0_IN        0xbf0a0008
#define BTN_STANDBY     (1u << 11)      /* reads 0 while pressed */
#define COUNT_HZ        324000000u      /* CP0 Count rate (cpuinfo) */

static u32 read_count(void) {
    u32 v;
    __asm__ volatile("mfc0 %0, $9" : "=r" (v));
    return v;
}

static void wait_ms(u32 ms) {
    u32 t0 = read_count();
    while (read_count() - t0 < ms * (COUNT_HZ / 1000)) {
    }
}

static int standby_pressed(void) {
    return (REG32(GPIO0_IN) & BTN_STANDBY) == 0;
}

static void draw_static(struct fb *fb) {
    int i;

    fb_clear(fb, RGB(10, 20, 60));

    fb_text(fb, 40, 30, "Hello from bare-metal MIPS!", 4, WHITE, TRANSPARENT);
    fb_text(fb, 40, 110, "Drawn by my own C code on a Nationalchip 5874 set-top box", 2, YELLOW, TRANSPARENT);

    fb_text(fb, 40, 170, "CPU    : MIPS 24KEc @ ~648 MHz, 16K I$ + 16K D$", 2, CYAN, TRANSPARENT);
    fb_text(fb, 40, 205, "Screen : OSD layer 6, 1280x720 ARGB1555", 2, CYAN, TRANSPARENT);
    fb_text(fb, 40, 240, "Board  : PCB-CS8051M", 2, CYAN, TRANSPARENT);

    /* Colour gradient strip: red, green, blue ramps */
    for (i = 0; i < 1200; i++) {
        int v = i * 255 / 1199;
        fb_rect(fb, 40 + i, 290, 1, 20, RGB(v, 0, 0));
        fb_rect(fb, 40 + i, 310, 1, 20, RGB(0, v, 0));
        fb_rect(fb, 40 + i, 330, 1, 20, RGB(0, 0, v));
        fb_rect(fb, 40 + i, 350, 1, 20, RGB(v, v, v));
    }

    /* Transparent window: the video layer shows through here */
    fb_rect(fb, 900, 400, 340, 180, TRANSPARENT);
    fb_text(fb, 912, 590, "transparent hole", 2, GREY, TRANSPARENT);

    fb_text(fb, 40, 670, "Press STANDBY on the box to return to the normal menu", 2, WHITE, TRANSPARENT);
}

void hook_main(printf_t pf) {
    static const u16 colours[] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE };
    struct fb fb;
    int x = 60, y = 400, dx = 7, dy = 5, frame = 0;
    const int size = 80, top = 390, bottom = 650, left = 40, right = 880;
    u16 bg = RGB(10, 20, 60);

    pf("\n=== HOOK DEMO ===\n");
    if (fb_init(&fb) < 0) {
        pf("OSD header looks wrong, demo skipped\n");
        return;
    }
    pf("OSD %dx%d pitch %d at %08x. Press STANDBY to exit.\n", fb.w, fb.h, fb.pitch, (u32) fb.pix);

    draw_static(&fb);

    while (!standby_pressed()) {
        fb_rect(&fb, x, y, size, size, bg);
        x += dx;
        y += dy;
        if (x < left || x + size > right) {
            dx = -dx;
            x += 2 * dx;
            frame++;
        }
        if (y < top || y + size > bottom) {
            dy = -dy;
            y += 2 * dy;
            frame++;
        }
        fb_rect(&fb, x, y, size, size, colours[frame % 7]);
        wait_ms(20);
    }
    while (standby_pressed()) {
        wait_ms(20);
    }

    fb_clear(&fb, TRANSPARENT);
    pf("=== HOOK DEMO END, firmware continues ===\n");
}
