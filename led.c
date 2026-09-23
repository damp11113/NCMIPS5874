/*
 * led: drive the two front LEDs, read the STANDBY button.
 *
 *   go ${a}
 *
 * Verified on board (regwatch + U-Boot disassembly):
 *   GPIO bank 64-71 at 0xbf155000: +0 out data, +4 dir (1 = input), +8 in level
 *   LEDs on pins 70 / 71 = bits 6 / 7 (already outputs, dir = 0x3f)
 *   STANDBY button on pin 11 = bit 11 of 0xbf0a0008, 0 while pressed
 *
 * Blinks both LEDs, then each STANDBY press steps to the next pattern.
 * Any key on the serial console restores the LEDs and exits.
 */
#include "uboot.h"

#define GPIO2_OUT   0xbf155000
#define GPIO0_IN    0xbf0a0008

#define LED_A       (1u << 6)   /* pin 70 */
#define LED_B       (1u << 7)   /* pin 71 */
#define BTN_STANDBY (1u << 11)  /* pin 11, active low */

static void leds_set (u32 bits) {
    u32 v = REG32 (GPIO2_OUT);

    v &= ~(LED_A | LED_B);
    v |= bits & (LED_A | LED_B);
    REG32 (GPIO2_OUT) = v;
}

static int standby_pressed (void) {
    return (REG32 (GPIO0_IN) & BTN_STANDBY) == 0;
}

int main (int argc, char *argv[]) {
    static const u32 patterns[] = { 0, LED_A, LED_B, LED_A | LED_B };
    static const char *names[] = {
        "pin70=0 pin71=0",
        "pin70=1 pin71=0",
        "pin70=0 pin71=1",
        "pin70=1 pin71=1",
    };
    u32 saved = REG32 (GPIO2_OUT);
    int i, mode = 0, was_pressed = 0;

    printf ("LED test. Saved GPIO2_OUT = 0x%08x\n", saved);

    printf ("Blinking: pin70 and pin71 alternate 5 times...\n");
    for (i = 0; i < 5; i++) {
        leds_set (LED_A);
        udelay (300000);
        leds_set (LED_B);
        udelay (300000);
    }

    printf ("\nNow press STANDBY to step through patterns.\n");
    printf ("Note which LED is lit for each line. Any serial key exits.\n\n");
    leds_set (patterns[mode]);
    printf ("  [%d] %s\n", mode, names[mode]);

    while (!tstc ()) {
        int pressed = standby_pressed ();

        if (pressed && !was_pressed) {
            mode = (mode + 1) % 4;
            leds_set (patterns[mode]);
            printf ("  [%d] %s\n", mode, names[mode]);
        }
        was_pressed = pressed;
        udelay (20000);
    }
    getc ();

    REG32 (GPIO2_OUT) = saved;
    printf ("Restored GPIO2_OUT = 0x%08x, bye!\n", saved);
    return 0;
}
