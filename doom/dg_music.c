/*
 * Doom music on the NC5874 box: MUS player driving the OPL2 emulator
 * (opl.c) with Doom's own instrument set (GENMIDI lump), as the DOS
 * version did on an AdLib / Sound Blaster. doomgeneric's music module
 * interface (i_sound.h) is implemented here as DG_music_module.
 *
 * MUS: events on 16 channels (15 = percussion), delays in 1/140 s ticks.
 *   byte: bit 7 = delay follows, bits 4-6 type, bits 0-3 channel
 *   0 release note, 1 play note (+ volume if note bit 7), 2 pitch bend
 *   (128 = centre, +-2 semitones), 3 system event (10/11 = sounds / notes
 *   off, 14 = reset controllers), 4 controller (0 instrument, 3 volume,
 *   4 pan, 5 expression), 5 end of measure, 6 score end.
 * GENMIDI: "#OPL_II#", then 175 x 36-byte instruments (128 melodic,
 *   47 percussion for notes 35-81): flags (1 = fixed note), fine tune,
 *   fixed note, 2 voices of 16 bytes: modulator (tremolo/mult 0x20,
 *   attack 0x60, sustain 0x80, waveform 0xE0, key scale 0x40 hi, level
 *   0x40 lo), feedback/connection 0xC0, carrier (same 6), unused,
 *   base note offset (int16). Only voice 1 is used (no double voices).
 *
 * 9 OPL voices, oldest-first stealing. Volume = note velocity x channel
 * volume x expression x music volume, applied as extra carrier (and, for
 * additive instruments, modulator) attenuation in dB. MUS pan gives each
 * voice a left / right gain (the OPL2 is mono; stereo is our addition).
 * dg_music_render () is called by dg_sound.c's mixer, which advances the
 * score at 140 ticks per second of rendered audio.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "opl.h"

typedef unsigned int u32;

#define RATE            48000
#define TICK_HZ         140
#define NUM_VOICES      9
#define MUSIC_GAIN      2           /* OPL channel +-4095 -> mix scale */

struct voice {
    int on;                         /* key held */
    int chan, note;                 /* MUS channel / note that started it */
    int play_note;                  /* note sent to the OPL (after offsets) */
    int velocity;
    unsigned age;
    const unsigned char *ins;       /* GENMIDI instrument */
};

struct mchan {
    int program, volume, expression, pan, bend, last_vel;
};

static const unsigned char *genmidi;    /* lump, NULL = no music */
static struct voice voices[NUM_VOICES];
static struct mchan chans[16];
static int pan_l[OPL_CHANNELS], pan_r[OPL_CHANNELS];
static unsigned age_counter;

static const unsigned char *song, *score, *score_end, *mus_pos;
static int playing, paused, looping, music_volume = 100;
static int delay_ticks, tick_acc;
static unsigned short fnum_tab[384];    /* note 60 + k/32 semitone, block 4 */
static unsigned char vol_atten[128];    /* amplitude 0-127 -> TL steps (0.75 dB) */

static const unsigned char op_off[NUM_VOICES] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

static int le16 (const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

/* ---- tables ---- */

static void build_tables (void) {
    double f = 261.6255653, r, x = 0.6931471805599453 / 384;   /* ln 2 / 384 */
    double amp = 127.0;
    int i, a;

    r = 1 + x + x * x / 2 + x * x * x / 6;                      /* 2 ^ (1/384) */
    for (i = 0; i < 384; i++) {
        fnum_tab[i] = (unsigned short) (f * 65536.0 / 49716.0 + 0.5);   /* block 4 */
        f *= r;
    }
    /* vol_atten[v] = steps of 0.75 dB so that 127 * 10^(-steps*0.75/20) ~ v */
    vol_atten[0] = 63;
    for (a = 0, i = 127; i > 0; i--) {
        while (amp * 0.9173 >= i && a < 63) {   /* 10 ^ (-0.75 / 20) */
            amp *= 0.9173;
            a++;
        }
        vol_atten[i] = a;
    }
}

/* ---- OPL voice programming ---- */

static void write_op (int voice, int which, const unsigned char *op, int level) {
    int off = op_off[voice] + (which ? 3 : 0);

    opl_write (0x20 + off, op[0]);
    opl_write (0x60 + off, op[1]);
    opl_write (0x80 + off, op[2]);
    opl_write (0xe0 + off, op[3]);
    opl_write (0x40 + off, op[4] | level);
}

static int voice_amp (const struct voice *v) {
    const struct mchan *c = &chans[v->chan];
    int amp = v->velocity * c->volume / 127;

    amp = amp * c->expression / 127;
    return amp * music_volume / 127;
}

/* Carrier (and additive modulator) level with the voice's volume applied */
static void set_voice_volume (int vi) {
    struct voice *v = &voices[vi];
    const unsigned char *vo = v->ins + 4;
    int att = vol_atten[voice_amp (v)];
    int car = (vo[7 + 5] & 0x3f) + att;

    if (car > 63) {
        car = 63;
    }
    opl_write (0x40 + op_off[vi] + 3, vo[7 + 4] | car);
    if (vo[6] & 1) {                /* additive: modulator is heard too */
        int mod = (vo[5] & 0x3f) + att;

        opl_write (0x40 + op_off[vi], vo[4] | (mod > 63 ? 63 : mod));
    }
}

static void set_voice_pan (int vi) {
    int pan = chans[voices[vi].chan].pan;       /* 0 left .. 64 centre .. 127 right */

    pan_l[vi] = pan <= 64 ? 256 : (127 - pan) * 256 / 63;
    pan_r[vi] = pan >= 64 ? 256 : pan * 256 / 64;
}

static void set_voice_freq (int vi, int key) {
    struct voice *v = &voices[vi];
    int p = v->play_note * 32 + (chans[v->chan].bend - 128) / 2;   /* 1/32 semitone */
    int k, oct, block, fnum;

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
    opl_write (0xa0 + vi, fnum & 0xff);
    opl_write (0xb0 + vi, (key ? 0x20 : 0) | (block << 2) | (fnum >> 8));
}

static void voice_off (int vi) {
    struct voice *v = &voices[vi];

    if (v->on) {
        v->on = 0;
        v->age = ++age_counter;
        set_voice_freq (vi, 0);
    }
}

static int alloc_voice (void) {
    int i, best = -1;
    unsigned best_age = ~0u;

    for (i = 0; i < NUM_VOICES; i++) {          /* oldest released voice */
        if (!voices[i].on && voices[i].age < best_age) {
            best = i;
            best_age = voices[i].age;
        }
    }
    if (best >= 0) {
        return best;
    }
    for (i = 0; i < NUM_VOICES; i++) {          /* else steal the oldest */
        if (voices[i].age < best_age) {
            best = i;
            best_age = voices[i].age;
        }
    }
    voice_off (best);
    return best;
}

static void note_on (int chan, int note, int vel) {
    const unsigned char *ins, *vo;
    struct voice *v;
    int vi, play;

    if (chan == 15) {
        if (note < 35 || note > 81) {
            return;
        }
        ins = genmidi + 8 + (128 + note - 35) * 36;
    } else {
        ins = genmidi + 8 + chans[chan].program * 36;
    }
    vo = ins + 4;
    play = (le16 (ins) & 1) ? ins[3] : note;
    play += (short) le16 (vo + 14);
    if (play < 0) {
        play = 0;
    } else if (play > 127) {
        play = 127;
    }

    vi = alloc_voice ();
    v = &voices[vi];
    set_voice_freq (vi, 0);                     /* key off before re-programming */
    v->on = 1;
    v->chan = chan;
    v->note = note;
    v->play_note = play;
    v->velocity = vel;
    v->age = ++age_counter;
    v->ins = ins;

    write_op (vi, 0, vo, vo[5] & 0x3f);
    write_op (vi, 1, vo + 7, vo[7 + 5] & 0x3f);
    opl_write (0xc0 + vi, vo[6]);
    set_voice_volume (vi);
    set_voice_pan (vi);
    set_voice_freq (vi, 1);
}

static void note_off (int chan, int note) {
    int i;

    for (i = 0; i < NUM_VOICES; i++) {
        if (voices[i].on && voices[i].chan == chan && voices[i].note == note) {
            voice_off (i);
        }
    }
}

static void all_notes_off (int chan) {
    int i;

    for (i = 0; i < NUM_VOICES; i++) {
        if (chan < 0 || voices[i].chan == chan) {
            voice_off (i);
        }
    }
}

static void update_chan (int chan, int what) {
    int i;

    for (i = 0; i < NUM_VOICES; i++) {
        if (voices[i].on && voices[i].chan == chan) {
            if (what == 0) {
                set_voice_volume (i);
            } else if (what == 1) {
                set_voice_pan (i);
            } else {
                set_voice_freq (i, 1);
            }
        }
    }
}

static void reset_chans (void) {
    int i;

    for (i = 0; i < 16; i++) {
        chans[i].program = 0;
        chans[i].volume = 100;
        chans[i].expression = 127;
        chans[i].pan = 64;
        chans[i].bend = 128;
        chans[i].last_vel = 127;
    }
}

static void silence (void) {
    int i;

    for (i = 0; i < NUM_VOICES; i++) {
        voice_off (i);
    }
}

/* ---- score ---- */

static int next_byte (void) {
    return mus_pos < score_end ? *mus_pos++ : 0;
}

/* Run events until one asks for a delay; returns 0 at score end */
static int run_events (void) {
    for (;;) {
        int ev, type, chan, b, last;

        if (mus_pos >= score_end) {
            return 0;
        }
        ev = next_byte ();
        type = (ev >> 4) & 7;
        chan = ev & 15;
        last = ev & 0x80;

        switch (type) {
        case 0:
            note_off (chan, next_byte () & 0x7f);
            break;
        case 1:
            b = next_byte ();
            if (b & 0x80) {
                chans[chan].last_vel = next_byte () & 0x7f;
            }
            note_on (chan, b & 0x7f, chans[chan].last_vel);
            break;
        case 2:
            chans[chan].bend = next_byte ();
            update_chan (chan, 2);
            break;
        case 3:
            b = next_byte ();
            if (b == 10 || b == 11) {
                all_notes_off (chan);
            } else if (b == 14) {
                chans[chan].volume = 100;
                chans[chan].expression = 127;
                chans[chan].pan = 64;
                chans[chan].bend = 128;
            }
            break;
        case 4:
            b = next_byte ();
            {
                int val = next_byte () & 0x7f;

                if (b == 0) {
                    chans[chan].program = val;
                } else if (b == 3) {
                    chans[chan].volume = val;
                    update_chan (chan, 0);
                } else if (b == 4) {
                    chans[chan].pan = val;
                    update_chan (chan, 1);
                } else if (b == 5) {
                    chans[chan].expression = val;
                    update_chan (chan, 0);
                }
            }
            break;
        case 5:
            break;
        case 6:
            return 0;
        default:
            break;
        }
        if (last) {
            int d = 0;

            do {
                b = next_byte ();
                d = (d << 7) | (b & 0x7f);
            } while ((b & 0x80) && mus_pos < score_end);
            if (d > 0) {
                delay_ticks = d;
                return 1;
            }
        }
    }
}

static void restart (void) {
    mus_pos = score;
    delay_ticks = 0;
    reset_chans ();
}

static void music_tick (void) {
    int restarts = 0;

    if (!playing || paused) {
        return;
    }
    if (delay_ticks > 0 && --delay_ticks > 0) {
        return;
    }
    while (delay_ticks == 0) {
        if (!run_events ()) {
            silence ();
            if (looping && restarts++ == 0) {
                restart ();     /* a score without any delay stops below */
                continue;
            }
            playing = 0;
            return;
        }
    }
}

u32 dg_music_ticks;                 /* OPL + score time (CP0 Count), for dg_debug.c */

/* CP0 Count (0 when built for the PC test) */
static inline u32 count_now (void) {
    u32 v = 0;

#ifdef __mips__
    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
#endif
    return v;
}

int dg_music_voices (void) {
    int i, n = 0;

    for (i = 0; i < NUM_VOICES; i++) {
        n += voices[i].on;
    }
    return n;
}

int dg_music_playing (void) {
    return playing && !paused;
}

/* Add n samples of music to the mixer's int buffers (dg_sound.c) */
void dg_music_render (int *left, int *right, int n) {
    u32 t0, t1;
    int i;

    if (!genmidi) {
        return;
    }
    t0 = count_now ();
    while (n > 0) {
        /* samples until the next 1/140 s tick */
        int seg = (RATE - tick_acc + TICK_HZ - 1) / TICK_HZ;

        if (seg > n) {
            seg = n;
        }
        if (seg > 0) {
            int tmp_l[256], tmp_r[256];
            int done = 0;

            while (done < seg) {
                int m = seg - done > 256 ? 256 : seg - done;

                memset (tmp_l, 0, m * sizeof (int));
                memset (tmp_r, 0, m * sizeof (int));
                opl_render (tmp_l, tmp_r, m, pan_l, pan_r);
                for (i = 0; i < m; i++) {
                    left[done + i] += tmp_l[i] * MUSIC_GAIN;
                    right[done + i] += tmp_r[i] * MUSIC_GAIN;
                }
                done += m;
            }
            left += seg;
            right += seg;
            n -= seg;
        }
        tick_acc += seg * TICK_HZ;
        if (tick_acc >= RATE) {
            tick_acc -= RATE;
            music_tick ();
        }
    }
    t1 = count_now ();
    dg_music_ticks += t1 - t0;
}

/* ---- DG_music_module ---- */

static boolean mus_init (void) {
    int lump = W_CheckNumForName ("GENMIDI");

    if (lump < 0 || W_LumpLength (lump) < 8 + 175 * 36) {
        printf ("dg_music: no GENMIDI lump, music off\n");
        return false;
    }
    genmidi = W_CacheLumpNum (lump, PU_STATIC);
    if (memcmp (genmidi, "#OPL_II#", 8) != 0) {
        printf ("dg_music: bad GENMIDI lump, music off\n");
        genmidi = 0;
        return false;
    }
    build_tables ();
    opl_init (RATE);
    opl_write (0x01, 0x20);         /* waveform select on */
    memset (voices, 0, sizeof (voices));
    reset_chans ();
    printf ("dg_music: OPL2 emulation, %d voices, GENMIDI instruments\n", NUM_VOICES);
    return true;
}

static void mus_shutdown (void) {
    playing = 0;
    silence ();
}

static void mus_set_volume (int volume) {
    int i;

    music_volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    for (i = 0; i < NUM_VOICES; i++) {
        if (voices[i].on) {
            set_voice_volume (i);
        }
    }
}

static void mus_pause (void) {
    paused = 1;
    silence ();
}

static void mus_resume (void) {
    paused = 0;
}

static void *mus_register (void *data, int len) {
    const unsigned char *d = data;

    if (len < 16 || memcmp (d, "MUS\x1a", 4) != 0) {
        printf ("dg_music: not a MUS lump (MIDI music is not supported)\n");
        return 0;
    }
    return data;
}

static void mus_unregister (void *handle) {
    (void) handle;
}

static void mus_play (void *handle, boolean loop) {
    const unsigned char *d = handle;
    int len, start;

    if (!d || !genmidi) {
        return;
    }
    len = le16 (d + 4);
    start = le16 (d + 6);
    silence ();
    song = d;
    score = d + start;
    score_end = score + len;
    looping = loop;
    paused = 0;
    tick_acc = 0;
    restart ();
    playing = 1;
}

static void mus_stop (void) {
    playing = 0;
    silence ();
}

static boolean mus_is_playing (void) {
    return playing;
}

static void mus_poll (void) {
}

static snddevice_t music_devices[] = {
    SNDDEVICE_ADLIB, SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

music_module_t DG_music_module = {
    music_devices,
    sizeof (music_devices) / sizeof (music_devices[0]),
    mus_init,
    mus_shutdown,
    mus_set_volume,
    mus_pause,
    mus_resume,
    mus_register,
    mus_unregister,
    mus_play,
    mus_stop,
    mus_is_playing,
    mus_poll,
};
