/*
 * Call stubs for U-Boot 2012.04 exported functions (same trick as
 * U-Boot's examples/standalone/stubs.c).
 *
 * U-Boot is PIC: every function rebuilds $gp from $t9 on entry, so the
 * jump MUST go through $t9. Plain C function pointers use other regs
 * and crash. Stub = tail jump, so args and $ra pass through untouched.
 *
 * $k0 = gd, gd->jt at 0x20. Index order from common/_exports.h.
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

EXPORT(get_version, 0)
EXPORT(getc, 1)
EXPORT(tstc, 2)
EXPORT(putc, 3)
EXPORT(puts, 4)
EXPORT(printf, 5)
EXPORT(malloc, 8)
EXPORT(free, 9)
EXPORT(udelay, 10)
EXPORT(get_timer, 11)
