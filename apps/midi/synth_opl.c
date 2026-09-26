/*
 * OPL engine for the MIDI player: GENMIDI patches (Doom's "#OPL_II#"
 * format: 128 melodic + 47 percussion instruments for keys 35-81) on the
 * 18-channel OPL emulator (sdk/opl.c), as DOS games played General MIDI on
 * an AdLib / OPL3 card.
 *
 * GENMIDI instrument (36 bytes): flags (1 fixed note, 4 double voice),
 * fine tune (second voice, 128 = none), fixed note, 2 voices of 16 bytes:
 * modulator (0x20 tremolo/mult, 0x60 attack/decay, 0x80 sustain/release,
 * 0xE0 wave, key scale 0x40 hi, level 0x40 lo), feedback/connection 0xC0,
 * carrier (same 6), unused, base note offset (int16).
 *
 * Volume = velocity x CC7 x CC11 as carrier (and additive modulator)
 * attenuation in dB; CC10 pan gives the voice a left / right gain (stereo
 * extension, the chip is mono); pitch bend +-2 semitones (RPN 0 changes
 * the range) in 1/32 semitone steps; CC64 sustain; CC120/123 off.
 */
#include <string.h>
#include <stdio.h>

#include "synth.h"
#include "opl.h"

#define NVOICES     OPL_CHANNELS
#define GAIN_MUL    5               /* OPL channel +-4095 -> mix scale: x 5/8, */
#define GAIN_SHIFT  3               /* dense MIDI with 18 voices must not clip */

struct voice {
    int on, sustained, chan, key, play_note, vel, second;
    unsigned age;
    const unsigned char *ins;
};

struct chan {
    int program, volume, expression, pan, bend, bend_range, sustain;
    int rpn_msb, rpn_lsb;
};

static const unsigned char *gm;     /* GENMIDI lump */
static struct voice voices[NVOICES];
static struct chan chans[16];
static int pan_l[OPL_CHANNELS], pan_r[OPL_CHANNELS];
static unsigned age_counter;
static unsigned short fnum_tab[384];
static unsigned char vol_atten[128];

static int le16(const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

/* OPL register of voice v (0-17: bank 1 for 9-17) */
static int reg(int v, int base) {
    return ((v / 9) << 8) | (base + v % 9);
}

static int op_reg(int v, int base, int carrier) {
    static const unsigned char off[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

    return ((v / 9) << 8) | (base + off[v % 9] + (carrier ? 3 : 0));
}

static void build_tables(void) {
    double f = 261.6255653, x = 0.6931471805599453 / 384, r, amp = 127.0;
    int i, a;

    r = 1 + x + x * x / 2 + x * x * x / 6;
    for (i = 0; i < 384; i++) {
        fnum_tab[i] = (unsigned short) (f * 65536.0 / 49716.0 + 0.5);
        f *= r;
    }
    vol_atten[0] = 63;
    for (a = 0, i = 127; i > 0; i--) {
        while (amp * 0.9173 >= i && a < 63) {
            amp *= 0.9173;
            a++;
        }
        vol_atten[i] = a;
    }
}

static const unsigned char *voice_data(const struct voice *v) {
    return v->ins + 4 + (v->second ? 16 : 0);
}

static void set_volume(int vi) {
    struct voice *v = &voices[vi];
    const unsigned char *vo = voice_data(v);
    const struct chan *c = &chans[v->chan];
    int amp = v->vel * c->volume / 127 * c->expression / 127;
    int att = vol_atten[amp], car = (vo[12] & 0x3f) + att;

    opl_write(op_reg(vi, 0x40, 1), vo[11] | (car > 63 ? 63 : car));
    if (vo[6] & 1) {
        int mod = (vo[5] & 0x3f) + att;

        opl_write(op_reg(vi, 0x40, 0), vo[4] | (mod > 63 ? 63 : mod));
    }
}

static void set_pan(int vi) {
    int pan = chans[voices[vi].chan].pan;

    pan_l[vi] = pan <= 64 ? 256 : (127 - pan) * 256 / 63;
    pan_r[vi] = pan >= 64 ? 256 : pan * 256 / 64;
}

static void set_freq(int vi, int key_on) {
    struct voice *v = &voices[vi];
    const struct chan *c = &chans[v->chan];
    int p = v->play_note * 32 + (c->bend - 8192) * c->bend_range * 32 / 8192;
    int k, oct, block, fnum;

    if (v->second) {
        p += ((int) v->ins[2] - 128) / 2;       /* fine tune of the second voice */
    }
    if (p < 0) {
        p = 0;
    }
    k = (p - 60 * 32) % 384;
    oct = (p - 60 * 32) / 384;
    if (k < 0) {
        k += 384;
        oct--;
    }
    block = 4 + oct;
    fnum = fnum_tab[k];
    while (block < 0) {
        fnum >>= 1;
        block++;
    }
    while (block > 7) {
        fnum <<= 1;
        block--;
    }
    if (fnum > 1023) {
        fnum = 1023;
    }
    opl_write(reg(vi, 0xa0), fnum & 0xff);
    opl_write(reg(vi, 0xb0), (key_on ? 0x20 : 0) | (block << 2) | (fnum >> 8));
}

static void voice_off(int vi) {
    if (voices[vi].on) {
        voices[vi].on = 0;
        voices[vi].sustained = 0;
        voices[vi].age = ++age_counter;
        set_freq(vi, 0);
    }
}

static int alloc_voice(void) {
    int i, best = -1;
    unsigned best_age = ~0u;

    for (i = 0; i < NVOICES; i++) {
        if (!voices[i].on && voices[i].age < best_age) {
            best = i;
            best_age = voices[i].age;
        }
    }
    if (best >= 0) {
        return best;
    }
    for (i = 0; i < NVOICES; i++) {
        if (voices[i].age < best_age) {
            best = i;
            best_age = voices[i].age;
        }
    }
    voice_off(best);
    return best;
}

static void start_voice(int ch, int key, int vel, const unsigned char *ins, int second) {
    const unsigned char *vo = ins + 4 + (second ? 16 : 0);
    int vi = alloc_voice(), play = (le16(ins) & 1) ? ins[3] : key;
    struct voice *v = &voices[vi];

    play += (short) le16(vo + 14);
    play = play < 0 ? 0 : play > 127 ? 127 : play;
    set_freq(vi, 0);
    v->on = 1;
    v->sustained = 0;
    v->chan = ch;
    v->key = key;
    v->play_note = play;
    v->vel = vel;
    v->second = second;
    v->age = ++age_counter;
    v->ins = ins;
    opl_write(op_reg(vi, 0x20, 0), vo[0]);
    opl_write(op_reg(vi, 0x60, 0), vo[1]);
    opl_write(op_reg(vi, 0x80, 0), vo[2]);
    opl_write(op_reg(vi, 0xe0, 0), vo[3]);
    opl_write(op_reg(vi, 0x40, 0), vo[4] | (vo[5] & 0x3f));
    opl_write(op_reg(vi, 0x20, 1), vo[7]);
    opl_write(op_reg(vi, 0x60, 1), vo[8]);
    opl_write(op_reg(vi, 0x80, 1), vo[9]);
    opl_write(op_reg(vi, 0xe0, 1), vo[10]);
    opl_write(reg(vi, 0xc0), vo[6] | 0x30);   /* 0x30: OPL3 left+right bits */
    set_volume(vi);
    set_pan(vi);
    set_freq(vi, 1);
}

static void s_note_on(int ch, int key, int vel) {
    const unsigned char *ins;

    if (!gm) {
        return;
    }
    if (ch == 9) {
        if (key < 35 || key > 81) {
            return;
        }
        ins = gm + 8 + (128 + key - 35) * 36;
    } else {
        ins = gm + 8 + chans[ch].program * 36;
    }
    start_voice(ch, key, vel, ins, 0);
    if (le16(ins) & 4) {
        start_voice(ch, key, vel, ins, 1);     /* double-voice instrument */
    }
}

static void s_note_off(int ch, int key) {
    int i;

    for (i = 0; i < NVOICES; i++) {
        if (voices[i].on && voices[i].chan == ch && voices[i].key == key) {
            if (chans[ch].sustain) {
                voices[i].sustained = 1;
            } else {
                voice_off(i);
            }
        }
    }
}

static void each_voice(int ch, void(*fn) (int)) {
    int i;

    for (i = 0; i < NVOICES; i++) {
        if (voices[i].on && voices[i].chan == ch) {
            fn(i);
        }
    }
}

static void freq_on(int vi) {
    set_freq(vi, 1);
}

static void s_program(int ch, int prog) {
    chans[ch].program = prog & 127;
}

static void s_control(int ch, int cc, int val) {
    struct chan *c = &chans[ch];
    int i;

    switch (cc) {
    case 7:
        c->volume = val;
        each_voice(ch, set_volume);
        break;
    case 11:
        c->expression = val;
        each_voice(ch, set_volume);
        break;
    case 10:
        c->pan = val;
        each_voice(ch, set_pan);
        break;
    case 64:
        c->sustain = val >= 64;
        if (!c->sustain) {
            for (i = 0; i < NVOICES; i++) {
                if (voices[i].on && voices[i].chan == ch && voices[i].sustained) {
                    voice_off(i);
                }
            }
        }
        break;
    case 101:
        c->rpn_msb = val;
        break;
    case 100:
        c->rpn_lsb = val;
        break;
    case 6:
        if (c->rpn_msb == 0 && c->rpn_lsb == 0) {
            c->bend_range = val > 24 ? 24 : val;
        }
        break;
    case 120:
    case 123:
        for (i = 0; i < NVOICES; i++) {
            if (voices[i].chan == ch) {
                voice_off(i);
            }
        }
        break;
    case 121:
        c->expression = 127;
        c->bend = 8192;
        c->sustain = 0;
        break;
    }
}

static void s_bend(int ch, int value) {
    chans[ch].bend = value;
    each_voice(ch, freq_on);
}

static void s_reset(void) {
    int i;

    for (i = 0; i < NVOICES; i++) {
        voice_off(i);
        voices[i].age = 0;
    }
    for (i = 0; i < 16; i++) {
        chans[i].program = 0;
        chans[i].volume = 100;
        chans[i].expression = 127;
        chans[i].pan = 64;
        chans[i].bend = 8192;
        chans[i].bend_range = 2;
        chans[i].sustain = 0;
        chans[i].rpn_msb = chans[i].rpn_lsb = 127;
    }
}

static void s_render(int *left, int *right, int n) {
    int tl[256], tr[256], i;

    while (n > 0) {
        int m = n > 256 ? 256 : n;

        memset(tl, 0, m * sizeof(int));
        memset(tr, 0, m * sizeof(int));
        opl_render(tl, tr, m, pan_l, pan_r);
        for (i = 0; i < m; i++) {
            left[i] += (tl[i] * GAIN_MUL) >> GAIN_SHIFT;
            right[i] += (tr[i] * GAIN_MUL) >> GAIN_SHIFT;
        }
        left += m;
        right += m;
        n -= m;
    }
}

static int s_voices(void) {
    int i, n = 0;

    for (i = 0; i < NVOICES; i++) {
        n += voices[i].on;
    }
    return n;
}

int synth_opl_init(const unsigned char *genmidi, long len) {
    if (!genmidi || len < 8 + 175 * 36 || memcmp(genmidi, "#OPL_II#", 8)) {
        printf("synth_opl: no GENMIDI instrument data\n");
        gm = 0;
        return -1;
    }
    gm = genmidi;
    build_tables();
    opl_init(SYNTH_RATE);
    opl_write(0x01, 0x20);
    memset(voices, 0, sizeof(voices));
    s_reset();
    return 0;
}

struct synth synth_opl = {
    "OPL FM (GENMIDI)", s_reset, s_note_on, s_note_off, s_program, s_control, s_bend,
    s_render, s_voices, NVOICES,
};
