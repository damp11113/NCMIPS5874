/*
 * RTOS test: the FreeRTOS port (rtos/port) on the box. The same checks as
 * the QEMU test (rtos/qemu), on screen and on the serial console:
 *
 *   busy A / busy B    same priority, never block: both must advance
 *   regs / dsp         every register (and ac1-ac3, DSPControl) filled with
 *                      a pattern, spin, check: errors must stay 0
 *   producer/consumer  queue, one item every 20 ms
 *   ui                 screen + serial line twice a second, keys
 *
 * BACK (or EXIT, MENU, POWER) stops the scheduler: main runs again,
 * shows PASS / FAIL and returns to the launcher. Only the ui task calls
 * U-Boot (keys, serial) and draws; U-Boot is not made for multitasking.
 *
 *   sh apps/rtostest/build.sh -> apps/rtostest/RTOSTEST.BIN
 */
#include "sdk.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define BG      RGB(10, 20, 30)

extern int reg_check(uint32_t seed, uint32_t loops);
extern int dsp_check(uint32_t seed, uint32_t loops);

static struct fb fb;
static int have_screen;
static volatile uint32_t busy_a, busy_b, reg_runs, reg_errors, dsp_runs, dsp_errors;
static volatile uint32_t produced, consumed, queue_errors, ticks_at_end, ub_at_end;
static uint32_t ub_t0;
static QueueHandle_t queue;

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
        reg_errors += reg_check(seed, 400000);
        reg_runs++;
        seed = seed * 1664525u + 1013904223u;
    }
}

static void task_dsp(void *arg) {
    uint32_t seed = 0x9e3779b9;

    (void) arg;
    for (;;) {
        dsp_errors += dsp_check(seed, 400000);
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
            produced = ++n;
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

static void line(int y, const char *s, u16 col) {
    if (have_screen) {
        fb_rect(&fb, 0, y - 4, fb.w, 40, BG);
        fb_text(&fb, 60, y, s, 2, col, TRANSPARENT);
    }
}

static void draw_status(void) {
    char s[120];
    uint32_t t = xTaskGetTickCount();

    snprintf(s, sizeof(s), "uptime %u.%03u s   U-Boot get_timer %u ms   free RTOS heap %u KB",
             t / 1000, t % 1000, (unsigned) ub_get_timer(ub_t0),
             (unsigned) (xPortGetFreeHeapSize() / 1024));
    line(160, s, WHITE);
    snprintf(s, sizeof(s), "busy A %10u   busy B %10u   (same priority: both grow)", busy_a,
             busy_b);
    line(210, s, WHITE);
    snprintf(s, sizeof(s), "register checks %6u   errors %u", reg_runs, reg_errors);
    line(260, s, reg_errors ? RED : GREEN);
    snprintf(s, sizeof(s), "DSP checks      %6u   errors %u", dsp_runs, dsp_errors);
    line(310, s, dsp_errors ? RED : GREEN);
    snprintf(s, sizeof(s), "queue %u sent, %u received, errors %u", produced, consumed,
             queue_errors);
    line(360, s, queue_errors ? RED : GREEN);
    printf("rtostest: t=%u ms busyA %u busyB %u regs %u/%u dsp %u/%u queue %u/%u/%u\n", t, busy_a,
           busy_b, reg_runs, reg_errors, dsp_runs, dsp_errors, produced, consumed, queue_errors);
}

static void task_ui(void *arg) {
    TickType_t last_draw = 0;
    struct sdk_key k;

    (void) arg;
    for (;;) {
        while (sdk_key_poll(&k)) {
            if (k.btn == BTN_BACK || k.btn == BTN_MENU || k.btn == BTN_POWER) {
                ticks_at_end = xTaskGetTickCount();
                ub_at_end = ub_get_timer(ub_t0);
                printf("rtostest: stopping the scheduler\n");
                vTaskEndScheduler();    /* main continues */
                for (;;) {
                }
            }
        }
        if (xTaskGetTickCount() - last_draw >= pdMS_TO_TICKS(500)) {
            last_draw = xTaskGetTickCount();
            draw_status();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- hooks ---- */

void vAssertCalled(const char *file, int line_no) {
    portDISABLE_INTERRUPTS();
    printf("rtostest: ASSERT %s:%d\n", file, line_no);
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name) {
    (void) t;
    printf("rtostest: STACK OVERFLOW in %s\n", name);
    for (;;) {
    }
}

void vApplicationTickHook(void) {
    sdk_timer_tick();
}

void vApplicationMallocFailedHook(void) {
    printf("rtostest: OUT OF RTOS HEAP\n");
    for (;;) {
    }
}

void vPortFatalException(uint32_t *frame, uint32_t cause, uint32_t badvaddr) {
    printf("rtostest: EXCEPTION code %u EPC %08x cause %08x badvaddr %08x ra %08x\n",
           (unsigned) ((cause >> 2) & 0x1f), (unsigned) frame[148 / 4], (unsigned) cause,
           (unsigned) badvaddr, (unsigned) frame[108 / 4]);
    for (;;) {
    }
}

int main(int argc, char *argv[]) {
    char s[120];
    struct sdk_key k;
    int ok;

    (void) argc;
    (void) argv;
    have_screen = osd_setup(&fb) == 0;
    if (sdk_sys) {
        /* the launcher's system already runs FreeRTOS: a second scheduler
         * inside its UI task would take its interrupts away */
        printf("rtostest: Multitasking is on, not starting a second FreeRTOS\n");
        if (have_screen) {
            fb_clear(&fb, BG);
            fb_text(&fb, 60, 50, "RTOS test", 3, WHITE, TRANSPARENT);
            fb_text(&fb, 60, 160, "The launcher already runs FreeRTOS (Multitasking ON).", 2,
                    YELLOW, TRANSPARENT);
            fb_text(&fb, 60, 200, "Switch Multitasking off in the settings to run this test.", 2,
                    GREY, TRANSPARENT);
            fb_text(&fb, 60, 260, "Any key: back", 2, GREY, TRANSPARENT);
        }
        while (!sdk_key_poll(&k) || k.repeat) {
            sdk_idle(10000);
        }
        return 0;
    }
    if (have_screen) {
        fb_clear(&fb, BG);
        fb_text(&fb, 60, 50, "FreeRTOS " tskKERNEL_VERSION_NUMBER " on MIPS 24KEc", 3, WHITE,
                TRANSPARENT);
        fb_text(&fb, 60, 100, "tick 1000 Hz, 8 tasks.  BACK stops the scheduler.", 2, GREY,
                TRANSPARENT);
    }
    printf("rtostest: FreeRTOS %s, starting the scheduler\n", tskKERNEL_VERSION_NUMBER);
    sdk_panel_show("rto");

    queue = xQueueCreate(8, sizeof(uint32_t));
    xTaskCreate(task_busy, "busyA", 1024, (void *) &busy_a, 1, NULL);
    xTaskCreate(task_busy, "busyB", 1024, (void *) &busy_b, 1, NULL);
    xTaskCreate(task_regs, "regs", 1024, NULL, 1, NULL);
    xTaskCreate(task_dsp, "dsp", 1024, NULL, 1, NULL);
    xTaskCreate(task_producer, "prod", 1024, NULL, 3, NULL);
    xTaskCreate(task_consumer, "cons", 1024, NULL, 2, NULL);
    xTaskCreate(task_ui, "ui", 4096, NULL, 4, NULL);

    sdk_timer_own();                    /* U-Boot's get_timer off Compare */
    ub_t0 = ub_get_timer(0);
    vTaskStartScheduler();
    sdk_timer_release();

    /* back from vTaskEndScheduler: plain NCAPPS app again */
    /* U-Boot's get_timer must have kept time with the tick (1 tick = 1 ms) */
    ok = busy_a > 1000 && busy_b > 1000 && reg_runs && dsp_runs && !reg_errors && !dsp_errors &&
         !queue_errors && produced > ticks_at_end / 25 && consumed + 1 >= produced &&
         ub_at_end + 20 >= ticks_at_end - ticks_at_end / 50 &&
         ub_at_end <= ticks_at_end + ticks_at_end / 50 + 20;
    snprintf(s, sizeof(s), "%s: ran %u ms (U-Boot %u ms), regs %u/%u err, dsp %u/%u err, queue %u/%u",
             ok ? "PASS" : "FAIL", ticks_at_end, ub_at_end, reg_runs, reg_errors, dsp_runs,
             dsp_errors, produced, consumed);
    printf("rtostest: back in main. %s\n", s);
    line(450, s, ok ? GREEN : RED);
    line(500, "Scheduler stopped. Any key: back to the launcher", GREY);
    sdk_panel_show(ok ? "PAS" : "ERR");
    while (!sdk_key_poll(&k) || k.repeat) {
        sdk_idle(10000);
    }
    return 0;
}
