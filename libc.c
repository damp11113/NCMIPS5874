/*
 * Minimal C library functions. GCC may emit calls to memcpy/memset/
 * memmove/memcmp on its own (struct copies, array init), even with
 * -ffreestanding, so every program links these (buildc.sh).
 */
typedef unsigned int size_t;

void *memcpy (void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove (void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset (void *dst, int c, size_t n) {
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char) c;
    }
    return dst;
}

int memcmp (const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;

    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

size_t strlen (const char *s) {
    const char *p = s;

    while (*p) {
        p++;
    }
    return p - s;
}

/* Hex string ("1f", "0x1f", "BF15C000") -> number, stops at first non-hex */
unsigned int parse_hex (const char *s) {
    unsigned int v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            v = (v << 4) | (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = (v << 4) | (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v = (v << 4) | (c - 'A' + 10);
        } else {
            break;
        }
    }
    return v;
}

int strcmp (const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}
