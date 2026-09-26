/*
 * irtest: IR remote receiver test (U-Boot).
 *
 *   go ${a}          dump regs, init IR block like the stock firmware, poll
 *   go ${a} r        read-only: dump regs and poll, no writes
 *   go ${a} p <hex>  init with prescale <hex> (0..f, default 2)
 *
 * Stops on STANDBY or any serial key.
 *
 * IR block 0xbf151000, from the firmware's MIPS16 driver (open 0x80267a08,
 * ISR 0x80266d0c, IRQ 40), hardware-decode mode (config mode 0):
 *   +0x00 data:   bits 0-15 user code, bits 16-23 key code (NEC)
 *   +0x04 pulse FIFO data (software-decode mode only)
 *   +0x08 config: 0x05740215 | prescale << 8 (firmware prescale 2)
 *   +0x0c bit 0 repeat, bits 8-15 pulse FIFO count
 *   +0x10 bits 0-3 interrupt enable (1 = data)
 *   +0x14 status: bit 0 data ready, bit 1 error (driver drops the frame)
 *   +0x18 bits 30-31 cleared at init
 *   +0x40..0x5c pulse-width windows (max << 16 | min), 2 tables picked
 *               by 0xbf140020 bit 30
 * VERIFIED on hardware 2026-09-24 (stock remote, user code 0xfe01).
 */
#include "board.h"
#include "irranges.h"

#define IR_BASE     0xbf151000
#define IR_DATA     (IR_BASE + 0x00)
#define IR_CFG      (IR_BASE + 0x08)
#define IR_REP      (IR_BASE + 0x0c)
#define IR_INTEN    (IR_BASE + 0x10)
#define IR_STAT     (IR_BASE + 0x14)
#define IR_CTRL2    (IR_BASE + 0x18)
#define IR_TIMING   (IR_BASE + 0x40)
#define CHIP_OPT    0xbf140020

/* Firmware function 0x80266130: windows for 0x40..0x5c */
static const u32 timing_a[8] = {   /* 0xbf140020 bit 30 clear */
    0x02240127, 0x0336023b, 0x02240127, 0x03360200,
    0x00280005, 0x0082004b, 0x00280005, 0x00410028,
};
static const u32 timing_b[8] = {   /* 0xbf140020 bit 30 set */
    0x01e70106, 0x02da01fb, 0x02240106, 0x02da01c7,
    0x00230004, 0x00730042, 0x00230004, 0x00390023,
};

static void dump_ranges(const char *tag) {
    u32 r, i;

    printf("=== IR SNAP BEGIN (%s) ===\n", tag);
    for (r = 0; r < IR_NRANGES; r++) {
        for (i = 0; i < ir_ranges[r].words; i += 4) {
            u32 a = ir_ranges[r].start + i * 4;
            printf("S %08x: %08x %08x %08x %08x\n", a,
                    REG32(a), REG32(a + 4), REG32(a + 8), REG32(a + 12));
        }
    }
    printf("=== IR SNAP END ===\n");
}

/* Same register writes as the firmware's mode 0 setup (0x80266a34) */
static void ir_init(u32 prescale) {
    const u32 *t = (REG32(CHIP_OPT) & 0x40000000) ? timing_b : timing_a;
    u32 cfg, i;

    REG32(IR_CTRL2) &= 0x3fffffff;

    cfg = 0x05740200 | ((prescale & 0xf) << 8);
    cfg = (cfg & ~0x60u) | 0x14;
    cfg = (cfg & ~0x02u) | 0x01;
    REG32(IR_CFG) = cfg;

    REG32(IR_INTEN) = (REG32(IR_INTEN) & ~0xfu) | 1;

    for (i = 0; i < 8; i++) {
        REG32(IR_TIMING + i * 4) = t[i];
    }

    printf("IR init: cfg %08x (read back %08x), timing table %c\n",
            cfg, REG32(IR_CFG), t == timing_b ? 'b' : 'a');
    if (REG32(IR_CFG) != cfg || REG32(IR_TIMING) != t[0]) {
        printf("WARNING: writes did not stick, block may be unclocked / in reset\n");
    }
}

int main(int argc, char *argv[]) {
    u32 prescale = 2, events = 0;
    u32 stat, data, rep;
    u32 last_stat, last_data, last_rep;
    int init = 1;

    if (argc > 1 && strcmp(argv[1], "r") == 0) {
        init = 0;
    } else if (argc > 2 && strcmp(argv[1], "p") == 0) {
        prescale = parse_hex(argv[2]);
    }

    dump_ranges("before");
    if (init) {
        ir_init(prescale);
        dump_ranges("after init");
    }

    last_stat = REG32(IR_STAT);
    last_data = REG32(IR_DATA);
    last_rep = REG32(IR_REP);
    printf("\nstat %08x data %08x rep %08x\n", last_stat, last_data, last_rep);
    printf("Point the remote at the box and press keys.\n");
    printf("STANDBY or any serial key stops.\n\n");

    while (!standby_pressed() && !tstc()) {
        stat = REG32(IR_STAT);
        rep = REG32(IR_REP);
        /* Data read may pop the decoder, so only read it when ready */
        data = (stat & 1) ? REG32(IR_DATA) : last_data;

        if (stat != last_stat) {
            printf("stat %08x -> %08x\n", last_stat, stat);
        }
        if (!(stat & 1) && rep != last_rep) {
            printf("rep  %08x -> %08x (fifo %d)\n", last_rep, rep, (rep >> 8) & 0xff);
        }
        if ((stat & 1) && (data != last_data || rep != last_rep || stat != last_stat)) {
            u32 key = (data >> 16) & 0xff;
            u32 inv = (data >> 24) & 0xff;

            printf("IR data %08x: user %04x key %02x (inv %02x %s) repeat %d fifo %d\n",
                    data, data & 0xffff, key, inv,
                    (key ^ inv) == 0xff ? "ok" : "--",
                    rep & 1, (rep >> 8) & 0xff);
            events++;
        }
        last_stat = stat;
        last_data = data;
        last_rep = rep;
        udelay(1000);
    }
    if (tstc()) {
        getc();
    }
    while (standby_pressed()) {
        udelay(10000);
    }

    dump_ranges("end");
    printf("irtest done, %d IR events\n", events);
    return 0;
}
