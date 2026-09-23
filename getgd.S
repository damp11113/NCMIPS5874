/*
 * Return U-Boot's global data pointer ($k0 on MIPS U-Boot 2012.04).
 * U-Boot prints it as "## Application terminated, rc = 0x...".
 */

    .set noreorder
    .section .text.start, "ax"
    .global _start

_start:
    jr      $ra
    move    $v0, $k0
