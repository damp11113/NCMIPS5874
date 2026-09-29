/*
 * SDK example: list the devices on the front-panel I2C bus (satellite box)
 * at standard (100 kHz) and fast (400 kHz) speed, on screen and serial.
 * Also times one address probe (START, address byte, ACK bit, STOP: about
 * 10 clock periods plus driver overhead), so the bus speed can be checked
 * without a scope: ~110 us at 100 kHz, ~30 us at 400 kHz.
 * OK scans again, EXIT / Esc quits.
 *
 *   sh sdk/build.sh I2CSCAN.BIN sdk/examples/i2cscan.c
 */
#include "sdk.h"

static struct fb fb;
static int have_screen, line_y;

static void out(const char *s, u16 col) {
    printf("i2cscan: %s\n", s);
    if (have_screen && line_y < 620) {
        fb_text(&fb, 40, line_y, s, 2, col, TRANSPARENT);
        line_y += 34;
    }
}

static inline u32 count(void) {
    u32 c;

    __asm__ volatile("mfc0 %0, $9" : "=r" (c));
    return c;
}

static void scan(int khz) {
    char line[160], list[120];
    int real = sdk_i2c_speed(khz), a, n = 0, errors = 0, len = 0;
    u32 t = 0, probes = 0;

    list[0] = 0;
    for (a = 0x08; a <= 0x77; a++) {
        u32 t0 = count();
        int r = sdk_i2c_write(a, 0, 0);

        t += count() - t0;
        probes++;
        if (r == 0) {
            n++;
            if (len < (int) sizeof(list) - 4) {
                len += snprintf(list + len, sizeof(list) - len, "%02x ", a);
            }
        } else if (r != SDK_I2C_ENACK) {
            errors++;
        }
    }
    /* CP0 Count runs at 324 MHz: 324 ticks per microsecond */
    snprintf(line, sizeof(line), "%d kHz (real %d kHz): %d device(s), %u us per probe%s",
             khz, real, n, t / probes / 324, errors ? ", BUS ERRORS" : "");
    out(line, errors ? RED : WHITE);
    snprintf(line, sizeof(line), "  %s", n ? list : "(none)");
    out(line, n ? CYAN : GREY);
}

static void run(void) {
    line_y = 130;
    if (have_screen) {
        fb_clear(&fb, RGB(12, 22, 30));
        fb_text(&fb, 40, 30, "I2C scan", 4, WHITE, TRANSPARENT);
        fb_text(&fb, 40, 84, "Front-panel bus, 7-bit addresses 0x08-0x77. FD650: 24-27, 34-37", 2,
                 GREY, TRANSPARENT);
    }
    if (!sdk_box_sat) {
        out("This box has no I2C bus for apps (satellite box only).", YELLOW);
        return;
    }
    scan(100);
    scan(400);
    sdk_i2c_speed(100);
    if (have_screen) {
        fb_text(&fb, 40, 660, "OK scan again, EXIT quits", 2, GREY, TRANSPARENT);
    }
}

int main(int argc, char *argv[]) {
    struct sdk_key k;

    (void) argc;
    (void) argv;
    have_screen = osd_setup(&fb) == 0;
    run();
    for (;;) {
        if (!sdk_key_poll(&k)) {
            sdk_idle(2000);
        } else if (k.btn == BTN_OK && !k.repeat) {
            run();
        } else if (k.btn == BTN_BACK || k.btn == BTN_POWER) {
            break;
        }
    }
    return 0;
}
