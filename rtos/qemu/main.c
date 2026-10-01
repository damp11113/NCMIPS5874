/*
 * FreeRTOS port test under QEMU: qemu-system-mipsel -M malta -cpu 24KEc
 *
 *   busy A / busy B    same priority, never block: both must advance
 *                      (preemption + time slicing)
 *   regs               every register filled with a pattern, spin, check
 *   dsp                the same for ac1-ac3 and DSPControl
 *   producer/consumer  queue, 1 item every 20 ms
 *   report             every second: counters, errors, free heap
 *   end                after RUN_SECONDS - 1 s suspends the busy tasks for a
 *                      second: the idle task must sleep (wait) almost all
 *                      of it; then stops the scheduler and main runs again
 *                      (the box needs this to return to the launcher)
 *
 * Output on the first serial port (-nographic: the terminal). "PASS" or
 * "FAIL" at the end.
 */
#include <stdarg.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#define RUN_SECONDS     6

/* ---- serial: the Malta board's 16550 at ISA 0x3f8 ---- */

#define UART_THR    (*(volatile uint8_t *) 0xb80003f8u)
#define UART_LSR    (*(volatile uint8_t *) 0xb80003fdu)

static void uart_putc(char c) {
    if (c == '\n') {
        uart_putc('\r');
    }
    while (!(UART_LSR & 0x20)) {
    }
    UART_THR = (uint8_t) c;
}

static void put_num(uint32_t v, int base, int width, char pad) {
    char buf[12];
    int n = 0;

    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width--) {
        uart_putc(pad);
    }
    while (n) {
        uart_putc(buf[--n]);
    }
}

/* %d %u %x %s %c, optional width with 0 */
static void vout(const char *f, va_list ap) {
    for (; *f; f++) {
        int width = 0;
        char pad = ' ';

        if (*f != '%') {
            uart_putc(*f);
            continue;
        }
        f++;
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f++ - '0');
        }
        if (*f == 'd') {
            int v = va_arg(ap, int);

            if (v < 0) {
                uart_putc('-');
                v = -v;
            }
            put_num((uint32_t) v, 10, width, pad);
        } else if (*f == 'u') {
            put_num(va_arg(ap, uint32_t), 10, width, pad);
        } else if (*f == 'x') {
            put_num(va_arg(ap, uint32_t), 16, width, pad);
        } else if (*f == 's') {
            const char *s = va_arg(ap, const char *);

            while (*s) {
                uart_putc(*s++);
            }
        } else if (*f == 'c') {
            uart_putc((char) va_arg(ap, int));
        } else {
            uart_putc(*f);
        }
    }
}

static SemaphoreHandle_t print_lock;

/* Before the scheduler and in hooks: no lock */
static void out(const char *f, ...) {
    va_list ap;

    va_start(ap, f);
    vout(f, ap);
    va_end(ap);
}

/* From tasks: one line at a time */
static void say(const char *f, ...) {
    va_list ap;

    xSemaphoreTake(print_lock, portMAX_DELAY);
    va_start(ap, f);
    vout(f, ap);
    va_end(ap);
    xSemaphoreGive(print_lock);
}

/* ---- tests ---- */

extern int reg_check(uint32_t seed, uint32_t loops);
extern int dsp_check(uint32_t seed, uint32_t loops);

static volatile uint32_t busy_a, busy_b, reg_runs, reg_errors, dsp_runs, dsp_errors;
static volatile uint32_t produced, consumed, queue_errors, idle_pct;
static QueueHandle_t queue;
static TaskHandle_t busy_tasks[4];

static void task_busy(void *arg) {
    volatile uint32_t *counter = arg;

    for (;;) {
        (*counter)++;
    }
}

static void task_regs(void *arg) {
    uint32_t seed = 0x12345678;

    (void) arg;
    for (;;) {
        reg_errors += reg_check(seed, 200000);
        reg_runs++;
        seed = seed * 1664525u + 1013904223u;
    }
}

static void task_dsp(void *arg) {
    uint32_t seed = 0x9e3779b9;

    (void) arg;
    for (;;) {
        dsp_errors += dsp_check(seed, 200000);
        dsp_runs++;
        seed = seed * 1664525u + 1013904223u;
    }
}

static void task_producer(void *arg) {
    TickType_t last = xTaskGetTickCount();
    uint32_t n = 0;

    (void) arg;
    for (;;) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        if (xQueueSend(queue, &n, 0) == pdPASS) {
            n++;
            produced = n;
        }
    }
}

static void task_consumer(void *arg) {
    uint32_t v, expect = 0;

    (void) arg;
    for (;;) {
        if (xQueueReceive(queue, &v, portMAX_DELAY) == pdPASS) {
            if (v != expect) {
                queue_errors++;
            }
            expect = v + 1;
            consumed++;
        }
    }
}

static void task_report(void *arg) {
    TickType_t last = xTaskGetTickCount();

    (void) arg;
    for (;;) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        say("t=%5u ms  busyA %9u  busyB %9u  regs %4u/%u err  dsp %4u/%u err  queue %u/%u  "
            "heap %u  resync %u\n", (uint32_t) xTaskGetTickCount(), busy_a, busy_b, reg_runs, reg_errors, dsp_runs,
            dsp_errors, produced, consumed, (uint32_t) xPortGetFreeHeapSize(),
            ulPortTickResyncs);
    }
}

static inline uint32_t count(void) {
    uint32_t c;

    __asm volatile ("mfc0 %0, $9" : "=r" (c));
    return c;
}

static void task_end(void *arg) {
    uint32_t c0, i0;
    int i;

    (void) arg;
    vTaskDelay(pdMS_TO_TICKS((RUN_SECONDS - 1) * 1000));
    for (i = 0; i < 4; i++) {
        vTaskSuspend(busy_tasks[i]);
    }
    c0 = count();
    i0 = ulPortIdleCount;
    vTaskDelay(pdMS_TO_TICKS(1000));
    idle_pct = (ulPortIdleCount - i0) / ((count() - c0) / 100);
    say("end: busy tasks suspended for 1 s: idle (wait) %u %%\n", idle_pct);
    say("end: stopping the scheduler\n");
    vTaskEndScheduler();                /* returns into main */
    for (;;) {
    }
}

/* ---- hooks ---- */

void vAssertCalled(const char *file, int line) {
    portDISABLE_INTERRUPTS();
    out("\nASSERT %s:%d\n", file, line);
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name) {
    (void) t;
    out("\nSTACK OVERFLOW in %s\n", name);
    for (;;) {
    }
}

void vApplicationIdleHook(void) {
    vPortIdleSleep();
}

void vApplicationMallocFailedHook(void) {
    out("\nOUT OF HEAP\n");
    for (;;) {
    }
}

void vPortFrameMismatch(void *tcb, uint32_t *frame) {
    out("\nFRAME MISMATCH: resuming %s (TCB %08x) from a frame of TCB %08x: EPC %08x v0 %08x "
        "a0 %08x\n", pcTaskGetName((TaskHandle_t) tcb), (uint32_t) tcb, frame[156 / 4],
        frame[148 / 4], frame[4 / 4], frame[12 / 4]);
    for (;;) {
    }
}

void vPortFatalException(uint32_t *frame, uint32_t cause, uint32_t badvaddr) {
    out("\nEXCEPTION code %u  EPC %08x  cause %08x  badvaddr %08x  ra %08x\n",
        (cause >> 2) & 0x1f, frame[148 / 4], cause, badvaddr, frame[108 / 4]);
    for (;;) {
    }
}

int main(void) {
    uint32_t a0, b0;
    int ok;

    out("\nFreeRTOS %s on MIPS 24KEc (QEMU malta), tick %u Hz\n", tskKERNEL_VERSION_NUMBER,
        (uint32_t) configTICK_RATE_HZ);
    print_lock = xSemaphoreCreateMutex();
    queue = xQueueCreate(8, sizeof(uint32_t));
    xTaskCreate(task_busy, "busyA", 512, (void *) &busy_a, 1, &busy_tasks[0]);
    xTaskCreate(task_busy, "busyB", 512, (void *) &busy_b, 1, &busy_tasks[1]);
    xTaskCreate(task_regs, "regs", 512, NULL, 1, &busy_tasks[2]);
    xTaskCreate(task_dsp, "dsp", 512, NULL, 1, &busy_tasks[3]);
    xTaskCreate(task_producer, "prod", 512, NULL, 3, NULL);
    xTaskCreate(task_consumer, "cons", 512, NULL, 2, NULL);
    xTaskCreate(task_report, "report", 1024, NULL, 4, NULL);
    xTaskCreate(task_end, "end", 512, NULL, 5, NULL);

    vTaskStartScheduler();

    /* only after vTaskEndScheduler */
    a0 = busy_a;
    b0 = busy_b;
    out("back in main: busyA %u busyB %u, regs %u runs %u errors, dsp %u runs %u errors, "
        "queue %u sent %u received %u errors, idle %u %%\n", a0, b0, reg_runs, reg_errors,
        dsp_runs, dsp_errors, produced, consumed, queue_errors, idle_pct);
    ok = a0 > 1000 && b0 > 1000 && reg_runs > 0 && dsp_runs > 0 && !reg_errors && !dsp_errors &&
         !queue_errors && produced > RUN_SECONDS * 40 && consumed + 1 >= produced && idle_pct > 80;
    out("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
