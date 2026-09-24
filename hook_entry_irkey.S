/*
 * Entry for hooks that run on every remote key press, INSIDE the stock
 * firmware (IR interrupt context).
 *
 * hookpatch 'irkey' mode replaces the first two instructions of the
 * firmware's IR key callback (0x8017e598, a0 = user code, a1 = key):
 *     addiu sp, sp, -24
 *     li    v0, 0xfe01
 * with   j _start ; nop
 * The trampoline calls hook_main (user, key), then runs those two
 * instructions and jumps back to 0x8017e5a0.
 */

#define HOOK_MAGIC 0x4b4f4f48       /* "HOOK", checked by hookpatch */

    .set noreorder
    .section .text.start, "ax"
    .global _start
_start:
    j       trampoline
    nop
    nop
    .word   HOOK_MAGIC              /* at _start + 12 */

trampoline:
    addiu   $sp, $sp, -32
    sw      $ra, 28($sp)
    sw      $a0, 16($sp)
    sw      $a1, 20($sp)
    sw      $a2, 24($sp)
    jal     hook_main
    sw      $a3, 32($sp)            /* caller's arg slot, free to use */
    lw      $a3, 32($sp)
    lw      $a2, 24($sp)
    lw      $a1, 20($sp)
    lw      $a0, 16($sp)
    lw      $ra, 28($sp)
    addiu   $sp, $sp, 32

    addiu   $sp, $sp, -24           /* original 0x8017e598 */
    j       0x8017e5a0
    li      $v0, 0xfe01             /* original 0x8017e59c, delay slot */
