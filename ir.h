/*
 * IR remote receiver (hardware NEC decoder at 0xbf151000).
 * Init values copied from the stock firmware (mode 0 setup 0x80266a34).
 * Verified on hardware 2026-09-24 with irtest: stock remote = user code
 * 0xfe01, every frame had key ^ inverted-key == 0xff.
 *
 *   ir_init ();
 *   struct ir_event ev;
 *   if (ir_poll (&ev)) { ... ev.key, ev.repeat ... }
 *
 * Poll at least every ~20 ms. Holding a key on the stock remote sends one
 * frame, one NEC repeat code, then full frames again (verified with
 * irkeys), so "held" is decided by timing: the same key again within
 * IR_HOLD_MS, or a repeat code, gives ev.repeat = 1.
 */
#ifndef IR_H
#define IR_H

#include "uboot.h"

#define IR_BASE     0xbf151000
#define IR_DATA     (IR_BASE + 0x00)   /* 0-15 user code, 16-23 key, 24-31 ~key */
#define IR_CFG      (IR_BASE + 0x08)
#define IR_REP      (IR_BASE + 0x0c)   /* bit 0 = repeat code seen */
#define IR_INTEN    (IR_BASE + 0x10)
#define IR_STAT     (IR_BASE + 0x14)   /* bit 0 = data ready, clears after read */
#define IR_CTRL2    (IR_BASE + 0x18)
#define IR_TIMING   (IR_BASE + 0x40)
#define IR_CHIP_OPT 0xbf140020         /* bit 30 picks the timing table */

#define IR_USER_STOCK 0xfe01           /* stock remote */
#define IR_HOLD_MS  250

struct ir_event {
    u32 user;       /* 16-bit NEC user (address) code */
    u32 key;        /* 8-bit key code */
    int repeat;     /* 1 = key still held, key = last key */
};

static u32 ir_last_user, ir_last_key, ir_last_rep, ir_last_ms;
static int ir_have_key;

static inline void ir_init (void) {
    static const u32 timing_a[8] = {
        0x02240127, 0x0336023b, 0x02240127, 0x03360200,
        0x00280005, 0x0082004b, 0x00280005, 0x00410028,
    };
    static const u32 timing_b[8] = {
        0x01e70106, 0x02da01fb, 0x02240106, 0x02da01c7,
        0x00230004, 0x00730042, 0x00230004, 0x00390023,
    };
    const u32 *t = (REG32 (IR_CHIP_OPT) & 0x40000000) ? timing_b : timing_a;
    u32 i;

    REG32 (IR_CTRL2) &= 0x3fffffff;
    REG32 (IR_CFG) = 0x05740215;               /* prescale 2 in bits 8-11 */
    REG32 (IR_INTEN) = (REG32 (IR_INTEN) & ~0xfu) | 1;
    for (i = 0; i < 8; i++) {
        REG32 (IR_TIMING + i * 4) = t[i];
    }

    ir_last_rep = REG32 (IR_REP) & 1;
    ir_have_key = 0;
    if (REG32 (IR_STAT) & 1) {
        (void) REG32 (IR_DATA);                /* drop a stale frame */
    }
}

/* Returns 1 and fills *ev for a new key press or a repeat, else 0.
 * Frames with a bad inverted key byte are dropped. */
static inline int ir_poll (struct ir_event *ev) {
    u32 rep = REG32 (IR_REP) & 1;
    int rising = rep && !ir_last_rep;
    u32 now = get_timer (0);
    int recent = ir_have_key && now - ir_last_ms < IR_HOLD_MS;

    ir_last_rep = rep;
    if (REG32 (IR_STAT) & 1) {
        u32 data = REG32 (IR_DATA);
        u32 key = (data >> 16) & 0xff;

        if ((key ^ (data >> 24)) != 0xff) {
            return 0;
        }
        ev->repeat = recent && key == ir_last_key && (data & 0xffff) == ir_last_user;
        ir_last_user = data & 0xffff;
        ir_last_key = key;
        ir_last_ms = now;
        ir_have_key = 1;
        ev->user = ir_last_user;
        ev->key = key;
        return 1;
    }
    if (rising && recent) {
        ir_last_ms = now;
        ev->user = ir_last_user;
        ev->key = ir_last_key;
        ev->repeat = 1;
        return 1;
    }
    return 0;
}

#endif
