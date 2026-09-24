/*
 * Exception vectors and entry/exit for mmutest.c (MIPS32r2, 24KEc).
 *
 * exc_base is 4 KB aligned; mmutest points CP0 EBase at it and clears
 * Status.BEV, so the CPU enters here:
 *   +0x000  TLB refill (mapped address, no TLB entry, Status.EXL = 0)
 *   +0x180  everything else (TLB invalid / modified, address error,
 *           syscall, reserved instruction, ...)
 *   +0x200  interrupts with Cause.IV = 1 (not used; interrupts stay off)
 *
 * All paths save the registers into exc_frame, switch to a private
 * stack and call exc_handler (struct frame *) in C, which may change the
 * saved registers, EPC and Status; then everything is restored and eret.
 *
 * $k0 holds U-Boot's global data pointer (gd) for the whole program, so
 * the entry code must not change it: only $k1 is used as scratch.
 */
#define F_HI        128
#define F_LO        132
#define F_EPC       136
#define F_STATUS    140
#define F_CAUSE     144
#define F_BADVADDR  148

    .set noreorder
    .set noat

    .section .text.exc_vectors, "ax"
    .align 12
    .globl exc_base
exc_base:
    j       exc_common              /* +0x000 TLB refill */
    nop
    .org    0x180
    j       exc_common              /* +0x180 general */
    nop
    .org    0x200
    j       exc_common              /* +0x200 interrupt (IV = 1) */
    nop

    .text
exc_common:
    lui     $k1, %hi(exc_frame)
    addiu   $k1, $k1, %lo(exc_frame)
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
    sw      $26, 104($k1)           /* k0 = gd, saved only for the dump */
    sw      $28, 112($k1)
    sw      $29, 116($k1)
    sw      $30, 120($k1)
    sw      $31, 124($k1)
    mfhi    $8
    sw      $8, F_HI($k1)
    mflo    $8
    sw      $8, F_LO($k1)
    mfc0    $8, $14                 /* EPC */
    sw      $8, F_EPC($k1)
    mfc0    $8, $12                 /* Status */
    sw      $8, F_STATUS($k1)
    mfc0    $8, $13                 /* Cause */
    sw      $8, F_CAUSE($k1)
    mfc0    $8, $8                  /* BadVAddr */
    sw      $8, F_BADVADDR($k1)

    lui     $sp, %hi(exc_stack_top)
    addiu   $sp, $sp, %lo(exc_stack_top)
    addiu   $sp, $sp, -16
    move    $4, $k1
    jal     exc_handler
    nop

    lui     $k1, %hi(exc_frame)
    addiu   $k1, $k1, %lo(exc_frame)
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
    lw      $28, 112($k1)
    lw      $29, 116($k1)
    lw      $30, 120($k1)
    lw      $31, 124($k1)
    eret
    nop

/*
 * enter_user (pc, sp): drop to user mode at pc with stack sp.
 * Status: KSU = user, EXL = 1 (so the eret below does the switch),
 * interrupts stay off. Returns only through an exception.
 */
    .globl enter_user
enter_user:
    mtc0    $4, $14                 /* EPC = pc */
    mfc0    $8, $12
    li      $9, ~0x1f
    and     $8, $8, $9
    ori     $8, $8, 0x12            /* KSU = 10 (user), EXL = 1, IE = 0 */
    mtc0    $8, $12
    ehb
    move    $sp, $5
    eret
    nop

/*
 * User program, copied by mmutest into a page mapped at virtual
 * 0x00400000. Position-independent apart from that base. Syscalls:
 *   v0 = 1 print string a0, 2 print number a0, 3 exit (a0 = code)
 */
    .globl user_prog, user_prog_end
user_prog:
    lui     $a0, 0x0040
    addiu   $a0, $a0, (umsg1 - user_prog)
    li      $v0, 1
    syscall
    nop

    li      $s0, 0                  /* count 1..5 on the user stack */
    addiu   $sp, $sp, -16
1:  addiu   $s0, $s0, 1
    sw      $s0, 0($sp)
    lw      $a0, 0($sp)
    li      $v0, 2
    syscall
    nop
    slti    $t0, $s0, 5
    bnez    $t0, 1b
    nop

    lui     $a0, 0x0040
    addiu   $a0, $a0, (umsg2 - user_prog)
    li      $v0, 1
    syscall
    nop

    lui     $t0, 0x8000             /* kernel memory: must be refused */
    lw      $t1, 0($t0)
    nop

    li      $a0, 99                 /* not reached */
    li      $v0, 3
    syscall
    nop

umsg1:
    .asciiz "  [user] hello from user mode at 0x00400000\n"
umsg2:
    .asciiz "  [user] now reading kernel memory 0x80000000...\n"
    .align 2
user_prog_end:

    .bss
    .align 3
    .globl exc_frame
exc_frame:
    .space 152
    .align 3
exc_stack:
    .space 8192
exc_stack_top:
