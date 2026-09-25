/*
 * Entry for hooks on the stock firmware's AV core IPC send function
 * (0x801a7e4c: a0 = receiver id, a1 = message words {cmd, p1, p2, p3,
 * timeout}, a2 = wait for answer).
 *
 * hookpatch 'ipc' mode replaces its first two instructions
 *     addiu sp, sp, -48
 *     sw    s4, 36(sp)
 * with   j _start ; nop
 * The trampoline calls hook_main (id, msg, ack), then runs those two
 * instructions and jumps back to 0x801a7e54.
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

    addiu   $sp, $sp, -48           /* original 0x801a7e4c */
    j       0x801a7e54
    sw      $s4, 36($sp)            /* original 0x801a7e50, delay slot */
