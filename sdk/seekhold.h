/*
 * seekhold: accelerating seek for players. A tap moves 2 s; while the
 * button is held (remote repeats), every repeat makes the step 25 % larger,
 * up to 60 s per step, so a long hold crosses minutes quickly but a short
 * one stays precise. The target is only shown while seeking; the player
 * jumps once, when the button is released (no repeat for SEEKHOLD_IDLE_MS).
 *
 *   static struct seekhold sk;
 *   on RED / GREEN:  seekhold_key (&sk, -1 or +1, k.repeat, pos_ms, len_ms, now_ms);
 *   every loop:      if (seekhold_done (&sk, now_ms)) jump to sk.target_ms;
 *   display:         sk.active ? sk.target_ms : pos_ms
 */
#ifndef SEEKHOLD_H
#define SEEKHOLD_H

#define SEEKHOLD_FIRST_MS   2000
#define SEEKHOLD_MAX_MS     60000
#define SEEKHOLD_IDLE_MS    400

struct seekhold {
    int active, dir;
    u32 step_ms, target_ms, last_ms;
};

static inline void seekhold_key (struct seekhold *s, int dir, int repeat, u32 pos_ms, u32 len_ms,
                                 u32 now_ms) {
    if (!s->active || dir != s->dir) {          /* new seek: start from the song position */
        s->active = 1;
        s->dir = dir;
        s->step_ms = SEEKHOLD_FIRST_MS;
        s->target_ms = pos_ms;
    } else if (repeat) {                        /* held: grow the step */
        s->step_ms = s->step_ms * 5 / 4;
        if (s->step_ms > SEEKHOLD_MAX_MS) {
            s->step_ms = SEEKHOLD_MAX_MS;
        }
    }
    if (dir < 0) {
        s->target_ms = s->target_ms > s->step_ms ? s->target_ms - s->step_ms : 0;
    } else {
        s->target_ms += s->step_ms;
        if (len_ms && s->target_ms + 1000 > len_ms) {
            s->target_ms = len_ms > 1000 ? len_ms - 1000 : 0;
        }
    }
    s->last_ms = now_ms;
}

/* 1 once the button was released: seek to s->target_ms now */
static inline int seekhold_done (struct seekhold *s, u32 now_ms) {
    if (s->active && now_ms - s->last_ms > SEEKHOLD_IDLE_MS) {
        s->active = 0;
        return 1;
    }
    return 0;
}

#endif
