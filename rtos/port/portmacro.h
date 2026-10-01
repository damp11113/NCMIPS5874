/*
 * FreeRTOS port for the MIPS 24KEc (MIPS32r2 + DSP ASE rev 1, no FPU) of the
 * NCAPPS boxes, also run under QEMU (-M malta -cpu 24KEc).
 *
 * Interrupts in compatibility mode (no external interrupt controller, no
 * vectored interrupts): every exception and interrupt enters at
 * EBase + 0x180. Two interrupt lines are used:
 *   SW0 (IP0)   context switch requested by a task (portYIELD)
 *   CP0 timer   the tick (Count / Compare, line from IntCtl.IPTI)
 * Device interrupts (IP2..IP6) stay masked: drivers poll from tasks.
 * Interrupts are masked with di / ei; ISRs run with Status.EXL set and do
 * not nest. Register k0 is never touched (U-Boot keeps its global data
 * pointer there); k1 is the only scratch register of the exception code.
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#ifdef __cplusplus
extern "C" {
#endif

#define portCHAR        char
#define portFLOAT       float
#define portDOUBLE      double
#define portLONG        long
#define portSHORT       short
#define portSTACK_TYPE  uint32_t
#define portBASE_TYPE   long

typedef portSTACK_TYPE StackType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;

#if (configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_32_BITS)
typedef uint32_t TickType_t;
#define portMAX_DELAY           ((TickType_t) 0xffffffffUL)
#define portTICK_TYPE_IS_ATOMIC 1
#else
#error this port only supports 32-bit ticks
#endif

#define portBYTE_ALIGNMENT      8
#define portSTACK_GROWTH        (-1)
#define portTICK_PERIOD_MS      ((TickType_t) 1000 / configTICK_RATE_HZ)
#define portNOP()               __asm volatile ("nop")
#define portMEMORY_BARRIER()    __asm volatile ("" ::: "memory")

/* ---- interrupts and critical sections ---- */

#define portDISABLE_INTERRUPTS()    __asm volatile ("di\n\tehb" ::: "memory")
#define portENABLE_INTERRUPTS()     __asm volatile ("ei\n\tehb" ::: "memory")

extern void vTaskEnterCritical(void);
extern void vTaskExitCritical(void);
#define portCRITICAL_NESTING_IN_TCB 1
#define portENTER_CRITICAL()        vTaskEnterCritical()
#define portEXIT_CRITICAL()         vTaskExitCritical()

/* ISRs run with EXL set: everything is masked already */
#define portSET_INTERRUPT_MASK_FROM_ISR()       0
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x)    ((void) (x))

/* ---- context switches ---- */

/* From a task: raise software interrupt 0; the switch happens as soon as
 * interrupts are enabled (at once, or when the critical section ends). */
void vPortYield(void);
#define portYIELD()                 vPortYield()

/* From an ISR: switch on the way out of the exception */
extern volatile uint32_t ulPortSwitchRequired;
#define portYIELD_FROM_ISR(x)       do { if (x) ulPortSwitchRequired = 1; } while (0)
#define portEND_SWITCHING_ISR(x)    portYIELD_FROM_ISR(x)

extern volatile uint32_t ulPortInterruptNesting;
#define portASSERT_IF_IN_ISR()      configASSERT(ulPortInterruptNesting == 0)

/* ---- task selection with the clz instruction ---- */

#ifndef configUSE_PORT_OPTIMISED_TASK_SELECTION
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#endif
#if configUSE_PORT_OPTIMISED_TASK_SELECTION == 1
#if (configMAX_PRIORITIES > 32)
#error configMAX_PRIORITIES must be 32 or less with configUSE_PORT_OPTIMISED_TASK_SELECTION
#endif
#define portRECORD_READY_PRIORITY(p, r)     (r) |= (1UL << (p))
#define portRESET_READY_PRIORITY(p, r)      (r) &= ~(1UL << (p))
#define portGET_HIGHEST_PRIORITY(top, r)    top = (31UL - (UBaseType_t) __builtin_clz((r)))
#endif

#define portTASK_FUNCTION_PROTO(f, p)   void f(void *p)
#define portTASK_FUNCTION(f, p)         void f(void *p)

/* ---- port services for the application ---- */

/* An exception that is not an interrupt (bad address, reserved
 * instruction, TLB miss, ...): the application may define this (weak
 * default: stop). frame = the saved registers (port_ctx.h offsets);
 * returning resumes the task from the frame, so changing CTX_EPC there
 * sends the task somewhere else (a recovery function). */
void vPortFatalException(uint32_t *frame, uint32_t cause, uint32_t badvaddr);

/* For vApplicationIdleHook: sleep (MIPS wait) until the next interrupt;
 * the time asleep adds up in ulPortIdleCount (CP0 Count ticks) */
void vPortIdleSleep(void);
extern volatile uint32_t ulPortIdleCount;

/* Times the tick had to restart from now (Count had passed the next
 * Compare: interrupts masked for longer than a tick) */
extern volatile uint32_t ulPortTickResyncs;

#ifdef __cplusplus
}
#endif

#endif /* PORTMACRO_H */
