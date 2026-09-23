/*
 * Platform hooks for libgfx's GFX_NC5874 back-end (see nc5874_std.h),
 * running under U-Boot on the set-top box.
 *
 * Heap: a simple first-fit allocator with coalescing over RAM that is unused
 * once an app runs: 0x81600000..0x82ff0000 (26 MB). U-Boot's own malloc pool
 * is too small for full-screen ARGB8888 surfaces (1280x720 = 3.7 MB).
 * Above U-Boot (ends ~0x81500000) and below the OSD plane (phys 0x03000000).
 */
#include "uboot.h"

#define HEAP_START  0x81600000u
#define HEAP_END    0x82ff0000u

typedef struct block {
    unsigned int size;          /* payload bytes */
    unsigned int used;
    struct block *next;
    unsigned int pad;           /* 16-byte header keeps payload 8-aligned */
} block_t;

static block_t *heap_head;

static void heap_init (void) {
    heap_head = (block_t *) HEAP_START;
    heap_head->size = HEAP_END - HEAP_START - sizeof (block_t);
    heap_head->used = 0;
    heap_head->next = 0;
}

void *gfx_nc5874_malloc (unsigned int n) {
    block_t *b;

    if (!heap_head) {
        heap_init ();
    }
    n = (n + 15u) & ~15u;
    for (b = heap_head; b; b = b->next) {
        if (b->used || b->size < n) {
            continue;
        }
        if (b->size >= n + sizeof (block_t) + 64u) {       /* split */
            block_t *rest = (block_t *) ((char *) (b + 1) + n);
            rest->size = b->size - n - sizeof (block_t);
            rest->used = 0;
            rest->next = b->next;
            b->next = rest;
            b->size = n;
        }
        b->used = 1;
        return b + 1;
    }
    printf ("gfx heap: out of memory (%u bytes)\n", n);
    return 0;
}

void gfx_nc5874_free (void *p) {
    block_t *b;

    if (!p) {
        return;
    }
    ((block_t *) p - 1)->used = 0;
    for (b = heap_head; b; b = b->next) {               /* coalesce neighbours */
        while (!b->used && b->next && !b->next->used) {
            b->size += sizeof (block_t) + b->next->size;
            b->next = b->next->next;
        }
    }
}

void gfx_nc5874_log (const char *s) {
    puts (s);
}

unsigned int gfx_nc5874_millis (void) {
    return (unsigned int) get_timer (0);
}
