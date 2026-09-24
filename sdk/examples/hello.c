/*
 * SDK example: text on screen and serial, the app's folder listing,
 * remote / serial buttons until BACK (EXIT on the remote, Esc on serial).
 * Crash tests for the launcher's crash screen: RED writes through a NULL
 * pointer, YELLOW does an unaligned read, BLUE jumps into data.
 *
 *   sh sdk/build.sh HELLO.BIN sdk/examples/hello.c
 */
#include "sdk.h"

static void crash_test (int btn) {
    static const u32 not_code[2] = { 0xffffffffu, 0 };      /* not an instruction */

    printf ("hello: crash test %s\n", sdk_btn_name (btn));
    if (btn == BTN_RED) {
        *(volatile int *) 0 = 1;
    } else if (btn == BTN_YELLOW) {
        u32 v;

        __asm__ volatile ("lw %0, 0(%1)" : "=r" (v) : "r" (0x80100002u));
        (void) v;
    } else if (btn == BTN_BLUE) {
        ((void (*) (void)) (void *) not_code) ();
    }
}

int main (int argc, char *argv[]) {
    struct fb fb;
    struct sdk_dirent e;
    struct sdk_key k;
    int h, y = 120, i;

    printf ("hello: app folder \"%s\", data folder \"%s\", %d args:", sdk_app_dir,
            sdk_data_dir, argc - 1);
    for (i = 1; i < argc; i++) {
        printf (" %s", argv[i]);
    }
    printf ("\n");

    if (osd_setup (&fb) < 0) {
        printf ("hello: no display (source avstart.scr first)\n");
        return 1;
    }
    fb_clear (&fb, RGB (10, 20, 60));
    fb_text (&fb, 40, 30, "Hello from the SDK!", 4, WHITE, TRANSPARENT);

    h = sdk_dir_open ("");
    if (h >= 0) {
        while (sdk_dir_read (h, &e) && y < 560) {
            char line[160];

            snprintf (line, sizeof (line), "%s%s  %u", e.name, e.is_dir ? "/" : "", e.size);
            fb_text (&fb, 40, y, line, 2, e.is_dir ? YELLOW : CYAN, TRANSPARENT);
            printf ("  %s\n", line);
            y += 34;
        }
        sdk_dir_close (h);
    }
    fb_text (&fb, 40, 620, "Press buttons; EXIT / Esc quits", 2, GREY, TRANSPARENT);
    fb_text (&fb, 40, 660, "Crash tests: RED NULL write, YELLOW unaligned, BLUE bad code", 2,
             RGB (255, 120, 120), TRANSPARENT);

    for (;;) {
        if (!sdk_key_poll (&k)) {
            sdk_idle (2000);
        } else if (k.btn != BTN_NONE) {
            char line[64];

            snprintf (line, sizeof (line), "button %-8s%s", sdk_btn_name (k.btn),
                      k.repeat ? " (held)" : "        ");
            fb_text (&fb, 40, 580, line, 2, GREEN, RGB (10, 20, 60));
            printf ("%s\n", line);
            if (k.btn == BTN_BACK) {
                break;
            }
            if ((k.btn == BTN_RED || k.btn == BTN_YELLOW || k.btn == BTN_BLUE) && !k.repeat) {
                crash_test (k.btn);
            }
        }
    }
    fb_clear (&fb, TRANSPARENT);
    return 0;
}
