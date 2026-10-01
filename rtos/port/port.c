/*
 * FreeRTOS port for the MIPS 24KEc (see portmacro.h): task stacks, the
 * exception vectors, the CP0 timer tick, idle sleep, and starting / ending
 * the scheduler. vPortEndScheduler puts EBase, Status and Cause back as
 * they were, so a program can return to U-Boot (or the NCAPPS launcher).
 *
 * Two vector modes, picked when the scheduler starts:
 *   chained  (under U-Boot: Status.BEV = 0, Cause.IV = 1, vectored
 *            interrupts): the current vector page is copied (U-Boot's, or
 *            the launcher's copy of it) and only four entries are replaced:
 *            TLB refill (+0x000), general exceptions (+0x180), the SW0
 *            vector and the CP0 timer's vector. U-Boot's own hardware
 *            interrupt lines (IM4 on the boxes) keep running U-Boot's
 *            handlers, which save their registers below the current stack
 *            pointer (keep ~1.5 KB of headroom on every task stack).
 *   simple   (QEMU, or no vectored interrupts): Cause.IV = 0, everything
 *            enters at +0x180, all other interrupt lines masked.
 *
 * The application's FreeRTOSConfig.h must define configCPU_COUNT_HZ, the
 * rate of the CP0 Count register (CPU clock / 2 on the 24KEc: 324 MHz on
 * the boxes; 160 MHz on QEMU's malta).
 */
#include "FreeRTOS.h"
#include "task.h"
#include "port_ctx.h"

#ifndef configCPU_COUNT_HZ
#error FreeRTOSConfig.h must define configCPU_COUNT_HZ
#endif

#define ST_IE       0x00000001u
#define ST_EXL      0x00000002u
#define ST_ERL      0x00000004u
#define ST_KSU      0x00000018u
#define ST_IM       0x0000ff00u
#define ST_IM_SW    0x00000300u         /* IM0, IM1 */
#define ST_IM_SW0   0x00000100u
#define ST_BEV      0x00400000u
#define ST_MX       0x01000000u         /* DSP ASE enable */
#define CAUSE_IP0   0x00000100u
#define CAUSE_IV    0x00800000u

#define read_c0(reg, sel) ({ uint32_t _v; \
    __asm volatile ("mfc0 %0, $" #reg ", " #sel : "=r" (_v)); _v; })
#define write_c0(reg, sel, v) \
    __asm volatile ("mtc0 %0, $" #reg ", " #sel "\n\tehb" : : "r" (v) : "memory")

#define C0_COUNT    9, 0
#define C0_COMPARE  11, 0
#define C0_STATUS   12, 0
#define C0_INTCTL   12, 1
#define C0_CAUSE    13, 0
#define C0_EBASE    15, 1
#define C0_BADVADDR 8, 0
#define RC0(r)      read_c0(r)
#define WC0(r, v)   write_c0(r, v)

#define VEC_PAGE    8192                /* covers 8 vectors at up to 0x380 spacing */

volatile uint32_t ulPortSwitchRequired;
volatile uint32_t ulPortInterruptNesting;
volatile uint32_t ulPortIdleCount;      /* CP0 Count ticks spent in wait */
volatile uint32_t ulPortTickResyncs;    /* times the tick restarted from now */

/* Exceptions run on their own stack, so task stacks need no room for them */
static uint32_t ulIsrStack[2048] __attribute__((aligned(8)));
uint32_t *const pulPortIsrStackTop = &ulIsrStack[2048 - 8];

static uint8_t ucVectorPage[VEC_PAGE] __attribute__((aligned(4096)));

static uint32_t ulTickPeriod, ulTimerIM;
static uint32_t ulSavedStatus, ulSavedEbase, ulSavedCause;
static uint32_t ulSchedulerExit[12];

extern void vPortVector(void);
extern void vPortVectorEnd(void);
extern void vPortStartFirstTask(void);
extern int port_setjmp(uint32_t *buf);
extern void port_longjmp(uint32_t *buf, int v) __attribute__((noreturn));

/* Status bit of the line the CP0 timer raises (IntCtl.IPTI: 6 on the
 * boxes, 7 in QEMU) */
static uint32_t prvTimerLine(void) {
    uint32_t ipti = RC0(C0_INTCTL) >> 29;

    return ipti >= 2 ? ipti : 7;
}

static uint32_t prvTimerIM(void) {
    return 1u << (8 + prvTimerLine());
}

/* Under U-Boot with vectored interrupts (see the top of the file) */
static int prvChainedMode(void) {
    return !(RC0(C0_STATUS) & ST_BEV) && (RC0(C0_CAUSE) & CAUSE_IV);
}

/* Status a task runs with: kernel mode, DSP on, interrupts on, the
 * software interrupt and the timer unmasked, plus U-Boot's own hardware
 * lines in chained mode. EXL is set because the value is loaded just
 * before eret. */
static uint32_t prvTaskStatus(void) {
    uint32_t st = RC0(C0_STATUS);
    uint32_t keep = prvChainedMode() ? (st & ST_IM & ~ST_IM_SW & ~prvTimerIM()) : 0;

    st &= ~(ST_IM | ST_KSU | ST_ERL | ST_BEV);
    return st | ST_MX | ST_EXL | ST_IE | ST_IM_SW0 | prvTimerIM() | keep;
}

static void prvTaskExitError(void) {
    configASSERT(0);                    /* a task function returned */
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
}

StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode,
                                   void *pvParameters) {
    /* 32 bytes above the frame: the argument area the ABI lets the task
     * function use */
    uint32_t *sp = (uint32_t *) ((((uint32_t) (pxTopOfStack + 1)) - CTX_BYTES - 32) & ~7u);
    uint32_t gp, i;

    __asm volatile ("move %0, $28" : "=r" (gp));
    for (i = 0; i < CTX_BYTES / 4; i++) {
        sp[i] = 0;
    }
    sp[CTX_A0 / 4] = (uint32_t) pvParameters;
    sp[CTX_RA / 4] = (uint32_t) prvTaskExitError;
    sp[CTX_T9 / 4] = (uint32_t) pxCode;
    sp[CTX_GP / 4] = gp;
    sp[CTX_EPC / 4] = (uint32_t) pxCode;
    sp[CTX_STATUS / 4] = prvTaskStatus();
    return (StackType_t *) sp;
}

#ifdef PORT_DEBUG_FRAMES
/* A task was about to be resumed from a frame another task saved */
__attribute__((weak)) void vPortFrameMismatch(void *tcb, uint32_t *frame) {
    (void) tcb;
    (void) frame;
    for (;;) {
    }
}
#endif

/* Next tick at Count == next. Compare must end up ahead of Count: if Count
 * passed it already (ticks missed while interrupts were masked, or Count
 * passing it between our read and write; under QEMU the host can pause
 * the CPU thread there), restart from now. Otherwise the next tick would
 * only come when Count wraps: 13 s on the boxes, half a minute in QEMU. */
static void prvSetCompare(uint32_t next) {
    for (;;) {
        WC0(C0_COMPARE, next);
        if ((int32_t) (next - RC0(C0_COUNT)) > 0) {
            return;
        }
        ulPortTickResyncs++;
        next = RC0(C0_COUNT) + ulTickPeriod;
    }
}

void vPortYield(void) {
    uint32_t cause = RC0(C0_CAUSE);

    WC0(C0_CAUSE, cause | CAUSE_IP0);   /* taken as soon as IE allows */
}

/* For the application's idle hook: sleep until the next interrupt and
 * count the time (ulPortIdleCount, CP0 Count ticks) */
void vPortIdleSleep(void) {
    uint32_t t0 = RC0(C0_COUNT);

    __asm volatile ("wait" ::: "memory");
    ulPortIdleCount += RC0(C0_COUNT) - t0;
}

/* A non-interrupt exception (bad address, reserved instruction, TLB miss
 * on a NULL pointer, ...). The application may override this; returning
 * resumes the task from the (possibly changed) frame, e.g. with EPC set
 * to a recovery function. The default stops everything. */
__attribute__((weak)) void vPortFatalException(uint32_t *frame, uint32_t cause,
                                               uint32_t badvaddr) {
    (void) frame;
    (void) cause;
    (void) badvaddr;
    for (;;) {
    }
}

/* Called from vPortExceptionEntry with the interrupted task's frame */
void vPortExceptionHandler(uint32_t *frame) {
    uint32_t cause = RC0(C0_CAUSE);
    uint32_t pending;

    ulPortInterruptNesting = 1;
    if ((cause >> 2) & 0x1f) {          /* not an interrupt */
        vPortFatalException(frame, cause, RC0(C0_BADVADDR));
        ulPortInterruptNesting = 0;
        return;
    }
    pending = cause & RC0(C0_STATUS) & ST_IM;
    if (pending & CAUSE_IP0) {
        WC0(C0_CAUSE, RC0(C0_CAUSE) & ~CAUSE_IP0);
        ulPortSwitchRequired = 1;
    }
    if (pending & ulTimerIM) {
        prvSetCompare(RC0(C0_COMPARE) + ulTickPeriod);  /* also acknowledges it */
        if (xTaskIncrementTick() != pdFALSE) {
            ulPortSwitchRequired = 1;
        }
    }
    if (ulPortSwitchRequired) {
        ulPortSwitchRequired = 0;
        vTaskSwitchContext();
    }
    ulPortInterruptNesting = 0;
}

static void prvPutVector(uint32_t off) {
    const uint32_t *src = (const uint32_t *) vPortVector;
    uint32_t *dst = (uint32_t *) (ucVectorPage + off);
    uint32_t n = ((uint32_t) vPortVectorEnd - (uint32_t) vPortVector) / 4, i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* Build our vector page and point EBase at it. EBase may only change
 * while BEV is set. */
static void prvInstallVectors(int chained) {
    uint32_t st = RC0(C0_STATUS), i;

    if (chained) {
        uint32_t spacing = ((RC0(C0_INTCTL) >> 5) & 0x1f) << 5;
        const uint8_t *old = (const uint8_t *) (RC0(C0_EBASE) & 0xfffff000u);

        for (i = 0; i < VEC_PAGE; i++) {
            ucVectorPage[i] = old[i];
        }
        prvPutVector(0x000);                                /* TLB refill */
        prvPutVector(0x180);                                /* general */
        prvPutVector(0x200);                                /* SW0 (vector 0) */
        prvPutVector(0x200 + prvTimerLine() * spacing);     /* CP0 timer */
    } else {
        prvPutVector(0x000);
        prvPutVector(0x180);
    }
    for (i = 0; i < VEC_PAGE; i += 16) {
        __asm volatile ("synci 0(%0)" : : "r" (ucVectorPage + i) : "memory");
    }
    __asm volatile ("sync" ::: "memory");
    WC0(C0_STATUS, (st & ~ST_IE) | ST_BEV);
    WC0(C0_EBASE, (uint32_t) ucVectorPage);
    if (!chained) {
        WC0(C0_CAUSE, RC0(C0_CAUSE) & ~CAUSE_IV);
    }
    WC0(C0_STATUS, st & ~(ST_BEV | ST_IE));
}

BaseType_t xPortStartScheduler(void) {
    int chained;

    portDISABLE_INTERRUPTS();
    chained = prvChainedMode();
    ulSavedStatus = RC0(C0_STATUS);
    ulSavedEbase = RC0(C0_EBASE);
    ulSavedCause = RC0(C0_CAUSE);
    prvInstallVectors(chained);
    ulTimerIM = prvTimerIM();
    ulTickPeriod = configCPU_COUNT_HZ / configTICK_RATE_HZ;
    prvSetCompare(RC0(C0_COUNT) + ulTickPeriod);
    if (port_setjmp(ulSchedulerExit) == 0) {
        vPortStartFirstTask();          /* does not return */
    }
    /* back from vPortEndScheduler: everything as it was before */
    WC0(C0_STATUS, (ulSavedStatus & ~ST_IE) | ST_BEV);
    WC0(C0_EBASE, ulSavedEbase);
    WC0(C0_CAUSE, (RC0(C0_CAUSE) & ~(CAUSE_IV | CAUSE_IP0)) | (ulSavedCause & CAUSE_IV));
    WC0(C0_STATUS, ulSavedStatus);
    return pdFALSE;
}

/* From a task (vTaskEndScheduler): back into xPortStartScheduler, on the
 * stack it was called with */
void vPortEndScheduler(void) {
    portDISABLE_INTERRUPTS();
    port_longjmp(ulSchedulerExit, 1);
}
