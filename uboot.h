/*
 * U-Boot 2012.04 exported functions, usable like normal C.
 * Implemented as jump stubs in exports.S (link it in, buildc.sh does).
 * puts/printf verified on board; others are standard 2012.04 order.
 *
 * Usable from C++ too. In libgfx (GFX_NC5874) builds, printf/malloc/free come
 * from nc5874_std.h instead, so they are not declared here.
 */
#ifndef UBOOT_H
#define UBOOT_H

typedef unsigned int u32;

#ifdef __cplusplus
extern "C" {
#endif

int getc (void);
int tstc (void);
void putc (char c);
void puts (const char *s);
#if !(defined(__cplusplus) && defined(GFX_NC5874))
int printf (const char *fmt, ...);
void *malloc (unsigned int size);
void free (void *ptr);
#endif
void udelay (unsigned long us);
unsigned long get_timer (unsigned long base);

/* From libc.c */
u32 parse_hex (const char *s);
int strcmp (const char *a, const char *b);

#ifdef __cplusplus
}
#endif

/* Direct hardware register access, for later */
#define REG32(addr)     (*(volatile u32 *) (addr))
#define REG8(addr)      (*(volatile unsigned char *) (addr))

#endif
