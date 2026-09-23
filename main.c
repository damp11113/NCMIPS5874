#include "board.h"

/*
 * Example using board.h: STANDBY toggles between red and green,
 * any serial key quits and leaves the LED green (normal "on" state).
 */
int main (int argc, char *argv[]) {
    int green = 1, presses = 0;

    printf ("Press STANDBY to switch red/green. Any serial key quits.\n");
    led_red (0);
    led_green (1);

    while (wait_standby ()) {
        green = !green;
        led_green (green);
        led_red (!green);
        presses++;
        printf ("  press %d: %s\n", presses, green ? "green" : "red");
    }

    led_red (0);
    led_green (1);
    printf ("Bye after %d presses.\n", presses);
    return 0;
}
