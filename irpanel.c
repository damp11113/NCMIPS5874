/*
 * irpanel: IR remote test for the satellite box (no IPTV board GPIOs).
 *
 *   go ${a}
 *
 * Prints the IR block registers as U-Boot left them, sets the decoder up
 * like on the IPTV box (ir.h), then prints every key (code, name, NEC
 * user code) and shows the key code on the front display. Raw decoder
 * frames are printed too (also ones ir.h drops), so another user code or
 * a bad inverted byte still shows up. MENU on the front or any serial
 * key stops.
 */
#include "uboot.h"
#include "ir.h"
#include "fd650.h"

static void dump_ir (const char *when) {
    int off;

    printf ("%s: IR 0xbf151000:", when);
    for (off = 0x00; off <= 0x18; off += 4) {
        printf (" +%02x=%08x", off, REG32 (IR_BASE + off));
    }
    printf ("\n      timing +40..+5c:");
    for (off = 0x40; off <= 0x5c; off += 4) {
        printf (" %08x", REG32 (IR_BASE + off));
    }
    printf ("\n      chip opt 0xbf140020 = %08x\n", REG32 (IR_CHIP_OPT));
}

static void show_hex (u32 v) {
    char s[3];

    s[0] = "0123456789abcdef"[(v >> 4) & 0xf];
    s[1] = "0123456789abcdef"[v & 0xf];
    s[2] = 0;
    fd650_show (s);
}

int main () {
    struct ir_event ev;
    u32 last_raw = 0, frames = 0;
    int panel;

    dump_ir ("before");
    ir_init ();
    dump_ir ("after ");

    panel = fd650_init (0x200) == 0;
    if (panel) {
        fd650_show ("ir");
        fd650_led (0);
    }
    printf ("irpanel: press remote keys (MENU on the front or a serial key stops)%s\n",
            panel ? "" : " [front panel not answering]");

    while (!tstc ()) {
        /* Raw view: a new word in the data register, before ir_poll consumes it */
        if (REG32 (IR_STAT) & 1) {
            u32 raw = REG32 (IR_DATA);

            frames++;
            if (raw != last_raw) {
                printf ("  raw frame %08x (user %04x key %02x ~key %02x)\n",
                        raw, raw & 0xffff, (raw >> 16) & 0xff, raw >> 24);
                last_raw = raw;
            }
            if (((raw >> 16) ^ (raw >> 24)) & 0xff) {
                if ((((raw >> 16) ^ (raw >> 24)) & 0xff) != 0xff) {
                    printf ("  (bad inverted byte)\n");
                } else {
                    ev.user = raw & 0xffff;
                    ev.key = (raw >> 16) & 0xff;
                    printf ("key 0x%02x %-8s (user %04x%s)\n", ev.key, ir_key_name (ev.key),
                            ev.user, ev.user == IR_USER_STOCK ? "" : ", not stock remote");
                    if (panel) {
                        show_hex (ev.key);
                        fd650_led (1);
                        udelay (30000);
                        fd650_led (0);
                    }
                }
            }
        }
        if (panel) {
            int k = fd650_key ();

            if (k == (FD650_KEY_MENU | FD650_KEY_PRESSED)) {
                break;
            }
        }
        udelay (10000);
    }
    if (tstc ()) {
        (void) getc ();
    }
    printf ("irpanel: %u frames\n", frames);
    return 0;
}
