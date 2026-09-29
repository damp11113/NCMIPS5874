/*
 * adstest: front-panel I2C bus check (satellite box) with an ADS1115.
 *
 *   go ${a} [addr-hex]          ADS1115 address, default 48
 *
 * The FD650 panel chip acknowledges every byte on the bus (it is not a
 * real I2C device), so an ACK does not prove a device is there. This
 * reads data back instead: an absent address returns ff ff, the
 * ADS1115's config register returns 85 83 after power-on. Then one
 * single-shot conversion per input (AIN0..3 against GND, +-4.096 V,
 * 128 samples/s), printed in millivolts. Runs at 100 kHz.
 */
#include "uboot.h"
#include "fd650.h"

#define ADS_CONV    0x00
#define ADS_CONFIG  0x01

static int rd16(int addr, int reg, unsigned char *b) {
    unsigned char r = (unsigned char) reg;

    b[0] = b[1] = 0xee;
    return i2c_write_read(I2C_CH_FP, addr, &r, 1, b, 2);
}

int main(int argc, char *argv[]) {
    static const int probe[] = { 0x24, 0x27, 0x48, 0x49, 0x50, 0x7f };
    int ads = argc > 1 ? (int) parse_hex(argv[1]) : 0x48;
    unsigned char b[2];
    int i, r;

    fd650_init(FD650_PRESCALE_100K);
    printf("adstest: 100 kHz, ADS1115 at 0x%02x\n", ads);

    printf("probe (0 = ACK):");
    for (i = 0; i < (int) (sizeof(probe) / sizeof(probe[0])); i++) {
        printf(" %02x:%d", probe[i], i2c_write(I2C_CH_FP, probe[i], 0, 0));
    }
    printf("\n");

    for (i = 0; i < 3; i++) {
        static const int one[3] = { 0x27, 0x50, 0x48 };
        unsigned char reg = ADS_CONFIG;

        if (one[i] == 0x48) {
            printf("  set ADS pointer to config: rc %d\n", i2c_write(I2C_CH_FP, 0x48, &reg, 1));
        }
        b[0] = 0xee;
        r = i2c_read(I2C_CH_FP, one[i], b, 1);
        printf("read 1 byte from 0x%02x: rc %d, %02x, status after %02x\n", one[i], r, b[0],
               REG8(i2c_base(I2C_CH_FP) + I2C_ST));
    }
    r = i2c_read(I2C_CH_FP, 0x50, b, 2);
    printf("read 2 bytes from absent 0x50: rc %d, %02x %02x (expect ff ff)\n", r, b[0], b[1]);

    r = rd16(ads, ADS_CONFIG, b);
    printf("ADS1115 config register: rc %d, %02x %02x (power-on default 85 83)\n", r, b[0], b[1]);

    for (i = 0; i < 4; i++) {
        unsigned char cfg[3] = { ADS_CONFIG, (unsigned char) (0xc3 | (i << 4)), 0x83 };
        u32 t0;
        int raw, mv;

        r = i2c_write(I2C_CH_FP, ads, cfg, 3);
        if (r) {
            printf("AIN%d: write config failed, rc %d\n", i, r);
            continue;
        }
        t0 = get_timer(0);
        do {                                /* OS bit 15 = 1 when the conversion is done */
            r = rd16(ads, ADS_CONFIG, b);
        } while (r == 0 && !(b[0] & 0x80) && get_timer(t0) < 50);
        r = rd16(ads, ADS_CONV, b);
        raw = (short) (b[0] << 8 | b[1]);
        mv = raw * 4096 / 32768;
        printf("AIN%d: rc %d, raw %6d = %5d mV (%u ms)\n", i, r, raw, mv, get_timer(t0));
    }
    return 0;
}
