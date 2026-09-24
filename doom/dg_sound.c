/*
 * Doom sound effects on the NC5874 box: a software mixer feeding audio.h
 * (HDMI + RCA, 48 kHz stereo). doomgeneric's sound module interface
 * (FEATURE_SOUND, i_sound.h) is implemented here as DG_sound_module.
 *
 * Effects are DMX lumps ("DSxxxx"): 8-byte header (format 3, rate,
 * length), then unsigned 8-bit mono samples with 16 padding bytes at
 * each end (skipped, as DMX does). Each channel steps through its sample
 * at rate / 48000 with linear interpolation; vol 0-127 and sep 0-254
 * (0 = left, 128 = centre) set the left / right gain.
 *
 * Only ~85 ms is kept queued in the audio ring so effects start close to
 * the action; dg_sound_pump () is called from drawing and input polling
 * as well as from Doom's I_UpdateSound, so the ring does not run dry
 * while a frame renders.
 *
 * Music (dg_music.c, OPL2 emulation) is mixed in here too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define BOX_WANT_AUDIO
#include "box.h"

#define NUM_CHANNELS    16
#define TARGET_FRAMES   4096        /* ~85 ms queued at 48 kHz */
#define MIX_CHUNK       512

/* Referenced by i_sound.c's config bindings */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

struct channel {
    const unsigned char *data;      /* NULL = free */
    u32 len;                        /* samples */
    u32 pos, step;                  /* 16.16 */
    int gain_l, gain_r;
};

static struct channel chan[NUM_CHANNELS];
static int sound_on, sfx_prefix;
static short mix_buf[MIX_CHUNK * 2];
static int acc_l[MIX_CHUNK], acc_r[MIX_CHUNK];

void dg_music_render (int *left, int *right, int n);   /* dg_music.c */
extern u32 dg_music_ticks;

u32 dg_sound_ticks;                 /* mixer time without music (CP0 Count) */

static inline u32 ticks (void) {
    u32 v;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (v));
    return v;
}

int dg_sound_active (void) {
    int c, n = 0;

    for (c = 0; c < NUM_CHANNELS; c++) {
        n += chan[c].data != 0;
    }
    return n;
}

u32 dg_audio_queued (void) {
    return sound_on ? ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME : 0;
}

void dg_sound_pump (void) {
    u32 queued, want, t0 = ticks (), m0 = dg_music_ticks;

    if (!sound_on) {
        return;
    }
    queued = ((AUD_REG (0x104) & AUD_MASK) << 3) / AUD_FRAME;
    if (queued >= TARGET_FRAMES) {
        return;
    }
    want = TARGET_FRAMES - queued;
    if (want > audio_space ()) {
        want = audio_space ();
    }
    while (want > 0) {
        u32 n = want > MIX_CHUNK ? MIX_CHUNK : want, i;
        int c;

        memset (acc_l, 0, n * sizeof (int));
        memset (acc_r, 0, n * sizeof (int));
        dg_music_render (acc_l, acc_r, n);
        for (i = 0; i < n; i++) {
            int l = acc_l[i], r = acc_r[i];

            for (c = 0; c < NUM_CHANNELS; c++) {
                struct channel *ch = &chan[c];
                u32 idx;
                int s;

                if (!ch->data) {
                    continue;
                }
                idx = ch->pos >> 16;
                if (idx + 1 >= ch->len) {
                    ch->data = 0;           /* finished */
                    continue;
                }
                s = ch->data[idx] - 128;
                s += ((ch->data[idx + 1] - 128 - s) * (int) (ch->pos & 0xffff)) >> 16;
                l += s * ch->gain_l;
                r += s * ch->gain_r;
                ch->pos += ch->step;
            }
            if (l > 32767) {
                l = 32767;
            } else if (l < -32768) {
                l = -32768;
            }
            if (r > 32767) {
                r = 32767;
            } else if (r < -32768) {
                r = -32768;
            }
            /* audio.h word = hi << 16 | lo; R = hi, L = lo (as wavplay) */
            mix_buf[2 * i] = r;
            mix_buf[2 * i + 1] = l;
        }
        audio_write (mix_buf, n);
        want -= n;
    }
    dg_sound_ticks += (ticks () - t0) - (dg_music_ticks - m0);
}

void dg_sound_stop (void) {
    if (sound_on) {
        sound_on = 0;
        audio_stop ();
    }
}

static boolean snd_init (boolean use_sfx_prefix) {
    memset (chan, 0, sizeof (chan));
    sfx_prefix = use_sfx_prefix;
    audio_start ();
    sound_on = 1;
    printf ("dg_sound: effects mixer, %d channels -> 48 kHz (HDMI + RCA)\n", NUM_CHANNELS);
    return true;
}

static void snd_shutdown (void) {
    dg_sound_stop ();
}

static int snd_get_lump (sfxinfo_t *sfx) {
    char name[16];

    if (sfx->link) {
        sfx = sfx->link;
    }
    snprintf (name, sizeof (name), sfx_prefix ? "ds%s" : "%s", sfx->name);
    return W_GetNumForName (name);
}

static void snd_update (void) {
    dg_sound_pump ();
}

static void set_params (struct channel *ch, int vol, int sep) {
    if (vol < 0) {
        vol = 0;
    } else if (vol > 127) {
        vol = 127;
    }
    if (sep < 0) {
        sep = 0;
    } else if (sep > 254) {
        sep = 254;
    }
    ch->gain_l = vol * (254 - sep) / 127;
    ch->gain_r = vol * sep / 127;
}

static void snd_update_params (int channel, int vol, int sep) {
    if (channel >= 0 && channel < NUM_CHANNELS) {
        set_params (&chan[channel], vol, sep);
    }
}

static int snd_start (sfxinfo_t *sfx, int channel, int vol, int sep) {
    const unsigned char *lump;
    struct channel *ch;
    u32 lumplen, rate, len;

    if (channel < 0 || channel >= NUM_CHANNELS) {
        return -1;
    }
    ch = &chan[channel];
    ch->data = 0;

    if (sfx->lumpnum < 0) {
        sfx->lumpnum = snd_get_lump (sfx);
    }
    lump = W_CacheLumpNum (sfx->lumpnum, PU_STATIC);
    lumplen = W_LumpLength (sfx->lumpnum);
    if (lumplen < 8 || lump[0] != 3 || lump[1] != 0) {
        return -1;
    }
    rate = lump[2] | (lump[3] << 8);
    len = lump[4] | (lump[5] << 8) | (lump[6] << 16) | ((u32) lump[7] << 24);
    if (len > lumplen - 8 || len <= 48 || rate == 0) {
        return -1;
    }

    ch->len = len - 32;             /* DMX skips 16 bytes at each end */
    ch->pos = 0;
    ch->step = (rate << 12) / 3000; /* rate * 65536 / 48000 */
    set_params (ch, vol, sep);
    ch->data = lump + 8 + 16;
    return channel;
}

static void snd_stop (int channel) {
    if (channel >= 0 && channel < NUM_CHANNELS) {
        chan[channel].data = 0;
    }
}

static boolean snd_is_playing (int channel) {
    return channel >= 0 && channel < NUM_CHANNELS && chan[channel].data != 0;
}

static void snd_cache (sfxinfo_t *sounds, int num_sounds) {
    (void) sounds;
    (void) num_sounds;              /* lumps are cached on first use (WAD is in RAM) */
}

static snddevice_t snd_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    snd_devices,
    sizeof (snd_devices) / sizeof (snd_devices[0]),
    snd_init,
    snd_shutdown,
    snd_get_lump,
    snd_update,
    snd_update_params,
    snd_start,
    snd_stop,
    snd_is_playing,
    snd_cache,
};

void I_InitTimidityConfig (void) {
}
