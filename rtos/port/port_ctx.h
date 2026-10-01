/*
 * Saved task context (byte offsets), shared by port.c and port_asm.S.
 * The frame sits on the task's stack; the task's stack pointer after the
 * frame was pushed is stored in its TCB (first member, pxTopOfStack).
 * Not saved: zero, k0 (U-Boot's global data pointer, never touched),
 * k1 (exception scratch), sp (kept in the TCB).
 */
#ifndef PORT_CTX_H
#define PORT_CTX_H

#define CTX_AT      0
#define CTX_V0      4
#define CTX_V1      8
#define CTX_A0      12
#define CTX_A1      16
#define CTX_A2      20
#define CTX_A3      24
#define CTX_T0      28
#define CTX_T1      32
#define CTX_T2      36
#define CTX_T3      40
#define CTX_T4      44
#define CTX_T5      48
#define CTX_T6      52
#define CTX_T7      56
#define CTX_S0      60
#define CTX_S1      64
#define CTX_S2      68
#define CTX_S3      72
#define CTX_S4      76
#define CTX_S5      80
#define CTX_S6      84
#define CTX_S7      88
#define CTX_T8      92
#define CTX_T9      96
#define CTX_GP      100
#define CTX_FP      104
#define CTX_RA      108
#define CTX_HI      112
#define CTX_LO      116
#define CTX_AC1HI   120         /* DSP accumulators 1-3 and DSPControl */
#define CTX_AC1LO   124
#define CTX_AC2HI   128
#define CTX_AC2LO   132
#define CTX_AC3HI   136
#define CTX_AC3LO   140
#define CTX_DSPC    144
#define CTX_EPC     148
#define CTX_STATUS  152
#define CTX_OWNER   156         /* PORT_DEBUG_FRAMES: TCB the frame belongs to */
#define CTX_BYTES   160         /* 8-byte aligned */

#endif
