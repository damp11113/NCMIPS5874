/*
 * strbuf: tiny string building for screen text (U-Boot exports no sprintf).
 *
 *   struct str s;
 *   s_reset (&s); s_add (&s, "CPU "); s_num (&s, 42, 1); s_add (&s, " %");
 *   ... s.buf
 */
#ifndef STRBUF_H
#define STRBUF_H

struct str {
    char buf[96];
    int n;
};

__attribute__ ((unused))
static void s_reset (struct str *s) {
    s->n = 0;
    s->buf[0] = 0;
}

__attribute__ ((unused))
static void s_add (struct str *s, const char *t) {
    while (*t && s->n < (int) sizeof (s->buf) - 1) {
        s->buf[s->n++] = *t++;
    }
    s->buf[s->n] = 0;
}

/* Unsigned decimal, at least 'digits' digits (zero padded) */
__attribute__ ((unused))
static void s_num (struct str *s, u32 v, int digits) {
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = '0' + v % 10;
        v /= 10;
    } while (v || i < digits);
    while (i > 0 && s->n < (int) sizeof (s->buf) - 1) {
        s->buf[s->n++] = tmp[--i];
    }
    s->buf[s->n] = 0;
}

__attribute__ ((unused))
static void s_time (struct str *s, u32 secs) {
    s_num (s, secs / 60, 1);
    s_add (s, ":");
    s_num (s, secs % 60, 2);
}

#endif
