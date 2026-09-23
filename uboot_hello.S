/*
 * Hello world through U-Boot 2012.04 exported functions.
 *
 * $k0 = gd, gd->jt at offset 0x20 (verified with getgd + md.l on board).
 * jt indices (common/_exports.h): 0 get_version, 1 getc, 2 tstc,
 *                                 3 putc, 4 puts, 5 printf
 */

#define GD_JT       0x20
#define XF_PUTS     4
#define XF_PRINTF   5

    .set noreorder
    .section .text.start, "ax"
    .global _start

_start:
    addiu   $sp, $sp, -24           # o32: 16-byte arg area + saved ra
    sw      $ra, 20($sp)

    lw      $t9, GD_JT($k0)         # t9 = gd->jt
    lw      $t9, (XF_PUTS * 4)($t9) # t9 = jt[puts]
    la      $a0, msg
    jalr    $t9
    nop

    lw      $t9, GD_JT($k0)
    lw      $t9, (XF_PRINTF * 4)($t9)
    la      $a0, fmt
    move    $a1, $k0
    jalr    $t9
    nop

    lw      $ra, 20($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    move    $v0, $zero

    .section .rodata
msg:
    .asciz  "Hello from bare-metal MIPS via U-Boot puts!\n"
fmt:
    .asciz  "printf works too, gd = 0x%08x\n"
