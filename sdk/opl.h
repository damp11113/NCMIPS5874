/*
 * opl: small OPL2 (YM3812) FM synth emulator, register-level.
 *
 * 9 channels x 2 operators, waveforms 0-3 (reg 0x01 bit 5 enables
 * select), feedback, FM / additive connection, ADSR envelope with the
 * datasheet attack / decay times, total level. Not emulated (kept
 * simple): tremolo / vibrato (AM / VIB bits), key-scale level and rate,
 * rhythm mode, timers. Output is rendered directly at the caller's rate.
 *
 *   opl_init (48000);
 *   opl_write (reg, val);                      as on the real chip
 *   opl_render (left, right, n, pan_l, pan_r); adds n samples to int
 *                                              buffers; pan_l/pan_r[18] are
 *                                              per-channel gains 0-256
 *                                              (a stereo extension: the
 *                                              OPL2 itself is mono)
 * One channel at full level gives about +-4095 before panning.
 */
#ifndef OPL_H
#define OPL_H

/* 9 OPL2 channels + 9 more in a second register bank (reg | 0x100) */
#define OPL_CHANNELS 18

void opl_init (int rate);
void opl_reset (void);
void opl_write (int reg, int val);
void opl_render (int *left, int *right, int n, const int *pan_l, const int *pan_r);

#endif
