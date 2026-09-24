/*
 * opl: small OPL2 (YM3812) FM synth emulator. See opl.h.
 *
 * Operator:   out = wave[(phase >> 22) + modulation] * gain (attenuation)
 *             phase step = fnum * 2^block * multiple * 49716 / 2^20 Hz
 * Modulation: carrier phase += modulator output (+-4095 = +-4 cycles, as
 *             the chip); modulator feedback = (last two outputs) >> (9 - FB)
 * Envelope:   attenuation in 0.1875 dB units (0-511), linear in dB per
 *             sample; attack / decay / release times from the datasheet
 *             (rate 1 = 2826 ms attack, 39281 ms decay over 96 dB, each
 *             step halves). Sustain level = 3 dB steps. EG type 0
 *             (percussive) keeps decaying at the release rate after the
 *             sustain level.
 * Total attenuation = envelope + TL * 4 (0.75 dB steps), gain table.
 * Channels 9-17: a second register bank at 0x100-0x1ff (as the OPL3's
 * second array), so MIDI can use 18 voices; the OPL2 itself has 9.
 */
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "opl.h"

#define OPL_CLOCK   49716
#define ENV_MAX     (511 << 16)

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

struct op {
    uint32_t phase, step;
    int env, state;                 /* env: attenuation << 16 */
    int tl;                         /* total level, 0.1875 dB units */
    int ar, dr, sl, rr, egt, mult, wave;
    int out, prev;
};

struct chan {
    struct op op[2];                /* 0 = modulator, 1 = carrier */
    int fnum, block, key, fb, conn;
};

static struct chan ch[OPL_CHANNELS];
static int wave_select;
static int out_rate;

static short wave_tab[4][1024];
static unsigned short gain_tab[1024];   /* attenuation -> 0..4096 */
static uint32_t attack_step[16], decay_step[16];
static uint32_t step_k;                 /* phase step factor, see set_step () */

/* Frequency multiple x2 (MULT 0 = 1/2) */
static const unsigned char mult_x2[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

/* Register offset 0x00-0x15 -> slot (-1 = none); slot -> channel, op */
static const signed char slot_of_off[0x16] = {
    0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1, 12, 13, 14, 15, 16, 17
};

static struct op *op_of_off (int bank, int off) {
    int s, k;

    if (off < 0 || off >= 0x16 || slot_of_off[off] < 0) {
        return 0;
    }
    s = slot_of_off[off];
    k = s % 6;
    return &ch[bank * 9 + (s / 6) * 3 + k % 3].op[k / 3];
}

void opl_init (int rate) {
    /* Datasheet times in ms (attack 0 -> max, decay 96 dB), rates 1..15 */
    static const double attack_ms[16] = {
        0, 2826.24, 1413.12, 706.56, 353.28, 176.64, 88.32, 44.16,
        22.08, 11.04, 5.52, 2.76, 1.38, 0.69, 0.34, 0
    };
    static const double decay_ms[16] = {
        0, 39280.64, 19640.32, 9820.16, 4910.08, 2455.04, 1227.52, 613.76,
        306.88, 153.44, 76.72, 38.36, 19.18, 9.59, 4.79, 2.40
    };
    double g = 4096.0;
    int i;

    out_rate = rate;
    for (i = 0; i < 1024; i++) {
        int s = (int) (sin ((i + 0.5) * 2 * M_PI / 1024) * 4095.0);
        int a = s < 0 ? -s : s;

        wave_tab[0][i] = s;
        wave_tab[1][i] = i < 512 ? s : 0;
        wave_tab[2][i] = a;
        wave_tab[3][i] = (i & 256) ? 0 : a;
    }
    for (i = 0; i < 1024; i++) {
        gain_tab[i] = (unsigned short) g;
        g *= 0.978645;                  /* 10 ^ (-0.1875 / 20) */
    }
    for (i = 0; i < 16; i++) {
        double samples_a = attack_ms[i] * rate / 1000.0;
        double samples_d = decay_ms[i] * rate / 1000.0;

        attack_step[i] = i == 0 ? 0 : i == 15 ? ENV_MAX :
                         (uint32_t) (ENV_MAX / (samples_a < 1 ? 1 : samples_a));
        decay_step[i] = i == 0 ? 0 : (uint32_t) (ENV_MAX / (samples_d < 1 ? 1 : samples_d));
    }
    /* step = (fnum << block) * mult_x2 / 2 * OPL_CLOCK / 2^20 / rate * 2^32
     *      = (fnum << block) * mult_x2 * step_k >> 10 */
    step_k = (uint32_t) ((double) OPL_CLOCK * 2048.0 * 1024.0 / rate);
    opl_reset ();
}

void opl_reset (void) {
    int c, o;

    memset (ch, 0, sizeof (ch));
    for (c = 0; c < OPL_CHANNELS; c++) {
        for (o = 0; o < 2; o++) {
            ch[c].op[o].env = ENV_MAX;
            ch[c].op[o].state = EG_OFF;
        }
    }
    wave_select = 0;
}

static void set_step (struct chan *c) {
    uint32_t f = (uint32_t) c->fnum << c->block;
    int o;

    for (o = 0; o < 2; o++) {
        c->op[o].step = (uint32_t) (((uint64_t) f * (mult_x2[c->op[o].mult] * step_k)) >> 10);
    }
}

static void key_on (struct op *op) {
    op->phase = 0;
    if (op->ar == 15) {
        op->env = 0;
        op->state = EG_DECAY;
    } else {
        op->state = EG_ATTACK;
    }
}

static void key_off (struct op *op) {
    if (op->state != EG_OFF) {
        op->state = EG_RELEASE;
    }
}

void opl_write (int reg, int val) {
    struct op *op;
    struct chan *c;
    int key, bank = (reg >> 8) & 1;

    reg &= 0xff;
    val &= 0xff;
    if (reg == 0x01 && bank == 0) {
        wave_select = (val & 0x20) != 0;
        return;
    }
    switch (reg & 0xe0) {
    case 0x20:
        if ((op = op_of_off (bank, reg - 0x20))) {
            op->egt = (val >> 5) & 1;
            op->mult = val & 15;
        }
        break;
    case 0x40:
        if ((op = op_of_off (bank, reg - 0x40))) {
            op->tl = (val & 0x3f) * 4;
        }
        break;
    case 0x60:
        if ((op = op_of_off (bank, reg - 0x60))) {
            op->ar = val >> 4;
            op->dr = val & 15;
        }
        break;
    case 0x80:
        if ((op = op_of_off (bank, reg - 0x80))) {
            op->sl = ((val >> 4) == 15 ? 511 : (val >> 4) * 16) << 16;
            op->rr = val & 15;
        }
        break;
    case 0xe0:
        if ((op = op_of_off (bank, reg - 0xe0))) {
            op->wave = val & 3;
        }
        break;
    case 0xa0:
        if (reg >= 0xa0 && reg <= 0xa8) {
            c = &ch[bank * 9 + reg - 0xa0];
            c->fnum = (c->fnum & 0x300) | val;
            set_step (c);
        } else if (reg >= 0xb0 && reg <= 0xb8) {
            c = &ch[bank * 9 + reg - 0xb0];
            c->fnum = (c->fnum & 0xff) | ((val & 3) << 8);
            c->block = (val >> 2) & 7;
            set_step (c);
            key = (val >> 5) & 1;
            if (key && !c->key) {
                key_on (&c->op[0]);
                key_on (&c->op[1]);
            } else if (!key && c->key) {
                key_off (&c->op[0]);
                key_off (&c->op[1]);
            }
            c->key = key;
        }
        break;
    case 0xc0:
        if (reg >= 0xc0 && reg <= 0xc8) {
            c = &ch[bank * 9 + reg - 0xc0];
            c->fb = (val >> 1) & 7;
            c->conn = val & 1;
        }
        break;
    }
}

static inline void env_step (struct op *op) {
    switch (op->state) {
    case EG_ATTACK:
        op->env -= attack_step[op->ar];
        if (op->env <= 0) {
            op->env = 0;
            op->state = EG_DECAY;
        }
        break;
    case EG_DECAY:
        op->env += decay_step[op->dr];
        if (op->env >= op->sl) {
            op->env = op->sl;
            op->state = EG_SUSTAIN;
        }
        break;
    case EG_SUSTAIN:
        if (!op->egt) {
            op->env += decay_step[op->rr];     /* percussive: keep fading */
        }
        break;
    case EG_RELEASE:
        op->env += decay_step[op->rr];
        break;
    }
    if (op->env >= ENV_MAX) {
        op->env = ENV_MAX;
        if (op->state != EG_ATTACK) {
            op->state = EG_OFF;
        }
    }
}

static inline int op_calc (struct op *op, int mod) {
    int att = (op->env >> 16) + op->tl;
    int idx = ((op->phase >> 22) + mod) & 1023;
    int out;

    if (att > 1023) {
        att = 1023;
    }
    out = (wave_tab[wave_select ? op->wave : 0][idx] * gain_tab[att]) >> 12;
    op->phase += op->step;
    env_step (op);
    return out;
}

void opl_render (int *left, int *right, int n, const int *pan_l, const int *pan_r) {
    int c, i;

    for (c = 0; c < OPL_CHANNELS; c++) {
        struct chan *cc = &ch[c];
        struct op *m = &cc->op[0], *k = &cc->op[1];
        int gl = pan_l[c], gr = pan_r[c];

        if (k->state == EG_OFF && (!cc->conn || m->state == EG_OFF)) {
            continue;
        }
        for (i = 0; i < n; i++) {
            int fb = cc->fb ? (m->out + m->prev) >> (9 - cc->fb) : 0;
            int mo = op_calc (m, fb), out;

            m->prev = m->out;
            m->out = mo;
            out = cc->conn ? mo + op_calc (k, 0) : op_calc (k, mo);
            left[i] += (out * gl) >> 8;
            right[i] += (out * gr) >> 8;
        }
    }
}
