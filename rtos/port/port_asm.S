/*
 * FreeRTOS port for the MIPS 24KEc: exception entry, context save and
 * restore, first task start, and a small setjmp / longjmp for ending the
 * scheduler. See portmacro.h. Only k1 ($27) is used as scratch; k0 ($26)
 * is never touched.
 */
#include "port_ctx.h"

        .set    noreorder
        .set    noat
        .set    dsp
        .text

/* Copied to EBase + 0x180 by port.c: every exception and interrupt */
        .globl  vPortVector
        .ent    vPortVector
vPortVector:
        lui     $27, %hi(vPortExceptionEntry)
        addiu   $27, $27, %lo(vPortExceptionEntry)
        jr      $27
        nop
        .end    vPortVector
        .globl  vPortVectorEnd
vPortVectorEnd:

/* Push the interrupted task's registers on its own stack, store the stack
 * pointer in its TCB, run the C handler on the exception stack, then
 * restore whichever task is current now. */
        .globl  vPortExceptionEntry
        .ent    vPortExceptionEntry
vPortExceptionEntry:
        addiu   $sp, $sp, -CTX_BYTES
        sw      $1, CTX_AT($sp)
        sw      $2, CTX_V0($sp)
        sw      $3, CTX_V1($sp)
        sw      $4, CTX_A0($sp)
        sw      $5, CTX_A1($sp)
        sw      $6, CTX_A2($sp)
        sw      $7, CTX_A3($sp)
        sw      $8, CTX_T0($sp)
        sw      $9, CTX_T1($sp)
        sw      $10, CTX_T2($sp)
        sw      $11, CTX_T3($sp)
        sw      $12, CTX_T4($sp)
        sw      $13, CTX_T5($sp)
        sw      $14, CTX_T6($sp)
        sw      $15, CTX_T7($sp)
        sw      $16, CTX_S0($sp)
        sw      $17, CTX_S1($sp)
        sw      $18, CTX_S2($sp)
        sw      $19, CTX_S3($sp)
        sw      $20, CTX_S4($sp)
        sw      $21, CTX_S5($sp)
        sw      $22, CTX_S6($sp)
        sw      $23, CTX_S7($sp)
        sw      $24, CTX_T8($sp)
        sw      $25, CTX_T9($sp)
        sw      $28, CTX_GP($sp)
        sw      $30, CTX_FP($sp)
        sw      $31, CTX_RA($sp)
        mfhi    $27
        sw      $27, CTX_HI($sp)
        mflo    $27
        sw      $27, CTX_LO($sp)
        mfhi    $27, $ac1
        sw      $27, CTX_AC1HI($sp)
        mflo    $27, $ac1
        sw      $27, CTX_AC1LO($sp)
        mfhi    $27, $ac2
        sw      $27, CTX_AC2HI($sp)
        mflo    $27, $ac2
        sw      $27, CTX_AC2LO($sp)
        mfhi    $27, $ac3
        sw      $27, CTX_AC3HI($sp)
        mflo    $27, $ac3
        sw      $27, CTX_AC3LO($sp)
        rddsp   $27, 0x3f
        sw      $27, CTX_DSPC($sp)
        mfc0    $27, $14                /* EPC */
        sw      $27, CTX_EPC($sp)
        mfc0    $27, $12                /* Status (EXL set) */
        sw      $27, CTX_STATUS($sp)

        lui     $27, %hi(pxCurrentTCB)
        lw      $27, %lo(pxCurrentTCB)($27)
        sw      $sp, 0($27)             /* pxCurrentTCB->pxTopOfStack */
#ifdef PORT_DEBUG_FRAMES
        sw      $27, CTX_OWNER($sp)
#endif

        move    $4, $sp                 /* a0 = the frame */
        lui     $27, %hi(pulPortIsrStackTop)
        lw      $sp, %lo(pulPortIsrStackTop)($27)
        jal     vPortExceptionHandler
        nop
        .end    vPortExceptionEntry

/* Fall through: restore pxCurrentTCB's frame and return into that task */
        .globl  vPortRestoreContext
        .ent    vPortRestoreContext
vPortRestoreContext:
        lui     $27, %hi(pxCurrentTCB)
        lw      $27, %lo(pxCurrentTCB)($27)
        lw      $sp, 0($27)
#ifdef PORT_DEBUG_FRAMES
        lw      $1, CTX_OWNER($sp)       /* 0: a new task's first frame */
        beq     $1, $27, 1f
        nop
        beqz    $1, 1f
        nop
        move    $4, $27                 /* a0 = TCB, a1 = frame */
        move    $5, $sp
        lui     $sp, %hi(pulPortIsrStackTop)
        lw      $sp, %lo(pulPortIsrStackTop)($sp)
        jal     vPortFrameMismatch      /* does not return */
        nop
1:
#endif

        lw      $27, CTX_STATUS($sp)
        mtc0    $27, $12
        lw      $27, CTX_EPC($sp)
        mtc0    $27, $14
        lw      $27, CTX_DSPC($sp)
        wrdsp   $27, 0x3f
        lw      $27, CTX_AC1HI($sp)
        mthi    $27, $ac1
        lw      $27, CTX_AC1LO($sp)
        mtlo    $27, $ac1
        lw      $27, CTX_AC2HI($sp)
        mthi    $27, $ac2
        lw      $27, CTX_AC2LO($sp)
        mtlo    $27, $ac2
        lw      $27, CTX_AC3HI($sp)
        mthi    $27, $ac3
        lw      $27, CTX_AC3LO($sp)
        mtlo    $27, $ac3
        lw      $27, CTX_HI($sp)
        mthi    $27
        lw      $27, CTX_LO($sp)
        mtlo    $27

        lw      $1, CTX_AT($sp)
        lw      $2, CTX_V0($sp)
        lw      $3, CTX_V1($sp)
        lw      $4, CTX_A0($sp)
        lw      $5, CTX_A1($sp)
        lw      $6, CTX_A2($sp)
        lw      $7, CTX_A3($sp)
        lw      $8, CTX_T0($sp)
        lw      $9, CTX_T1($sp)
        lw      $10, CTX_T2($sp)
        lw      $11, CTX_T3($sp)
        lw      $12, CTX_T4($sp)
        lw      $13, CTX_T5($sp)
        lw      $14, CTX_T6($sp)
        lw      $15, CTX_T7($sp)
        lw      $16, CTX_S0($sp)
        lw      $17, CTX_S1($sp)
        lw      $18, CTX_S2($sp)
        lw      $19, CTX_S3($sp)
        lw      $20, CTX_S4($sp)
        lw      $21, CTX_S5($sp)
        lw      $22, CTX_S6($sp)
        lw      $23, CTX_S7($sp)
        lw      $24, CTX_T8($sp)
        lw      $25, CTX_T9($sp)
        lw      $28, CTX_GP($sp)
        lw      $30, CTX_FP($sp)
        lw      $31, CTX_RA($sp)
        addiu   $sp, $sp, CTX_BYTES
        ehb
        eret
        .end    vPortRestoreContext

/* Start the first task: mask interrupts, set EXL so the restore's eret
 * applies the task's Status (EXL cleared, IE and the tick enabled). */
        .globl  vPortStartFirstTask
        .ent    vPortStartFirstTask
vPortStartFirstTask:
        di
        ehb
        mfc0    $8, $12
        ori     $8, $8, 0x0002
        mtc0    $8, $12
        ehb
        j       vPortRestoreContext
        nop
        .end    vPortStartFirstTask

/* int port_setjmp (uint32_t buf[12]) / void port_longjmp (uint32_t buf[12], int v) */
        .globl  port_setjmp
        .ent    port_setjmp
port_setjmp:
        sw      $16, 0($4)
        sw      $17, 4($4)
        sw      $18, 8($4)
        sw      $19, 12($4)
        sw      $20, 16($4)
        sw      $21, 20($4)
        sw      $22, 24($4)
        sw      $23, 28($4)
        sw      $28, 32($4)
        sw      $29, 36($4)
        sw      $30, 40($4)
        sw      $31, 44($4)
        jr      $31
        move    $2, $0
        .end    port_setjmp

        .globl  port_longjmp
        .ent    port_longjmp
port_longjmp:
        lw      $16, 0($4)
        lw      $17, 4($4)
        lw      $18, 8($4)
        lw      $19, 12($4)
        lw      $20, 16($4)
        lw      $21, 20($4)
        lw      $22, 24($4)
        lw      $23, 28($4)
        lw      $28, 32($4)
        lw      $29, 36($4)
        lw      $30, 40($4)
        lw      $31, 44($4)
        jr      $31
        move    $2, $5
        .end    port_longjmp
