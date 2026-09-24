/*
 * Exception vectors for the launcher's crash screen (launcher.c).
 *
 * The launcher copies U-Boot's whole 4 KB vector page (EBase) and points
 * EBase at the copy, so every interrupt vector stays U-Boot's own code:
 * U-Boot runs with Cause.IV = 1, i.e. vectored interrupts at 0x200 + n x
 * spacing (a hand-made page with only +0x200 froze on the first other
 * interrupt). Only two entries are replaced with the stubs below:
 *   +0x000  TLB refill (e.g. NULL pointer)          -> crash
 *   +0x180  general: interrupts (ExcCode 0) jump on to U-Boot's own
 *           vector (uboot_ebase + 0x180) untouched; everything else -> crash
 * The stubs only use absolute j / lui+lw and a relative branch inside the
 * stub, so they work at any address in kseg0.
 *
 * crash_common saves all registers into crash_frame and calls
 * crash_handler (struct frame *) on its own stack; the handler rewrites
 * EPC / SP so the eret below continues in crash_resume () (C).
 * $k0 is U-Boot's global data pointer: never changed here, only $k1.
 */
#define F_HI        128
#define F_LO        132
#define F_EPC       136
#define F_STATUS    140
#define F_CAUSE     144
#define F_BADVADDR  148

    .set noreorder
    .set noat

    .text
    .globl crash_stub_refill, crash_stub_refill_end
crash_stub_refill:                  /* copied to +0x000 */
    j       crash_common
    nop
crash_stub_refill_end:

    .globl crash_stub_general, crash_stub_general_end
crash_stub_general:                 /* copied to +0x180 */
    mfc0    $k1, $13                /* Cause */
    andi    $k1, $k1, 0x7c          /* ExcCode << 2 */
    beqz    $k1, 1f
    nop
    j       crash_common
    nop
1:  lui     $k1, %hi(uboot_ebase)   /* interrupt: U-Boot handles it */
    lw      $k1, %lo(uboot_ebase)($k1)
    addiu   $k1, $k1, 0x180
    jr      $k1
    nop
crash_stub_general_end:

crash_common:
    lui     $k1, %hi(crash_frame)
    addiu   $k1, $k1, %lo(crash_frame)
    sw      $1, 4($k1)
    sw      $2, 8($k1)
    sw      $3, 12($k1)
    sw      $4, 16($k1)
    sw      $5, 20($k1)
    sw      $6, 24($k1)
    sw      $7, 28($k1)
    sw      $8, 32($k1)
    sw      $9, 36($k1)
    sw      $10, 40($k1)
    sw      $11, 44($k1)
    sw      $12, 48($k1)
    sw      $13, 52($k1)
    sw      $14, 56($k1)
    sw      $15, 60($k1)
    sw      $16, 64($k1)
    sw      $17, 68($k1)
    sw      $18, 72($k1)
    sw      $19, 76($k1)
    sw      $20, 80($k1)
    sw      $21, 84($k1)
    sw      $22, 88($k1)
    sw      $23, 92($k1)
    sw      $24, 96($k1)
    sw      $25, 100($k1)
    sw      $26, 104($k1)
    sw      $28, 112($k1)
    sw      $29, 116($k1)
    sw      $30, 120($k1)
    sw      $31, 124($k1)
    mfhi    $8
    sw      $8, F_HI($k1)
    mflo    $8
    sw      $8, F_LO($k1)
    mfc0    $8, $14
    sw      $8, F_EPC($k1)
    mfc0    $8, $12
    sw      $8, F_STATUS($k1)
    mfc0    $8, $13
    sw      $8, F_CAUSE($k1)
    mfc0    $8, $8
    sw      $8, F_BADVADDR($k1)

    lui     $sp, %hi(crash_stack_top)
    addiu   $sp, $sp, %lo(crash_stack_top)
    addiu   $sp, $sp, -16
    move    $4, $k1
    jal     crash_handler
    nop

    lui     $k1, %hi(crash_frame)
    addiu   $k1, $k1, %lo(crash_frame)
    lw      $8, F_HI($k1)
    mthi    $8
    lw      $8, F_LO($k1)
    mtlo    $8
    lw      $8, F_EPC($k1)
    mtc0    $8, $14
    lw      $8, F_STATUS($k1)
    mtc0    $8, $12
    ehb
    lw      $1, 4($k1)
    lw      $2, 8($k1)
    lw      $3, 12($k1)
    lw      $4, 16($k1)
    lw      $5, 20($k1)
    lw      $6, 24($k1)
    lw      $7, 28($k1)
    lw      $8, 32($k1)
    lw      $9, 36($k1)
    lw      $10, 40($k1)
    lw      $11, 44($k1)
    lw      $12, 48($k1)
    lw      $13, 52($k1)
    lw      $14, 56($k1)
    lw      $15, 60($k1)
    lw      $16, 64($k1)
    lw      $17, 68($k1)
    lw      $18, 72($k1)
    lw      $19, 76($k1)
    lw      $20, 80($k1)
    lw      $21, 84($k1)
    lw      $22, 88($k1)
    lw      $23, 92($k1)
    lw      $24, 96($k1)
    lw      $25, 100($k1)
    lw      $26, 104($k1)
    lw      $28, 112($k1)
    lw      $29, 116($k1)
    lw      $30, 120($k1)
    lw      $31, 124($k1)
    eret
    nop

    .bss
    .align 3
    .globl crash_frame
crash_frame:
    .space 152
    .align 3
crash_stack:
    .space 16384
    .globl crash_stack_top
crash_stack_top:
