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
 *
 * Key codes of the stock remote: IR_KEY_* below, names via ir_key_name ().
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

static inline void ir_init(void) {
    static const u32 timing_a[8] = {
        0x02240127, 0x0336023b, 0x02240127, 0x03360200,
        0x00280005, 0x0082004b, 0x00280005, 0x00410028,
    };
    static const u32 timing_b[8] = {
        0x01e70106, 0x02da01fb, 0x02240106, 0x02da01c7,
        0x00230004, 0x00730042, 0x00230004, 0x00390023,
    };
    const u32 *t = (REG32(IR_CHIP_OPT) & 0x40000000) ? timing_b : timing_a;
    u32 i;

    REG32(IR_CTRL2) &= 0x3fffffff;
    REG32(IR_CFG) = 0x05740215;               /* prescale 2 in bits 8-11 */
    REG32(IR_INTEN) = (REG32(IR_INTEN) & ~0xfu) | 1;
    for (i = 0; i < 8; i++) {
        REG32(IR_TIMING + i * 4) = t[i];
    }

    ir_last_rep = REG32(IR_REP) & 1;
    ir_have_key = 0;
    if (REG32(IR_STAT) & 1) {
        (void) REG32(IR_DATA);                /* drop a stale frame */
    }
}

/* Returns 1 and fills *ev for a new key press or a repeat, else 0.
 * Frames with a bad inverted key byte are dropped. */
static inline int ir_poll(struct ir_event *ev) {
    u32 rep = REG32(IR_REP) & 1;
    int rising = rep && !ir_last_rep;
    u32 now = get_timer(0);
    int recent = ir_have_key && now - ir_last_ms < IR_HOLD_MS;

    ir_last_rep = rep;
    if (REG32(IR_STAT) & 1) {
        u32 data = REG32(IR_DATA);
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

/* Stock remote key codes (user code 0xfe01), mapped 2026-09-25 with irkeys.
 * The arrow keys double as channel / volume keys. */
#define IR_KEY_POWER    0x1c
#define IR_KEY_MUTE     0x0b
#define IR_KEY_INFO     0x14
#define IR_KEY_ITV      0x5a
#define IR_KEY_USB      0x5b
#define IR_KEY_RADIO    0x4a
#define IR_KEY_RED      0x4f
#define IR_KEY_GREEN    0x4e            /* labelled GREEN (SW) */
#define IR_KEY_YELLOW   0x4d
#define IR_KEY_BLUE     0x4c
#define IR_KEY_PLAY     0x53
#define IR_KEY_PAUSE    0x52
#define IR_KEY_STOP     0x51
#define IR_KEY_NEXT     0x58
#define IR_KEY_SETTINGS 0x18
#define IR_KEY_EXIT     0x1a
#define IR_KEY_VIDEO    0x43
#define IR_KEY_HOME     0x41
#define IR_KEY_UP       0x47            /* CH+ */
#define IR_KEY_DOWN     0x4b            /* CH- */
#define IR_KEY_LEFT     0x49            /* V- */
#define IR_KEY_RIGHT    0x45            /* V+ */
#define IR_KEY_OK       0x1e
#define IR_KEY_1        0x08
#define IR_KEY_2        0x09
#define IR_KEY_3        0x0a
#define IR_KEY_4        0x0c
#define IR_KEY_5        0x0d
#define IR_KEY_6        0x0e
#define IR_KEY_7        0x10
#define IR_KEY_8        0x11
#define IR_KEY_9        0x12
#define IR_KEY_0        0x15
#define IR_KEY_WIFI     0x46
#define IR_KEY_RECALL   0x16

/* Button name for a key code of the stock remote, "?" if unknown */
static inline const char *ir_key_name(u32 key) {
    static const struct { unsigned char key; const char *name; } names[] = {
        { IR_KEY_POWER, "POWER" }, { IR_KEY_MUTE, "MUTE" }, { IR_KEY_INFO, "INFO" },
        { IR_KEY_ITV, "ITV" }, { IR_KEY_USB, "USB" }, { IR_KEY_RADIO, "RADIO" },
        { IR_KEY_RED, "RED" }, { IR_KEY_GREEN, "GREEN" }, { IR_KEY_YELLOW, "YELLOW" },
        { IR_KEY_BLUE, "BLUE" }, { IR_KEY_PLAY, "PLAY" }, { IR_KEY_PAUSE, "PAUSE" },
        { IR_KEY_STOP, "STOP" }, { IR_KEY_NEXT, "NEXT" }, { IR_KEY_SETTINGS, "SETTINGS" },
        { IR_KEY_EXIT, "EXIT" }, { IR_KEY_VIDEO, "VIDEO" }, { IR_KEY_HOME, "HOME" },
        { IR_KEY_UP, "UP" }, { IR_KEY_DOWN, "DOWN" }, { IR_KEY_LEFT, "LEFT" },
        { IR_KEY_RIGHT, "RIGHT" }, { IR_KEY_OK, "OK" },
        { IR_KEY_1, "1" }, { IR_KEY_2, "2" }, { IR_KEY_3, "3" }, { IR_KEY_4, "4" },
        { IR_KEY_5, "5" }, { IR_KEY_6, "6" }, { IR_KEY_7, "7" }, { IR_KEY_8, "8" },
        { IR_KEY_9, "9" }, { IR_KEY_0, "0" },
        { IR_KEY_WIFI, "WIFI" }, { IR_KEY_RECALL, "RECALL" },
    };
    u32 i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (names[i].key == key) {
            return names[i].name;
        }
    }
    return "?";
}

#endif
