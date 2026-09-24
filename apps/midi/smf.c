/*
 * Standard MIDI File sequencer, see smf.h.
 *
 * Time: samples per tick = tempo (us per quarter) * 48000 / 1e6 / division,
 * kept in 32.32 fixed point; the sample time of the current tick is
 * advanced whenever events are processed, so tempo changes are exact.
 */
#include <string.h>
#include <stdio.h>

#include "smf.h"

#define MAX_TRACKS 64

struct track {
    const unsigned char *p, *end;
    uint32_t next_tick;
    int status, done;
};

static const unsigned char *file;
static long file_len;
static struct track tracks[MAX_TRACKS];
static int ntracks, division;
static struct synth *syn;

static uint32_t tempo;              /* us per quarter note */
static uint64_t spt;                /* samples per tick, 32.32 */
static uint32_t cur_tick;
static uint64_t tick_time, now;     /* sample times, 32.32 */
static int finished;
static uint32_t length_ms;
static char title[64];

int smf_chan_level[16], smf_chan_program[16], smf_chan_used[16];
static int chan_cc[16][128], chan_bend[16];

static uint32_t be32 (const unsigned char *p) {
    return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint32_t vlq (struct track *t) {
    uint32_t v = 0;
    int i;

    for (i = 0; i < 4 && t->p < t->end; i++) {
        unsigned char b = *t->p++;

        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) {
            break;
        }
    }
    return v;
}

static void set_tempo (uint32_t us) {
    tempo = us ? us : 500000;
    /* tempo * 48000 / 1e6 / division in 32.32. Scale by 48 / 1000 (not
     * 48000 / 1e6) first: tempo * 48000 << 32 overflows 64 bits for normal
     * tempos (e.g. 740741 us), which made songs play ~5x too fast. */
    spt = ((uint64_t) tempo * (SYNTH_RATE / 1000) << 32) / (1000ull * (uint64_t) division);
}

static void read_delta (struct track *t) {
    if (t->p >= t->end) {
        t->done = 1;
        return;
    }
    t->next_tick += vlq (t);
}

/* send: 0 = nothing (length scan), 1 = everything, 2 = all but notes (seek) */
static void channel_event (int status, int a, int b, int send) {
    int ch = status & 15;

    switch (status & 0xf0) {
    case 0x80:
        if (send == 1) {
            syn->note_off (ch, a);
        }
        break;
    case 0x90:
        if (b == 0) {
            if (send == 1) {
                syn->note_off (ch, a);
            }
        } else {
            smf_chan_used[ch] = 1;
            if (send == 1) {
                syn->note_on (ch, a, b);
                if (b > smf_chan_level[ch]) {
                    smf_chan_level[ch] = b;
                }
            }
        }
        break;
    case 0xb0:
        chan_cc[ch][a & 127] = b;
        if (send) {
            syn->control (ch, a, b);
        }
        break;
    case 0xc0:
        smf_chan_program[ch] = a;
        if (send) {
            syn->program (ch, a);
        }
        break;
    case 0xe0:
        chan_bend[ch] = a | (b << 7);
        if (send) {
            syn->bend (ch, chan_bend[ch]);
        }
        break;
    }
}

/* Run all events at the earliest pending tick. 0 when every track ended. */
static int step (int send) {
    uint32_t t_min = 0xffffffffu;
    int i, any = 0;

    for (i = 0; i < ntracks; i++) {
        if (!tracks[i].done && tracks[i].next_tick < t_min) {
            t_min = tracks[i].next_tick;
            any = 1;
        }
    }
    if (!any) {
        return 0;
    }
    tick_time += (uint64_t) (t_min - cur_tick) * spt;
    cur_tick = t_min;

    for (i = 0; i < ntracks; i++) {
        struct track *t = &tracks[i];

        while (!t->done && t->next_tick == cur_tick) {
            int st;

            if (t->p >= t->end) {
                t->done = 1;
                break;
            }
            st = *t->p;
            if (st & 0x80) {
                t->p++;
            } else {
                st = t->status;                 /* running status */
            }
            if (st == 0xff) {                   /* meta */
                int type = t->p < t->end ? *t->p++ : 0;
                uint32_t len = vlq (t);

                if (t->p + len > t->end) {
                    t->done = 1;
                    break;
                }
                if (type == 0x51 && len == 3) {
                    set_tempo ((t->p[0] << 16) | (t->p[1] << 8) | t->p[2]);
                } else if (type == 0x2f) {
                    t->done = 1;
                } else if ((type == 0x03 || type == 0x01) && !title[0] && len > 0) {
                    uint32_t k, n = 0;

                    for (k = 0; k < len && n < sizeof (title) - 1; k++) {
                        unsigned char c = t->p[k];

                        title[n++] = (c >= 32 && c < 127) ? c : ' ';
                    }
                    while (n > 0 && title[n - 1] == ' ') {
                        n--;
                    }
                    title[n] = 0;
                }
                t->p += len;
            } else if (st == 0xf0 || st == 0xf7) { /* sysex */
                uint32_t len = vlq (t);

                t->p += len;
                if (t->p > t->end) {
                    t->done = 1;
                }
            } else if (st >= 0x80 && st < 0xf0) {
                int kind = st & 0xf0, a, b = 0;

                t->status = st;
                a = t->p < t->end ? *t->p++ & 0x7f : 0;
                if (kind != 0xc0 && kind != 0xd0) {
                    b = t->p < t->end ? *t->p++ & 0x7f : 0;
                }
                channel_event (st, a, b, send);
            } else {
                t->done = 1;                    /* unknown: give up on the track */
                break;
            }
            if (!t->done) {
                read_delta (t);
            }
        }
    }
    return 1;
}

static void init_tracks (void) {
    long pos = 14;
    int i;

    ntracks = 0;
    while (pos + 8 <= file_len && ntracks < MAX_TRACKS) {
        uint32_t len = be32 (file + pos + 4);

        if (pos + 8 + (long) len > file_len) {
            len = file_len - pos - 8;
        }
        if (!memcmp (file + pos, "MTrk", 4)) {
            struct track *t = &tracks[ntracks++];

            t->p = file + pos + 8;
            t->end = t->p + len;
            t->next_tick = 0;
            t->status = 0;
            t->done = 0;
            read_delta (t);
        }
        pos += 8 + len;
    }
    for (i = 0; i < 16; i++) {
        smf_chan_level[i] = 0;
        smf_chan_program[i] = 0;
        memset (chan_cc[i], 0, sizeof (chan_cc[i]));
        chan_cc[i][7] = 100;
        chan_cc[i][10] = 64;
        chan_cc[i][11] = 127;
        chan_bend[i] = 8192;
    }
    set_tempo (500000);
    cur_tick = 0;
    tick_time = now = 0;
    finished = 0;
}

void smf_rewind (void) {
    init_tracks ();
    if (syn) {
        syn->reset ();
    }
}

int smf_load (const unsigned char *data, long len, struct synth *s) {
    file = data;
    file_len = len;
    title[0] = 0;
    if (len < 14 || memcmp (data, "MThd", 4) || be32 (data + 4) < 6) {
        printf ("smf: not a MIDI file\n");
        return -1;
    }
    division = (data[12] << 8) | data[13];
    if (division & 0x8000) {
        printf ("smf: SMPTE time division not supported\n");
        return -1;
    }
    if (division == 0) {
        division = 96;
    }

    /* Dry run for the length (and the title) */
    syn = 0;
    init_tracks ();
    memset (smf_chan_used, 0, sizeof (smf_chan_used));
    while (step (0)) {
    }
    length_ms = (uint32_t) ((tick_time >> 32) * 1000 / SYNTH_RATE);

    syn = s;
    smf_rewind ();
    return 0;
}

void smf_set_synth (struct synth *s) {
    int ch;

    if (syn) {
        syn->reset ();
    }
    syn = s;
    syn->reset ();
    for (ch = 0; ch < 16; ch++) {               /* carry the channel state over */
        syn->program (ch, smf_chan_program[ch]);
        syn->control (ch, 7, chan_cc[ch][7]);
        syn->control (ch, 10, chan_cc[ch][10]);
        syn->control (ch, 11, chan_cc[ch][11]);
        syn->bend (ch, chan_bend[ch]);
    }
}

void smf_render (int *left, int *right, int n) {
    while (n > 0) {
        uint32_t t_min = 0xffffffffu;
        uint64_t next;
        int i, seg;

        for (i = 0; i < ntracks; i++) {
            if (!tracks[i].done && tracks[i].next_tick < t_min) {
                t_min = tracks[i].next_tick;
            }
        }
        if (t_min == 0xffffffffu) {
            finished = 1;
            seg = n;
        } else {
            next = tick_time + (uint64_t) (t_min - cur_tick) * spt;
            if (next <= now) {
                step (1);
                continue;
            }
            seg = (int) ((next - now + 0xffffffffull) >> 32);
            if (seg > n) {
                seg = n;
            }
        }
        syn->render (left, right, seg);
        left += seg;
        right += seg;
        n -= seg;
        now += (uint64_t) seg << 32;
    }
}

void smf_seek (uint32_t ms) {
    uint64_t target = ((uint64_t) ms * SYNTH_RATE / 1000) << 32;

    init_tracks ();
    if (syn) {
        syn->reset ();
    }
    for (;;) {
        uint32_t t_min = 0xffffffffu;
        int i;

        for (i = 0; i < ntracks; i++) {
            if (!tracks[i].done && tracks[i].next_tick < t_min) {
                t_min = tracks[i].next_tick;
            }
        }
        if (t_min == 0xffffffffu) {
            finished = 1;
            break;
        }
        if (tick_time + (uint64_t) (t_min - cur_tick) * spt > target) {
            break;
        }
        step (2);                               /* programs, controllers, tempo */
    }
    now = target;
}

int smf_done (void) {
    return finished;
}

uint32_t smf_length_ms (void) {
    return length_ms;
}

uint32_t smf_position_ms (void) {
    return (uint32_t) ((now >> 32) * 1000 / SYNTH_RATE);
}

uint32_t smf_tempo_bpm (void) {
    return tempo ? 60000000u / tempo : 0;
}

const char *smf_title (void) {
    return title;
}
