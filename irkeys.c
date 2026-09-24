/*
 * irkeys: ir.h demo / key-map helper. Prints every remote key, flashes
 * the green LED on each new press. STANDBY or a serial key stops.
 *
 *   go ${a}
 */
#include "board.h"
#include "ir.h"

int main () {
    struct ir_event ev;
    u32 presses = 0, repeats = 0;

    ir_init ();
    printf ("irkeys: press remote keys (STANDBY or serial key stops)\n");

    while (!standby_pressed () && !tstc ()) {
        if (ir_poll (&ev)) {
            if (ev.repeat) {
                printf ("  repeat key 0x%02x\n", ev.key);
                repeats++;
            } else {
                printf ("key 0x%02x  (user %04x%s)\n", ev.key, ev.user,
                        ev.user == IR_USER_STOCK ? "" : ", not stock remote");
                led_green (1);
                udelay (30000);
                led_green (0);
                presses++;
            }
        }
        udelay (5000);
    }
    if (tstc ()) {
        getc ();
    }
    while (standby_pressed ()) {
        udelay (10000);
    }

    led_green (1);
    printf ("irkeys done: %d presses, %d repeats\n", presses, repeats);
    return 0;
}
