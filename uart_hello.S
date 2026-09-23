/*
 * Bare-metal UART hello for Nationalchip 5874 (MIPS32 LE).
 *
 * Assumes a 16550-compatible UART. Build with:
 *   -DUART_BASE=0xbXXXXXXX   uncached (kseg1) address of THR/RBR
 *   -DREG_SHIFT=0|2          0 = byte-spaced regs, 2 = word-spaced regs
 *
 * Returns to U-Boot when done, so many bases can be tried without a reset:
 *   rc = 0  all characters sent
 *   rc = 1  THR-empty never seen (probably wrong base / not a 16550)
 */

#ifndef UART_BASE
#define UART_BASE   0xb8000000
#endif
#ifndef REG_SHIFT
#define REG_SHIFT   0
#endif

#define UART_THR    (0 << REG_SHIFT)
#define UART_LSR    (5 << REG_SHIFT)
#define LSR_THRE    0x20
#define POLL_LIMIT  0x00100000

    .set noreorder
    .section .text.start, "ax"
    .global _start

_start:
    li      $t0, UART_BASE
    la      $t1, msg

next_char:
    lbu     $t2, 0($t1)
    beqz    $t2, done
    li      $v0, 0

    li      $t3, POLL_LIMIT
wait_thre:
    lbu     $t4, UART_LSR($t0)
    andi    $t4, $t4, LSR_THRE
    bnez    $t4, send
    addiu   $t3, $t3, -1
    bnez    $t3, wait_thre
    nop
    b       done
    li      $v0, 1

send:
    sb      $t2, UART_THR($t0)
    b       next_char
    addiu   $t1, $t1, 1

done:
    jr      $ra
    nop

    .section .rodata
msg:
    .asciz  "\r\nHello from bare-metal MIPS!\r\n"
