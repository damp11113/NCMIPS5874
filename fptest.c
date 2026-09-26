/*
 * fptest: satellite box front panel (FD650 on SoC I2C channel 2).
 *
 *   go ${a} [prescale-hex]      default 200 (SCL <= ~126 kHz for any
 *                               input clock up to 324 MHz)
 *
 * Prints the pinmux / I2C registers as U-Boot left them, sets up the
 * panel like the stock firmware, lights everything for 2 s, then shows
 * "123" with the green LED on (OK toggles it), then polls the
 * keys every 50 ms and prints each new key code.
 * Serial keys: '+' / '-' brightness, 's' scan the bus for devices,
 * 'w' walk: one register bit at a time (Enter = next, q = stop),
 * 'q' (or any other key) quits.
 */
#include "uboot.h"
#include "fd650.h"

static void dump_regs(void) {
    u32 b = i2c_base(I2C_CH_FP);
    int off;

    printf("pinmux 0xbf15b400 = %08x, 0xbf15b404 = %08x\n",
            REG32(0xbf15b400u), REG32(0xbf15b404u));
    printf("i2c ch2 0x%08x:", b);
    for (off = 0x04; off <= 0x1c; off += 4) {
        printf(" +%02x=%02x", off, REG8(b + off));
    }
    printf("\n");
}

static const char *err_name(int r) {
    switch (r) {
    case 0:
        return "ok";
    case I2C_E_NACK:
        return "NACK";
    case I2C_E_TIMEOUT:
        return "timeout";
    case I2C_E_BUS:
        return "bus error";
    default:
        return "error";
    }
}

static void show_default(void) {
    fd650_show("123");
    fd650_led(1);
}

/* Light one register bit at a time, to find the digits and the LED */
static void walk(void) {
    int reg, bit, i;

    printf("walk: note what lights for each step (Enter = next, q = stop)\n");
    for (reg = 0; reg < 4; reg++) {
        for (bit = 0; bit < 8; bit++) {
            for (i = 0; i < 4; i++) {
                fd650_digit(i, i == reg ? (unsigned char) (1 << bit) : 0);
            }
            printf("  reg %d (byte 0x%02x) bit %d: ", reg, 0x68 + 2 * reg, bit);
            if (getc() == 'q') {
                printf("\n");
                show_default();
                return;
            }
            printf("\n");
        }
    }
    show_default();
}

static void scan(void) {
    int a, n = 0;

    printf("scan ch2:");
    for (a = 0x08; a < 0x78; a++) {
        int r = i2c_write(I2C_CH_FP, a, 0, 0);

        if (r == 0) {
            printf(" %02x", a);
            n++;
        } else if (r != I2C_E_NACK) {
            printf(" [%02x: %s, cmd reg %02x]", a, err_name(r), i2c_last_cmd);
            break;
        }
    }
    printf("\n%d address(es) answered (FD650: 24-27, 34-37)\n", n);
}

int main(int argc, char *argv[]) {
    u32 pre = argc > 1 ? parse_hex(argv[1]) : 0x200;
    int bright = 4, last = -1, led = 1, r, i;

    printf("fptest: FD650 front panel, prescaler 0x%x\n", pre);
    printf("before: ");
    dump_regs();

    r = fd650_init(pre);
    printf("display on (0x48 0x41): %s", err_name(r));
    if (r == I2C_E_TIMEOUT) {
        printf(" (cmd reg stuck at %02x)", i2c_last_cmd);
    }
    printf("\nafter:  ");
    dump_regs();
    if (r < 0) {
        printf("no answer from the panel; try another prescaler, e.g. go ${a} 40\n");
        return 1;
    }

    for (i = 0; i < 4; i++) {
        fd650_digit(i, 0xff);
    }
    printf("all segments + LED on for 2 s\n");
    udelay(2000000);
    fd650_show("123");
    fd650_led(1);
    printf("showing 123, LED on; a pressed key shows its code, OK toggles the LED\n");
    printf("press the 6 front buttons; serial: + - brightness, s scan, w walk, q quit\n");

    for (;;) {
        int k = fd650_key();

        if (k != last) {
            if (k < 0) {
                printf("key read: %s\n", err_name(k));
            } else {
                printf("key 0x%02x%s\n", k, (k & FD650_KEY_PRESSED) ? " (pressed)" : "");
                if (k & FD650_KEY_PRESSED) {
                    char s[3];

                    s[0] = "0123456789abcdef"[(k >> 4) & 0xf];
                    s[1] = "0123456789abcdef"[k & 0xf];
                    s[2] = 0;
                    fd650_show(s);
                    if ((k & ~FD650_KEY_PRESSED) == FD650_KEY_OK) {
                        led = !led;
                        fd650_led(led);
                    }
                }
            }
            last = k;
        }
        if (tstc()) {
            int c = getc();

            if (c == '+' || c == '-') {
                bright = (bright + (c == '+' ? 1 : 7)) & 7;
                /* brightness field: 0 = 8/8, 1..7 = 1/8..7/8 */
                fd650_control((unsigned char) ((bright << 4) | 1));
                printf("brightness field %d\n", bright);
            } else if (c == 's') {
                scan();
            } else if (c == 'w') {
                walk();
            } else {
                break;
            }
        }
        udelay(50000);
    }
    printf("fptest: done\n");
    return 0;
}
