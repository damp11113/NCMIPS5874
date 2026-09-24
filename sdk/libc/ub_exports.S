/*
 * U-Boot 2012.04 exported functions with a ub_ prefix, so Doom's own
 * C library can use the standard names (printf, puts, malloc, ...).
 * Same tail-jump stubs as ../../exports.S: U-Boot is PIC and must be
 * entered with $t9 = function address. $k0 = gd, gd->jt at 0x20.
 */

#define GD_JT 0x20

#define EXPORT(name, idx)        \
    .globl name;                 \
    .type name, @function;       \
name:                            \
    lw      $t9, GD_JT($k0);     \
    lw      $t9, ((idx) * 4)($t9); \
    jr      $t9;                 \
    nop

    .set noreorder
    .text

EXPORT(ub_getc, 1)
EXPORT(ub_tstc, 2)
EXPORT(ub_putc, 3)
EXPORT(ub_puts, 4)
EXPORT(ub_udelay, 10)
EXPORT(ub_get_timer, 11)
EXPORT(ub_getenv, 14)
