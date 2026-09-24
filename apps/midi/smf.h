/*
 * Standard MIDI File (type 0 / 1) sequencer: all tracks merged in time
 * order, tempo changes, running status; events go to a struct synth.
 * The file stays in the caller's buffer.
 *
 *   smf_load (data, len, &synth)       0 = ok
 *   smf_render (left, right, n)        advance n samples at 48 kHz: events
 *                                      are sent exactly on time, audio
 *                                      comes from synth->render
 *   smf_done ()                        1 after the last event
 */
#ifndef SMF_H
#define SMF_H

#include <stdint.h>
#include "synth.h"

int smf_load (const unsigned char *data, long len, struct synth *s);
void smf_set_synth (struct synth *s);                   /* switch engine, restarts notes */
void smf_rewind (void);
void smf_seek (uint32_t ms);                            /* jump; instruments / controllers
                                                           are chased, notes skipped */
void smf_render (int *left, int *right, int n);
int smf_done (void);
uint32_t smf_length_ms (void);                          /* whole song */
uint32_t smf_position_ms (void);
uint32_t smf_tempo_bpm (void);
const char *smf_title (void);                           /* first track name / text */

/* Channel activity for display: last note-on velocity (decays), program */
extern int smf_chan_level[16];
extern int smf_chan_program[16];
extern int smf_chan_used[16];

#endif
