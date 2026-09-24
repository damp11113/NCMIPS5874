/*
 * SoundFont 2 engine for the MIDI player.
 *
 * The whole .sf2 file stays in RAM (samples are played from it). Parsed
 * chunks (pdta): phdr presets, pbag / pgen preset zones, inst, ibag /
 * igen instrument zones, shdr samples; sdta smpl = 16-bit mono samples.
 *
 * Note on: preset = (bank, program) of the channel (bank 128 on channel
 * 10, fallback bank 0, then preset 0). Every preset zone and instrument
 * zone whose key and velocity ranges match starts a voice. Generators:
 * instrument global zone, then the instrument zone (overrides); the preset
 * global + preset zone values are added on top (SF2 2.04 rules).
 *
 * Supported: sample offsets (fine + coarse), loop modes (none, continuous,
 * until release), root key / override, coarse / fine tune, scale tuning,
 * sample pitch correction, pitch bend (+-2, RPN 0), initial attenuation,
 * pan, volume envelope (delay, attack linear, hold, decay and release
 * linear in dB, sustain level, key-number scaling of hold / decay),
 * exclusive class, CC7 / CC11 / CC10 / CC64 / CC0 bank, all notes off.
 * Not done: filters, modulation envelope, LFOs, modulators (the default
 * ones are approximated: velocity, volume and expression as 40 log10).
 *
 * Mixing: 48 voices, linear interpolation, gain updated every 32 samples.
 */
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "synth.h"

#define NVOICES     48
#define BLOCK       32
#define MASTER      11600           /* Q15 master gain (headroom for many voices) */
#define MAX_CB      1440

enum {
    G_START = 0, G_END = 1, G_LOOPSTART = 2, G_LOOPEND = 3, G_STARTCOARSE = 4,
    G_ENDCOARSE = 12, G_PAN = 17, G_DELAYENV = 33, G_ATTACKENV = 34, G_HOLDENV = 35,
    G_DECAYENV = 36, G_SUSTAINENV = 37, G_RELEASEENV = 38, G_KEYHOLD = 39, G_KEYDECAY = 40,
    G_INSTRUMENT = 41, G_KEYRANGE = 43, G_VELRANGE = 44, G_LOOPSTARTCOARSE = 45, G_KEYNUM = 46,
    G_VELOCITY = 47, G_ATTENUATION = 48, G_LOOPENDCOARSE = 50, G_COARSETUNE = 51,
    G_FINETUNE = 52, G_SAMPLEID = 53, G_SAMPLEMODES = 54, G_SCALETUNING = 56,
    G_EXCLUSIVE = 57, G_ROOTKEY = 58, G_COUNT = 61
};

struct sample {
    uint32_t start, end, loop_start, loop_end, rate;
    int root, correction, type;
};

enum { ENV_DELAY, ENV_ATTACK, ENV_HOLD, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_OFF };

struct voice {
    int active, chan, key, vel, released, sustained, exclusive;
    unsigned age;
    const short *data;
    uint32_t pos, frac, step, end, loop_start, loop_end;
    int loop_mode;
    double pitch_cents;             /* without bend */
    uint32_t rate;
    int base_cb, pan;               /* pan -500..500 */
    int env, env_t;                 /* stage, samples in stage */
    int delay_n, attack_n, hold_n, decay_n, release_n, sustain_cb;
    int env_cb, rel_step;           /* cB * 256 in decay / release */
    int gain_l, gain_r;             /* Q15 for the current block */
};

struct chan {
    int program, bank, volume, expression, pan, bend, bend_range, sustain;
    int rpn_msb, rpn_lsb;
};

static const unsigned char *phdr, *pbag, *pgen, *inst, *ibag, *igen, *shdr;
static int nphdr, npbag, npgen, ninst, nibag, nigen, nshdr;
static const short *smpl;
static uint32_t nsmpl;
static char sf_name[64];

static struct voice voices[NVOICES];
static struct chan chans[16];
static unsigned age_counter;
static unsigned short cb_gain[MAX_CB + 1];      /* Q15 amplitude of -cB */
static double cent_tab[1200];                   /* 2^(c/1200) */

static uint32_t rd16 (const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static uint32_t rd32 (const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* 2^(cents / 1200) */
static double pow2c (double cents) {
    int c = (int) (cents >= 0 ? cents + 0.5 : cents - 0.5), oct = 0;
    double r;

    while (c < 0) {
        c += 1200;
        oct--;
    }
    oct += c / 1200;
    r = cent_tab[c % 1200];
    while (oct > 0) {
        r *= 2;
        oct--;
    }
    while (oct < 0) {
        r *= 0.5;
        oct++;
    }
    return r;
}

/* Timecents -> samples at 48 kHz (-32768 = 0) */
static int tc_samples (int tc) {
    double s;

    if (tc <= -12000) {
        return 0;
    }
    s = pow2c (tc) * SYNTH_RATE;
    return s > 1e8 ? 100000000 : (int) s;
}

/* 40 log10 (127 / v) in cB, as a table-free approximation via cb_gain */
static int amp_to_cb (int v) {
    int target, lo = 0, hi = MAX_CB;

    if (v >= 127) {
        return 0;
    }
    if (v <= 0) {
        return MAX_CB;
    }
    /* amplitude (v/127)^2 in Q15 */
    target = (int) ((long long) v * v * 32767 / (127 * 127));
    while (lo < hi) {
        int mid = (lo + hi) / 2;

        if (cb_gain[mid] > target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void build_tables (void) {
    double g = 32767.0, r = 1.0, x = 0.6931471805599453 / 1200;
    double step = 1 + x + x * x / 2 + x * x * x / 6;
    int i;

    for (i = 0; i <= MAX_CB; i++) {
        cb_gain[i] = (unsigned short) g;
        g *= 0.98855309;                /* 10 ^ (-1 / 200) */
    }
    for (i = 0; i < 1200; i++) {
        cent_tab[i] = r;
        r *= step;
    }
}

/* ---- zone resolution ---- */

static const short gen_default[G_COUNT] = {
    [G_DELAYENV] = -12000, [G_ATTACKENV] = -12000, [G_HOLDENV] = -12000,
    [G_DECAYENV] = -12000, [G_RELEASEENV] = -12000, [G_KEYRANGE] = 0x7f00,
    [G_VELRANGE] = 0x7f00, [G_KEYNUM] = -1, [G_VELOCITY] = -1, [G_SCALETUNING] = 100,
    [G_ROOTKEY] = -1, [8] = 13500, [21] = -12000, [23] = -12000, [25] = -12000,
    [26] = -12000, [27] = -12000, [28] = -12000, [30] = -12000,
};

/* Apply generators [from, to) of a gen list to g (set = 1: override) */
static void apply_gens (short *g, const unsigned char *gens, int from, int to, int set) {
    int i;

    for (i = from; i < to; i++) {
        int op = rd16 (gens + i * 4);
        short amt = (short) rd16 (gens + i * 4 + 2);

        if (op >= G_COUNT) {
            continue;
        }
        if (set) {
            g[op] = amt;
        } else if (op == G_KEYRANGE || op == G_VELRANGE) {
            g[op] = amt;                        /* ranges are checked, not added */
        } else {
            g[op] += amt;
        }
    }
}

static int zone_last_gen (const unsigned char *bags, const unsigned char *gens, int ngens, int b) {
    int from = rd16 (bags + b * 4), to = rd16 (bags + (b + 1) * 4);

    if (to <= from || to > ngens) {
        return -1;
    }
    return rd16 (gens + (to - 1) * 4);
}

static int in_range (short range, int v) {
    int lo = range & 0xff, hi = (range >> 8) & 0xff;

    return v >= lo && v <= hi;
}

static int find_preset (int bank, int prog) {
    int i;

    for (i = 0; i < nphdr - 1; i++) {
        if ((int) rd16 (phdr + i * 38 + 20) == prog && (int) rd16 (phdr + i * 38 + 22) == bank) {
            return i;
        }
    }
    return -1;
}

static void start_voice (int ch, int key, int vel, const short *g);

static void play_zones (int ch, int key, int vel, int preset) {
    int pb0 = rd16 (phdr + preset * 38 + 24), pb1 = rd16 (phdr + (preset + 1) * 38 + 24);
    short pglobal[G_COUNT];
    int b;

    memset (pglobal, 0, sizeof (pglobal));
    pglobal[G_KEYRANGE] = pglobal[G_VELRANGE] = 0x7f00;
    if (pb1 - pb0 > 1 && zone_last_gen (pbag, pgen, npgen, pb0) != G_INSTRUMENT) {
        apply_gens (pglobal, pgen, rd16 (pbag + pb0 * 4), rd16 (pbag + (pb0 + 1) * 4), 1);
        pb0++;
    }
    for (b = pb0; b < pb1 && b < npbag - 1; b++) {
        short pz[G_COUNT];
        int ins, ib0, ib1, ib;
        short iglobal[G_COUNT];

        memcpy (pz, pglobal, sizeof (pz));
        apply_gens (pz, pgen, rd16 (pbag + b * 4), rd16 (pbag + (b + 1) * 4), 1);
        if (zone_last_gen (pbag, pgen, npgen, b) != G_INSTRUMENT ||
            !in_range (pz[G_KEYRANGE], key) || !in_range (pz[G_VELRANGE], vel)) {
            continue;
        }
        ins = pz[G_INSTRUMENT];
        if (ins < 0 || ins >= ninst - 1) {
            continue;
        }
        ib0 = rd16 (inst + ins * 22 + 20);
        ib1 = rd16 (inst + (ins + 1) * 22 + 20);
        memcpy (iglobal, gen_default, sizeof (iglobal));
        if (ib1 - ib0 > 1 && zone_last_gen (ibag, igen, nigen, ib0) != G_SAMPLEID) {
            apply_gens (iglobal, igen, rd16 (ibag + ib0 * 4), rd16 (ibag + (ib0 + 1) * 4), 1);
            ib0++;
        }
        for (ib = ib0; ib < ib1 && ib < nibag - 1; ib++) {
            short g[G_COUNT];
            int k;

            if (zone_last_gen (ibag, igen, nigen, ib) != G_SAMPLEID) {
                continue;
            }
            memcpy (g, iglobal, sizeof (g));
            apply_gens (g, igen, rd16 (ibag + ib * 4), rd16 (ibag + (ib + 1) * 4), 1);
            if (!in_range (g[G_KEYRANGE], key) || !in_range (g[G_VELRANGE], vel)) {
                continue;
            }
            /* Preset values add to the instrument's (not for sample / range gens) */
            for (k = 0; k < G_COUNT; k++) {
                if (k <= 4 || k == 12 || k == 41 || k == 43 || k == 44 || k == 45 ||
                    k == 46 || k == 47 || k == 50 || k == 53 || k == 54 || k == 57 || k == 58) {
                    continue;
                }
                g[k] += pz[k];
            }
            start_voice (ch, key, vel, g);
        }
    }
}

/* ---- voices ---- */

static void update_step (struct voice *v) {
    const struct chan *c = &chans[v->chan];
    double cents = v->pitch_cents + (c->bend - 8192) * c->bend_range * 100.0 / 8192;
    double ratio = pow2c (cents) * v->rate / SYNTH_RATE;

    v->step = (uint32_t) (ratio * 65536.0);
}

static int chan_cb (int ch) {
    return amp_to_cb (chans[ch].volume) + amp_to_cb (chans[ch].expression);
}

static int alloc_voice (void) {
    int i, best = -1, best_cb = -1;
    unsigned best_age = ~0u;

    for (i = 0; i < NVOICES; i++) {
        if (!voices[i].active) {
            return i;
        }
    }
    for (i = 0; i < NVOICES; i++) {             /* quietest released voice */
        if (voices[i].released && voices[i].env_cb > best_cb) {
            best = i;
            best_cb = voices[i].env_cb;
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
    return best;
}

static void start_voice (int ch, int key, int vel, const short *g) {
    const unsigned char *sh;
    struct sample s;
    struct voice *v;
    int root, k, vl, i;
    uint32_t start, end, ls, le;

    if (g[G_SAMPLEID] < 0 || g[G_SAMPLEID] >= nshdr - 1) {
        return;
    }
    sh = shdr + g[G_SAMPLEID] * 46;
    s.start = rd32 (sh + 20);
    s.end = rd32 (sh + 24);
    s.loop_start = rd32 (sh + 28);
    s.loop_end = rd32 (sh + 32);
    s.rate = rd32 (sh + 36);
    s.root = sh[40];
    s.correction = (signed char) sh[41];
    s.type = rd16 (sh + 44);
    if (s.type & 0x8000 || s.rate == 0) {
        return;                                 /* ROM sample */
    }
    start = s.start + g[G_START] + g[G_STARTCOARSE] * 32768;
    end = s.end + g[G_END] + g[G_ENDCOARSE] * 32768;
    ls = s.loop_start + g[G_LOOPSTART] + g[G_LOOPSTARTCOARSE] * 32768;
    le = s.loop_end + g[G_LOOPEND] + g[G_LOOPENDCOARSE] * 32768;
    if (end > nsmpl) {
        end = nsmpl;
    }
    if (start >= end) {
        return;
    }

    if (g[G_EXCLUSIVE]) {                       /* e.g. open / closed hi-hat */
        for (i = 0; i < NVOICES; i++) {
            if (voices[i].active && voices[i].chan == ch && voices[i].exclusive == g[G_EXCLUSIVE]) {
                voices[i].active = 0;
            }
        }
    }

    v = &voices[alloc_voice ()];
    memset (v, 0, sizeof (*v));
    k = g[G_KEYNUM] >= 0 ? g[G_KEYNUM] : key;
    vl = g[G_VELOCITY] >= 0 ? g[G_VELOCITY] : vel;
    root = g[G_ROOTKEY] >= 0 ? g[G_ROOTKEY] : (s.root > 127 ? 60 : s.root);

    v->active = 1;
    v->chan = ch;
    v->key = key;
    v->vel = vl;
    v->age = ++age_counter;
    v->exclusive = g[G_EXCLUSIVE];
    v->data = smpl;
    v->pos = start;
    v->end = end;
    v->loop_start = ls;
    v->loop_end = le;
    v->loop_mode = (g[G_SAMPLEMODES] & 3);
    if (v->loop_mode == 2 || le <= ls || le > end) {
        v->loop_mode = 0;
    }
    v->rate = s.rate;
    v->pitch_cents = (k - root) * g[G_SCALETUNING] + g[G_COARSETUNE] * 100 + g[G_FINETUNE] +
                     s.correction;
    update_step (v);

    /* Initial attenuation x 0.4, as FluidSynth (ALT_ATTENUATION_SCALE) and
     * the EMU hardware most SoundFonts were tuned on: with the plain cB of
     * the spec, e.g. Yamaha MA2 came out 18 dB quieter than E-mu's set. */
    v->base_cb = g[G_ATTENUATION] * 2 / 5 + amp_to_cb (vl) + chan_cb (ch);
    if (v->base_cb < 0) {
        v->base_cb = 0;
    }
    v->pan = g[G_PAN] + (chans[ch].pan - 64) * 1000 / 127;
    v->pan = v->pan < -500 ? -500 : v->pan > 500 ? 500 : v->pan;

    v->delay_n = tc_samples (g[G_DELAYENV]);
    v->attack_n = tc_samples (g[G_ATTACKENV]);
    v->hold_n = tc_samples (g[G_HOLDENV] + g[G_KEYHOLD] * (60 - k));
    v->decay_n = tc_samples (g[G_DECAYENV] + g[G_KEYDECAY] * (60 - k));
    v->release_n = tc_samples (g[G_RELEASEENV]);
    v->sustain_cb = g[G_SUSTAINENV] < 0 ? 0 : g[G_SUSTAINENV] > 1440 ? 1440 : g[G_SUSTAINENV];
    v->env = ENV_DELAY;
    v->env_t = 0;
    v->env_cb = 0;
}

static void release_voice (struct voice *v) {
    if (v->released) {
        return;
    }
    v->released = 1;
    if (v->env == ENV_DELAY) {
        v->active = 0;
        return;
    }
    if (v->env == ENV_ATTACK) {                 /* from the current linear level */
        int lin = v->attack_n ? (int) ((long long) v->env_t * 32767 / v->attack_n) : 32767;

        v->env_cb = amp_to_cb ((int) (127 * (long long) lin / 32767)) / 2 * 256;
    }
    v->env = ENV_RELEASE;
    v->env_t = 0;
    /* 100 dB (1000 cB) over the release time */
    v->rel_step = v->release_n ? (int) (1000LL * 256 / v->release_n) : 1000 * 256;
    if (v->rel_step < 1) {
        v->rel_step = 1;
    }
}

/* Envelope for the next BLOCK samples -> gain_l / gain_r */
static void envelope (struct voice *v) {
    int cb, lin = 32767;

    switch (v->env) {
    case ENV_DELAY:
        if (v->env_t >= v->delay_n) {
            v->env = ENV_ATTACK;
            v->env_t = 0;
        } else {
            v->gain_l = v->gain_r = 0;
            v->env_t += BLOCK;
            return;
        }
        /* fall through */
    case ENV_ATTACK:
        if (v->env_t >= v->attack_n) {
            v->env = ENV_HOLD;
            v->env_t = 0;
        } else {
            lin = (int) ((long long) v->env_t * 32767 / v->attack_n);
            break;
        }
        /* fall through */
    case ENV_HOLD:
        if (v->env_t >= v->hold_n) {
            v->env = ENV_DECAY;
            v->env_t = 0;
            v->env_cb = 0;
        } else {
            break;
        }
        /* fall through */
    case ENV_DECAY:
        if (v->decay_n) {
            v->env_cb += (int) (1000LL * 256 * BLOCK / v->decay_n);
        } else {
            v->env_cb = v->sustain_cb * 256;
        }
        if (v->env_cb >= v->sustain_cb * 256) {
            v->env_cb = v->sustain_cb * 256;
            v->env = ENV_SUSTAIN;
        }
        break;
    case ENV_SUSTAIN:
        break;
    case ENV_RELEASE:
        v->env_cb += v->rel_step * BLOCK;
        if (v->env_cb >= 960 * 256) {
            v->active = 0;
            v->gain_l = v->gain_r = 0;
            return;
        }
        break;
    }
    v->env_t += BLOCK;

    cb = v->base_cb + (v->env == ENV_ATTACK || v->env == ENV_HOLD ? 0 : v->env_cb / 256);
    if (v->env == ENV_RELEASE && cb >= 700) {
        v->active = 0;                          /* 70 dB down: inaudible, free it */
        v->gain_l = v->gain_r = 0;
        return;
    }
    if (cb >= MAX_CB) {
        v->gain_l = v->gain_r = 0;
        return;
    }
    if (cb < 0) {
        cb = 0;
    }
    lin = (int) ((long long) cb_gain[cb] * lin / 32767 * MASTER / 32767);
    /* Pan: centre = full on both sides (as the OPL engine) */
    v->gain_l = v->pan <= 0 ? lin : (int) ((long long) lin * (500 - v->pan) / 500);
    v->gain_r = v->pan >= 0 ? lin : (int) ((long long) lin * (500 + v->pan) / 500);
}

static void mix_voice (struct voice *v, int *left, int *right, int n) {
    const short *d = v->data;
    int i;

    for (i = 0; i < n; i++) {
        uint32_t p = v->pos;
        int a, b, s;

        if (p + 1 >= v->end) {
            v->active = 0;
            return;
        }
        a = d[p];
        b = d[p + 1];
        s = a + (((b - a) * (int) v->frac) >> 16);
        left[i] += (s * v->gain_l) >> 15;
        right[i] += (s * v->gain_r) >> 15;

        v->frac += v->step;
        v->pos += v->frac >> 16;
        v->frac &= 0xffff;
        if (v->loop_mode && (v->loop_mode == 1 || !v->released) && v->pos >= v->loop_end) {
            v->pos -= v->loop_end - v->loop_start;
        }
    }
}

/* ---- synth interface ---- */

static void s_note_on (int ch, int key, int vel) {
    int p, bank = ch == 9 ? 128 : chans[ch].bank;

    if (!phdr) {
        return;
    }
    p = find_preset (bank, chans[ch].program);
    if (p < 0) {
        p = find_preset (ch == 9 ? 128 : 0, ch == 9 ? 0 : chans[ch].program);
    }
    if (p < 0) {
        p = find_preset (0, 0);
    }
    if (p >= 0) {
        play_zones (ch, key, vel, p);
    }
}

static void s_note_off (int ch, int key) {
    int i;

    for (i = 0; i < NVOICES; i++) {
        struct voice *v = &voices[i];

        if (v->active && v->chan == ch && v->key == key && !v->released) {
            if (chans[ch].sustain) {
                v->sustained = 1;
            } else {
                release_voice (v);
            }
        }
    }
}

static void s_program (int ch, int prog) {
    chans[ch].program = prog & 127;
}

static void s_control (int ch, int cc, int val) {
    struct chan *c = &chans[ch];
    int i, old = chan_cb (ch);

    switch (cc) {
    case 0:
        c->bank = val;
        break;
    case 7:
    case 11:
        if (cc == 7) {
            c->volume = val;
        } else {
            c->expression = val;
        }
        for (i = 0; i < NVOICES; i++) {
            if (voices[i].active && voices[i].chan == ch) {
                voices[i].base_cb += chan_cb (ch) - old;
            }
        }
        break;
    case 10:
        c->pan = val;
        break;
    case 64:
        c->sustain = val >= 64;
        if (!c->sustain) {
            for (i = 0; i < NVOICES; i++) {
                if (voices[i].active && voices[i].chan == ch && voices[i].sustained) {
                    voices[i].sustained = 0;
                    release_voice (&voices[i]);
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
        for (i = 0; i < NVOICES; i++) {
            if (voices[i].chan == ch) {
                voices[i].active = 0;
            }
        }
        break;
    case 123:
        for (i = 0; i < NVOICES; i++) {
            if (voices[i].active && voices[i].chan == ch) {
                release_voice (&voices[i]);
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

static void s_bend (int ch, int value) {
    int i;

    chans[ch].bend = value;
    for (i = 0; i < NVOICES; i++) {
        if (voices[i].active && voices[i].chan == ch) {
            update_step (&voices[i]);
        }
    }
}

static void s_reset (void) {
    int i;

    memset (voices, 0, sizeof (voices));
    for (i = 0; i < 16; i++) {
        chans[i].program = 0;
        chans[i].bank = 0;
        chans[i].volume = 100;
        chans[i].expression = 127;
        chans[i].pan = 64;
        chans[i].bend = 8192;
        chans[i].bend_range = 2;
        chans[i].sustain = 0;
        chans[i].rpn_msb = chans[i].rpn_lsb = 127;
    }
}

static void s_render (int *left, int *right, int n) {
    while (n > 0) {
        int m = n > BLOCK ? BLOCK : n, i;

        for (i = 0; i < NVOICES; i++) {
            struct voice *v = &voices[i];

            if (!v->active) {
                continue;
            }
            envelope (v);
            if (v->active && v->env != ENV_DELAY) {
                mix_voice (v, left, right, m);
            }
        }
        left += m;
        right += m;
        n -= m;
    }
}

static int s_voices (void) {
    int i, n = 0;

    for (i = 0; i < NVOICES; i++) {
        n += voices[i].active;
    }
    return n;
}

const char *synth_sf2_name (void) {
    return sf_name;
}

int synth_sf2_init (unsigned char *sf, long len) {
    long pos = 12;

    phdr = pbag = pgen = inst = ibag = igen = shdr = 0;
    smpl = 0;
    sf_name[0] = 0;
    if (len < 12 || memcmp (sf, "RIFF", 4) || memcmp (sf + 8, "sfbk", 4)) {
        printf ("synth_sf2: not a SoundFont 2 file\n");
        return -1;
    }
    build_tables ();
    while (pos + 12 <= len) {
        uint32_t clen = rd32 (sf + pos + 4);
        long lpos = pos + 12, lend = pos + 8 + clen;

        if (memcmp (sf + pos, "LIST", 4)) {
            pos += 8 + ((clen + 1) & ~1u);
            continue;
        }
        if (lend > len) {
            lend = len;
        }
        while (lpos + 8 <= lend) {
            const unsigned char *id = sf + lpos, *d = sf + lpos + 8;
            uint32_t sz = rd32 (sf + lpos + 4);

            if (lpos + 8 + (long) sz > lend) {
                sz = lend - lpos - 8;
            }
            if (!memcmp (id, "INAM", 4)) {
                uint32_t k;

                for (k = 0; k < sz && k < sizeof (sf_name) - 1 && d[k]; k++) {
                    sf_name[k] = (d[k] >= 32 && d[k] < 127) ? d[k] : '?';
                }
                sf_name[k] = 0;
            } else if (!memcmp (id, "smpl", 4)) {
                smpl = (const short *) d;
                nsmpl = sz / 2;
            } else if (!memcmp (id, "phdr", 4)) {
                phdr = d;
                nphdr = sz / 38;
            } else if (!memcmp (id, "pbag", 4)) {
                pbag = d;
                npbag = sz / 4;
            } else if (!memcmp (id, "pgen", 4)) {
                pgen = d;
                npgen = sz / 4;
            } else if (!memcmp (id, "inst", 4)) {
                inst = d;
                ninst = sz / 22;
            } else if (!memcmp (id, "ibag", 4)) {
                ibag = d;
                nibag = sz / 4;
            } else if (!memcmp (id, "igen", 4)) {
                igen = d;
                nigen = sz / 4;
            } else if (!memcmp (id, "shdr", 4)) {
                shdr = d;
                nshdr = sz / 46;
            }
            lpos += 8 + ((sz + 1) & ~1u);
        }
        pos += 8 + ((clen + 1) & ~1u);
    }
    if (!phdr || !pbag || !pgen || !inst || !ibag || !igen || !shdr || !smpl || nphdr < 2) {
        printf ("synth_sf2: incomplete SoundFont\n");
        phdr = 0;
        return -1;
    }
    if (((uintptr_t) smpl) & 1) {
        printf ("synth_sf2: sample data not 16-bit aligned\n");
        phdr = 0;
        return -1;
    }
    s_reset ();
    printf ("synth_sf2: \"%s\", %d presets, %d instruments, %d samples, %d KB sample data\n",
            sf_name, nphdr - 1, ninst - 1, nshdr - 1, nsmpl * 2 / 1024);
    return 0;
}

struct synth synth_sf2 = {
    "SoundFont 2", s_reset, s_note_on, s_note_off, s_program, s_control, s_bend,
    s_render, s_voices, NVOICES,
};
