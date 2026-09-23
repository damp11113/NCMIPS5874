/*
 * Board support for PCB-CS8051M (Nationalchip 5874 "symphony").
 * Everything here is verified on real hardware.
 *
 * GPIO bank layout: +0x00 out data, +0x04 direction (1 = input),
 *                   +0x08 input level
 *   bank 0: 0xbf0a0000 pins 0-31
 *   bank 1: 0xbf0a0010 pins 32-63 (layout from U-Boot, untested)
 *   bank 2: 0xbf155000 pins 64-71
 *
 * Front power LED is bi-colour: pin 70 red, pin 71 green, 1 = on.
 * STANDBY button is pin 11, reads 0 while pressed.
 */
#ifndef BOARD_H
#define BOARD_H

#include "uboot.h"

#define GPIO0_IN    0xbf0a0008
#define GPIO2_OUT   0xbf155000

#define LED_RED     (1u << 6)   /* pin 70 */
#define LED_GREEN   (1u << 7)   /* pin 71 */
#define BTN_STANDBY (1u << 11)  /* pin 11, active low */

static inline void led_set (u32 mask, int on) {
    if (on) {
        REG32 (GPIO2_OUT) |= mask;
    } else {
        REG32 (GPIO2_OUT) &= ~mask;
    }
}

static inline void led_red (int on) {
    led_set (LED_RED, on);
}

static inline void led_green (int on) {
    led_set (LED_GREEN, on);
}

static inline int standby_pressed (void) {
    return (REG32 (GPIO0_IN) & BTN_STANDBY) == 0;
}

/* Block until STANDBY is pressed and released, or a serial key arrives.
 * Returns 1 for the button, 0 for a serial key. */
static inline int wait_standby (void) {
    while (!standby_pressed ()) {
        if (tstc ()) {
            getc ();
            return 0;
        }
        udelay (10000);
    }
    while (standby_pressed ()) {
        udelay (10000);
    }
    return 1;
}

#endif
