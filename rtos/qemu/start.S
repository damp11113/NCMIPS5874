/*
 * Entry for the QEMU test (loaded by qemu-system-mipsel -kernel): stack,
 * .bss, Status (kernel mode, DSP on, interrupts off), main. When main
 * returns, the Malta soft-reset register ends QEMU (-no-reboot).
 */
        .set    noreorder
        .section .text.start, "ax"
        .globl  _start
        .ent    _start
_start:
        la      $sp, _stack_top
        la      $8, __bss_start
        la      $9, __bss_end
1:      beq     $8, $9, 2f
        nop
        sw      $0, 0($8)
        b       1b
        addiu   $8, $8, 4
2:      mfc0    $8, $12
        lui     $9, 0x0100              /* MX: DSP ASE on */
        or      $8, $8, $9
        li      $9, ~0x1f               /* IE, EXL, ERL off, kernel mode */
        and     $8, $8, $9
        mtc0    $8, $12
        ehb
        jal     main
        nop
3:      li      $8, 0xbf000500          /* Malta SOFTRES */
        li      $9, 0x42
        sw      $9, 0($8)
        b       3b
        nop
        .end    _start
