/*
 * Entry for code that runs INSIDE the original firmware (not under U-Boot).
 *
 * hookpatch.bin replaces the firmware's call
 *     printf ("app_entry_task start!!!")      at 0x8009a264
 * with   jal hook_entry ; nop ; nop
 * At that point $s2 holds the firmware's printf, so pass it to C.
 * Returning continues the firmware's normal boot.
 */

#define HOOK_MAGIC 0x4b4f4f48       /* "HOOK", checked by hookpatch */

    .set noreorder
    .section .text.start, "ax"
    .global _start
_start:
    j       hook_main
    move    $a0, $s2                /* delay slot: a0 = firmware printf */
    nop
    .word   HOOK_MAGIC              /* at _start + 12 */
