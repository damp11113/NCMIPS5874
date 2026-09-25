/*
 * Synth interface for the MIDI player: one MIDI-style event API, two
 * engines (synth_opl.c: OPL2/3 FM with GENMIDI patches, synth_sf2.c:
 * SoundFont 2 samples). Plain C, no box headers, so the PC test can
 * build it too. Output: 48 kHz, added to int stereo buffers.
 */
#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

#define SYNTH_RATE  48000

struct synth {
    const char *name;
    void (*reset) (void);                               /* all notes off, controllers default */
    void (*note_on) (int ch, int key, int vel);
    void (*note_off) (int ch, int key);
    void (*program) (int ch, int prog);
    void (*control) (int ch, int cc, int val);
    void (*bend) (int ch, int value);                   /* 0-16383, 8192 = centre */
    void (*render) (int *left, int *right, int n);      /* adds n samples */
    int (*voices) (void);                               /* active voices */
    int max_voices;
};

/* Engines: load their data, 0 = ok (message printed otherwise) */
int synth_opl_init (const unsigned char *genmidi, long len);
extern struct synth synth_opl;
int synth_sf2_init (unsigned char *sf2, long len);      /* keeps the buffer */
extern struct synth synth_sf2;
const char *synth_sf2_name (void);
extern int synth_sf2_limit;         /* voices allowed now (<= max_voices); more steal */

#endif
