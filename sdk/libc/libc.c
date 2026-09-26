/*
 * Minimal C library for NC5874 box apps (SDK), running under U-Boot with
 * no OS. Written for Doom (doomgeneric), so it covers what Doom uses:
 *   - console output through U-Boot (ub_putc, ub_exports.S)
 *   - malloc: first-fit heap with coalescing over free RAM
 *   - stdio: read-only files held in RAM; fopen gets the whole file
 *     from the SDK runtime (sdk_load_file: USB stick via usbfat.h;
 *     relative paths are inside the app's folder).
 *     Writing files fails cleanly (fopen returns NULL).
 *   - printf family (d i u x X o c s p f %, flags, width, precision, l ll z)
 *   - string/ctype, strtol/atoi/atof, a small sscanf, qsort
 *   - math for Doom's start-up tables (sin, tan, atan) in soft-float
 *   - exit () returns to whoever started the app (sdk_exit, runtime.c)
 *   - sdk_libc_reset (): forget the heap and cached files (the launcher
 *     calls it after an app, which used the same heap RAM)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

void ub_putc(char c);
unsigned long ub_get_timer(unsigned long base);
void ub_udelay(unsigned long us);

/* SDK runtime (runtime.c) */
int sdk_load_file(const char *path, unsigned char **data, long *size);
void sdk_exit(int code) __attribute__((noreturn));

int errno;

/* ---- heap ---- */

/*
 * The heap is a list of free / used blocks over one or more RAM regions
 * (the SDK runtime adds them with sdk_heap_region () before main): the
 * main area above U-Boot, the unused rest of the app's own load area, and
 * the AV core's video memory when "big memory" mode gave it back. Blocks
 * of different regions are never adjacent, so they are never merged.
 */
#define HEAP_START  0x81600000u     /* default region if none was added */
#define HEAP_END    0x843f0000u

typedef struct block {
    size_t size;                    /* payload bytes */
    size_t used;
    struct block *next;
    size_t pad;                     /* 16-byte header keeps payload 8-aligned */
} block_t;

#define MAX_REGIONS 4
static struct { size_t start, end; } regions[MAX_REGIONS];
static int nregions;
static block_t *heap_head;
size_t heap_in_use, heap_total;

/* Add [start, end) to the heap (call before the first malloc) */
void sdk_heap_region(size_t start, size_t end) {
    start = (start + 15u) & ~15u;
    end &= ~15u;
    if (nregions < MAX_REGIONS && end > start + 64 * 1024) {
        regions[nregions].start = start;
        regions[nregions].end = end;
        nregions++;
        heap_total += end - start - sizeof(block_t);   /* known before the first malloc */
    }
}

static void heap_init(void) {
    block_t *prev = 0;
    int i;

    if (nregions == 0) {
        sdk_heap_region(HEAP_START, HEAP_END);
    }
    heap_head = 0;
    heap_total = 0;
    for (i = 0; i < nregions; i++) {
        block_t *b = (block_t *) regions[i].start;

        b->size = regions[i].end - regions[i].start - sizeof(block_t);
        b->used = 0;
        b->next = 0;
        heap_total += b->size;
        if (prev) {
            prev->next = b;
        } else {
            heap_head = b;
        }
        prev = b;
    }
}

void *malloc(size_t n) {
    block_t *b;

    if (!heap_head) {
        heap_init();
    }
    if (n == 0) {
        n = 1;
    }
    n = (n + 15u) & ~15u;
    for (b = heap_head; b; b = b->next) {
        if (b->used || b->size < n) {
            continue;
        }
        if (b->size >= n + sizeof(block_t) + 64) {
            block_t *rest = (block_t *) ((char *) (b + 1) + n);

            rest->size = b->size - n - sizeof(block_t);
            rest->used = 0;
            rest->next = b->next;
            b->next = rest;
            b->size = n;
        }
        b->used = 1;
        heap_in_use += b->size;
        return b + 1;
    }
    return 0;
}

void free(void *p) {
    block_t *b;

    if (!p) {
        return;
    }
    b = (block_t *) p - 1;
    b->used = 0;
    heap_in_use -= b->size;
    for (b = heap_head; b; b = b->next) {
        while (!b->used && b->next && !b->next->used &&
               (char *) (b + 1) + b->size == (char *) b->next) {
            b->size += sizeof(block_t) + b->next->size;
            b->next = b->next->next;
        }
    }
}

void *calloc(size_t n, size_t size) {
    void *p = malloc(n * size);

    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void *realloc(void *p, size_t n) {
    block_t *b;
    void *q;

    if (!p) {
        return malloc(n);
    }
    b = (block_t *) p - 1;
    if (b->size >= n) {
        return p;
    }
    q = malloc(n);
    if (q) {
        memcpy(q, p, b->size);
        free(p);
    }
    return q;
}

/* ---- string ---- */

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;

    if ((((uintptr_t) d | (uintptr_t) s) & 3) == 0) {
        while (n >= 4) {
            *(uint32_t *) d = *(const uint32_t *) s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d <= s || d >= s + n) {
        return memcpy(dst, src, n);
    }
    while (n--) {
        d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;

    if (((uintptr_t) d & 3) == 0) {
        uint32_t w = (unsigned char) c * 0x01010101u;

        while (n >= 4) {
            *(uint32_t *) d = w;
            d += 4;
            n -= 4;
        }
    }
    while (n--) {
        *d++ = (unsigned char) c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;

    for (; n; n--, p++, q++) {
        if (*p != *q) {
            return *p - *q;
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;

    for (; n; n--, p++) {
        if (*p == (unsigned char) c) {
            return (void *) p;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;

    while (*p) {
        p++;
    }
    return p - s;
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;

    while (i < n && s[i]) {
        i++;
    }
    return i;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;

    while ((*d++ = *src++)) {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;

    for (i = 0; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);

    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) {
            return (unsigned char) *a - (unsigned char) *b;
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char) *a) - tolower((unsigned char) *b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (tolower(*a) != tolower(*b) || !*a) {
            return tolower((unsigned char) *a) - tolower((unsigned char) *b);
        }
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char) c) {
            return (char *) s;
        }
        if (!*s) {
            return 0;
        }
    }
}

char *strrchr(const char *s, int c) {
    const char *r = 0;

    for (;; s++) {
        if (*s == (char) c) {
            r = s;
        }
        if (!*s) {
            return (char *) r;
        }
    }
}

char *strstr(const char *h, const char *n) {
    size_t len = strlen(n);

    for (; *h; h++) {
        if (!strncmp(h, n, len)) {
            return (char *) h;
        }
    }
    return len ? 0 : (char *) h;
}

char *strdup(const char *s) {
    char *d = malloc(strlen(s) + 1);

    if (d) {
        strcpy(d, s);
    }
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);

    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

char *strerror(int err) {
    switch (err) {
    case ENOENT: return "No such file";
    case ENOMEM: return "Out of memory";
    case EROFS: return "Read-only file system";
    default: return "Error";
    }
}

/* ---- ctype ---- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

/* ---- numbers ---- */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long v = 0;
    int neg = 0;

    while (isspace(*s)) {
        s++;
    }
    if (*s == '+' || *s == '-') {
        neg = *s++ == '-';
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        isxdigit(s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    for (;; s++) {
        int d;

        if (isdigit(*s)) {
            d = *s - '0';
        } else if (isalpha(*s)) {
            d = tolower(*s) - 'a' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
    }
    if (end) {
        *end = (char *) s;
    }
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base) {
    return (long) strtoul(s, end, base);
}

int atoi(const char *s) { return (int) strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

double strtod(const char *s, char **end) {
    double v = 0, scale = 1;
    int neg = 0;

    while (isspace(*s)) {
        s++;
    }
    if (*s == '+' || *s == '-') {
        neg = *s++ == '-';
    }
    while (isdigit(*s)) {
        v = v * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        while (isdigit(*s)) {
            scale /= 10;
            v += (*s++ - '0') * scale;
        }
    }
    if (end) {
        *end = (char *) s;
    }
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, 0); }

static unsigned int rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (rand_state >> 1) & RAND_MAX;
}

void srand(unsigned int seed) {
    rand_state = seed;
}

/* Shell sort: small, no recursion */
void qsort(void *base, size_t n, size_t size, int(*cmp) (const void *, const void *)) {
    unsigned char *a = base, tmp[64];
    size_t gap, i, j, k;

    if (size > sizeof(tmp)) {
        return;
    }
    for (gap = n / 2; gap > 0; gap /= 2) {
        for (i = gap; i < n; i++) {
            memcpy(tmp, a + i * size, size);
            for (j = i; j >= gap && cmp(a + (j - gap) * size, tmp) > 0; j -= gap) {
                for (k = 0; k < size; k++) {
                    a[j * size + k] = a[(j - gap) * size + k];
                }
            }
            memcpy(a + j * size, tmp, size);
        }
    }
}

char *getenv(const char *name) {
    (void) name;
    return 0;
}

int system(const char *cmd) {
    (void) cmd;
    return -1;
}

void exit(int code) {
    sdk_exit(code);
}

void abort(void) {
    printf("abort ()\n");
    sdk_exit(3);
}

void __assert_fail(const char *expr, const char *file, int line) {
    printf("assert failed: %s (%s:%d)\n", expr, file, line);
    sdk_exit(3);
}

/* ---- math (start-up tables only) ---- */

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x) {
    long i = (long) x;

    return (double) (x < 0 && (double) i != x ? i - 1 : i);
}

double ceil(double x) {
    return -floor(-x);
}

double sin(double x) {
    double term, sum;
    int n;

    while (x > M_PI) {
        x -= 2 * M_PI;
    }
    while (x < -M_PI) {
        x += 2 * M_PI;
    }
    term = sum = x;
    for (n = 1; n < 12; n++) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) {
    return sin(x + M_PI / 2);
}

double tan(double x) {
    return sin(x) / cos(x);
}

/* atan: fold to |x| <= tan (15 deg), then the series */
double atan(double x) {
    double sign = 1, off = 0, sum, term, x2;
    int inv = 0, n;

    if (x < 0) {
        x = -x;
        sign = -1;
    }
    if (x > 1) {
        x = 1 / x;
        inv = 1;
    }
    if (x > 0.2679491924311227) {                   /* tan (pi / 12) */
        x = (x - 0.5773502691896257) / (1 + x * 0.5773502691896257);    /* - 30 deg */
        off = M_PI / 6;
    }
    x2 = x * x;
    term = sum = x;
    for (n = 1; n < 12; n++) {
        term *= -x2;
        sum += term / (2 * n + 1);
    }
    sum += off;
    if (inv) {
        sum = M_PI / 2 - sum;
    }
    return sign * sum;
}

double sqrt(double x) {
    double r = x > 1 ? x / 2 : 1;
    int i;

    if (x <= 0) {
        return 0;
    }
    for (i = 0; i < 40; i++) {
        r = (r + x / r) / 2;
    }
    return r;
}

double pow(double x, double y) {
    double r = 1;
    int n = (int) y;

    while (n-- > 0) {
        r *= x;
    }
    return r;
}

/* ---- time ---- */

time_t time(time_t *t) {
    time_t v = ub_get_timer(0) / 1000;

    if (t) {
        *t = v;
    }
    return v;
}

clock_t clock(void) {
    return ub_get_timer(0);
}

int gettimeofday(struct timeval *tv, void *tz) {
    unsigned long ms = ub_get_timer(0);

    (void) tz;
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

int usleep(unsigned int us) {
    ub_udelay(us);
    return 0;
}

unsigned int sleep(unsigned int s) {
    ub_udelay(s * 1000000u);
    return 0;
}

/* ---- files: read-only, whole file in RAM ---- */

struct _FILE {
    unsigned char *data;
    long size, pos;
    int console, eof, err;
};

static FILE con_out = { 0, 0, 0, 1, 0, 0 };
static FILE con_in = { 0, 0, 0, 1, 1, 0 };
FILE *stdout = &con_out, *stderr = &con_out, *stdin = &con_in;

/* Loaded files stay cached (Doom opens the WAD more than once) */
#define MAX_CACHED 8
static struct { char name[128]; unsigned char *data; long size; } file_cache[MAX_CACHED];

void sdk_libc_reset(void) {
    heap_head = 0;                  /* rebuilt from the same regions on the next malloc */
    heap_in_use = 0;
    memset(file_cache, 0, sizeof(file_cache));
    errno = 0;
}

FILE *fopen(const char *path, const char *mode) {
    const char *name = path;
    unsigned char *data = 0;
    long size = 0;
    FILE *f;
    int i;

    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) {
        errno = EROFS;
        return 0;                       /* no writing: config / saves off */
    }
    while (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    for (i = 0; i < MAX_CACHED; i++) {
        if (file_cache[i].data && !strcasecmp(file_cache[i].name, name)) {
            data = file_cache[i].data;
            size = file_cache[i].size;
            break;
        }
    }
    if (!data) {
        if (sdk_load_file(name, &data, &size) < 0) {
            errno = ENOENT;
            return 0;
        }
        for (i = 0; i < MAX_CACHED; i++) {
            if (!file_cache[i].data) {
                strncpy(file_cache[i].name, name, sizeof(file_cache[i].name) - 1);
                file_cache[i].data = data;
                file_cache[i].size = size;
                break;
            }
        }
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        errno = ENOMEM;
        return 0;
    }
    f->data = data;
    f->size = size;
    return f;
}

int fclose(FILE *f) {
    if (f && !f->console) {
        free(f);                       /* data stays cached */
    }
    return 0;
}

size_t fread(void *ptr, size_t size, size_t n, FILE *f) {
    long want = (long) (size * n), left;

    if (!f || f->console || size == 0) {
        return 0;
    }
    left = f->size - f->pos;
    if (want > left) {
        want = left - left % size;
        f->eof = 1;
    }
    memcpy(ptr, f->data + f->pos, want);
    f->pos += want;
    return want / size;
}

static void con_write(const char *s, size_t n) {
    while (n--) {
        if (*s == '\n') {
            ub_putc('\r');
        }
        ub_putc(*s++);
    }
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f) {
    if (f && f->console) {
        con_write(ptr, size * n);
        return n;
    }
    return 0;
}

int fseek(FILE *f, long off, int whence) {
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : f->size;

    if (f->console || base + off < 0 || base + off > f->size) {
        return -1;
    }
    f->pos = base + off;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) { return f->pos; }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fflush(FILE *f) { (void) f; return 0; }
int fileno(FILE *f) { return f->console ? 1 : 3; }

int fgetc(FILE *f) {
    if (f->console || f->pos >= f->size) {
        f->eof = 1;
        return EOF;
    }
    return f->data[f->pos++];
}

int getc(FILE *f) { return fgetc(f); }

char *fgets(char *s, int n, FILE *f) {
    int i = 0, c = 0;

    while (i < n - 1 && (c = fgetc(f)) != EOF) {
        s[i++] = c;
        if (c == '\n') {
            break;
        }
    }
    if (i == 0 && c == EOF) {
        return 0;
    }
    s[i] = 0;
    return s;
}

int fputc(int c, FILE *f) {
    char ch = c;

    fwrite(&ch, 1, 1, f);
    return c;
}

int fputs(const char *s, FILE *f) {
    fwrite(s, 1, strlen(s), f);
    return 0;
}

int putchar(int c) { return fputc(c, stdout); }

int puts(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

int remove(const char *path) { (void) path; errno = EROFS; return -1; }
int rename(const char *a, const char *b) { (void) a; (void) b; errno = EROFS; return -1; }
int unlink(const char *path) { (void) path; errno = EROFS; return -1; }
int mkdir(const char *path, mode_t mode) { (void) path; (void) mode; errno = EROFS; return -1; }
int isatty(int fd) { return fd < 3; }
int close(int fd) { (void) fd; return 0; }
int open(const char *path, int flags, ...) { (void) path; (void) flags; errno = ENOENT; return -1; }
ssize_t read(int fd, void *buf, size_t n) { (void) fd; (void) buf; (void) n; return -1; }
off_t lseek(int fd, off_t off, int whence) { (void) fd; (void) off; (void) whence; return -1; }

char *getcwd(char *buf, size_t n) {
    if (buf && n > 1) {
        strcpy(buf, "/");
    }
    return buf;
}

ssize_t write(int fd, const void *buf, size_t n) {
    if (fd == 1 || fd == 2) {
        con_write(buf, n);
        return n;
    }
    return -1;
}

int access(const char *path, int mode) {
    FILE *f;

    if (mode & W_OK) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    fclose(f);
    return 0;
}

int stat(const char *path, struct stat *st) {
    FILE *f = fopen(path, "rb");

    if (!f) {
        return -1;
    }
    st->st_size = f->size;
    st->st_mode = 0100644;
    fclose(f);
    return 0;
}

/* ---- printf ---- */

struct out {
    char *buf;
    size_t size, len;
};

static void out_c(struct out *o, char c) {
    if (o->len + 1 < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_pad(struct out *o, char c, int n) {
    while (n-- > 0) {
        out_c(o, c);
    }
}

static void out_num(struct out *o, unsigned long long v, int base, int upper, char sign,
                     int width, int prec, int left, int zero, int alt) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0, total;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (prec >= 0) {
        zero = 0;
    }
    while (n < prec && n < (int) sizeof(tmp) - 2) {
        tmp[n++] = '0';
    }
    if (alt && base == 16) {
        tmp[n++] = upper ? 'X' : 'x';
        tmp[n++] = '0';
    }
    total = n + (sign != 0);
    if (!left && !zero) {
        out_pad(o, ' ', width - total);
    }
    if (sign) {
        out_c(o, sign);
    }
    if (!left && zero) {
        out_pad(o, '0', width - total);
    }
    while (n) {
        out_c(o, tmp[--n]);
    }
    if (left) {
        out_pad(o, ' ', width - total);
    }
}

static void out_str(struct out *o, const char *s, int len, int width, int left) {
    int i;

    if (!left) {
        out_pad(o, ' ', width - len);
    }
    for (i = 0; i < len; i++) {
        out_c(o, s[i]);
    }
    if (left) {
        out_pad(o, ' ', width - len);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0;

        if (*fmt != '%') {
            out_c(&o, *fmt);
            continue;
        }
        for (fmt++;; fmt++) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                zero = 1;
            } else if (*fmt == '+') {
                plus = 1;
            } else if (*fmt == ' ') {
                space = 1;
            } else if (*fmt == '#') {
                alt = 1;
            } else {
                break;
            }
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (isdigit(*fmt)) {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (isdigit(*fmt)) {
                    prec = prec * 10 + (*fmt++ - '0');
                }
            }
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            if (*fmt == 'l') {
                lng++;
            }
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            {
                long long sv = lng >= 2 ? va_arg(ap, long long) :
                               lng ? va_arg(ap, long) : va_arg(ap, int);
                char sign = sv < 0 ? '-' : plus ? '+' : space ? ' ' : 0;
                unsigned long long v = sv < 0 ? -(unsigned long long) sv : (unsigned long long) sv;

                out_num(&o, v, 10, 0, sign, width, prec, left, zero, 0);
            }
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            {
                unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) :
                                       lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                int base = *fmt == 'o' ? 8 : *fmt == 'u' ? 10 : 16;

                out_num(&o, v, base, *fmt == 'X', 0, width, prec, left, zero, alt);
            }
            break;
        case 'p':
            out_num(&o, (uintptr_t) va_arg(ap, void *), 16, 0, 0, width, prec, left, zero, 1);
            break;
        case 'c':
            {
                char c = (char) va_arg(ap, int);

                out_str(&o, &c, 1, width, left);
            }
            break;
        case 's':
            {
                const char *s = va_arg(ap, const char *);
                int len;

                if (!s) {
                    s = "(null)";
                }
                len = strlen(s);
                if (prec >= 0 && len > prec) {
                    len = prec;
                }
                out_str(&o, s, len, width, left);
            }
            break;
        case 'f':
        case 'g':
        case 'e':
            {
                double d = va_arg(ap, double);
                unsigned long ip, fp, scale = 1;
                int i;

                if (prec < 0) {
                    prec = 6;
                }
                if (prec > 9) {
                    prec = 9;
                }
                if (d < 0) {
                    out_c(&o, '-');
                    d = -d;
                }
                for (i = 0; i < prec; i++) {
                    scale *= 10;
                }
                ip = (unsigned long) d;
                fp = (unsigned long) ((d - ip) * scale + 0.5);
                if (fp >= scale) {
                    ip++;
                    fp -= scale;
                }
                out_num(&o, ip, 10, 0, 0, 0, -1, 0, 0, 0);
                if (prec) {
                    out_c(&o, '.');
                    out_num(&o, fp, 10, 0, 0, 0, prec, 0, 0, 0);
                }
            }
            break;
        case '%':
            out_c(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            out_c(&o, '%');
            out_c(&o, *fmt);
            break;
        }
    }
    if (size) {
        buf[o.len < size ? o.len : size - 1] = 0;
    }
    return o.len;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, 0x7fffffff, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (f && f->console) {
        con_write(buf, n < (int) sizeof(buf) ? n : (int) sizeof(buf) - 1);
    }
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* sscanf: %d %i %u %x %o %s %c and literal text, enough for Doom */
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int count = 0;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        char *end;
        int width = 0;

        if (isspace(*fmt)) {
            while (isspace(*s)) {
                s++;
            }
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) {
                break;
            }
            s++;
            continue;
        }
        fmt++;
        while (isdigit(*fmt)) {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h') {
            fmt++;
        }
        if (*fmt != 'c') {
            while (isspace(*s)) {
                s++;
            }
        }
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X' ||
            *fmt == 'o') {
            int base = (*fmt == 'x' || *fmt == 'X') ? 16 : *fmt == 'o' ? 8 : *fmt == 'i' ? 0 : 10;
            long v = strtol(s, &end, base);

            if (end == s) {
                break;
            }
            *va_arg(ap, int *) = (int) v;
            s = end;
            count++;
        } else if (*fmt == 's') {
            char *d = va_arg(ap, char *);
            int n = 0;

            if (!*s) {
                break;
            }
            while (*s && !isspace(*s) && (!width || n < width)) {
                d[n++] = *s++;
            }
            d[n] = 0;
            count++;
        } else if (*fmt == 'c') {
            if (!*s) {
                break;
            }
            *va_arg(ap, char *) = *s++;
            count++;
        } else if (*fmt == '%') {
            if (*s++ != '%') {
                break;
            }
        } else {
            break;
        }
    }
    va_end(ap);
    return count;
}
