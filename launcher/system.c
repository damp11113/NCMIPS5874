/*
 * NCAPPS system: the launcher under FreeRTOS (rtos/), switched on in the
 * launcher's settings ("Multitasking", system=1 in SETTINGS.TXT).
 *
 * Tasks (higher number = higher priority):
 *   music  4   background MP3 player: /MUSIC, or the folder of a song the
 *              MUSIC app hands over on HOME (PLAY / PAUSE / STOP / NEXT on
 *              the remote, anywhere, unless an app has the audio)
 *   usb    3   hot-plug: notices the stick going and coming back
 *              (sdk_storage_poll), U-Boot's "usb reset" + mount
 *   ui     1   the launcher's menu loop; apps run inside this task
 *   idle   0   sleeps with the MIPS wait instruction (CPU load meter)
 *
 * Apps get "@sys=<address of sys_table>" (sdk/sys.h): keys, idle, stick
 * sectors (one owner for USB, behind usb_mutex), hot-plug generation and
 * the audio claim go through it. Everything the system keeps lives in the
 * launcher's own 2 MB area (static data, FreeRTOS heap_4), never in the
 * shared heap RAM that apps reuse.
 */
#define BOX_WANT_AUDIO
#include "sdk.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "port_ctx.h"
#include "mp3stream.h"
#include "system.h"

#define PRIO_UI         1
#define PRIO_USB        3
#define PRIO_MUSIC      4
#define UI_STACK        (128 * 1024)        /* words: apps run on it too */
#define WORK_STACK      (16 * 1024)

static TaskHandle_t ui_task, usb_task, music_task;
static int (*ui_fn)(void);
static volatile int ui_rc, running;
static SemaphoreHandle_t usb_mutex;

/* ---- hooks for the SDK runtime ---- */

void sdk_usb_lock(void) {
    if (usb_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTakeRecursive(usb_mutex, portMAX_DELAY);
    }
}

void sdk_usb_unlock(void) {
    if (usb_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreGiveRecursive(usb_mutex);
    }
}

static void rtos_idle(u32 us) {
    TickType_t t = pdMS_TO_TICKS((us + 999) / 1000);

    vTaskDelay(t ? t : 1);
}

static u32 rtos_idle_ticks(void) {
    return ulPortIdleCount;
}

void vApplicationIdleHook(void) {
    vPortIdleSleep();
}

void vApplicationTickHook(void) {
    sdk_timer_tick();
}

void vAssertCalled(const char *file, int line) {
    portDISABLE_INTERRUPTS();
    printf("system: ASSERT %s:%d\n", file, line);
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name) {
    (void) t;
    printf("system: STACK OVERFLOW in task %s, restarting\n", name);
    sdk_reboot();
}

void vApplicationMallocFailedHook(void) {
    printf("system: out of RTOS heap\n");
}

/* A crashed task: the UI task (the launcher or an app in it) gets the
 * launcher's crash screen; a system task is stopped */
static void task_dead(void) {
    for (;;) {
        vTaskSuspend(NULL);
    }
}

void vPortFatalException(uint32_t *frame, uint32_t cause, uint32_t badvaddr) {
    if (xTaskGetCurrentTaskHandle() == ui_task) {
        launcher_rtos_crash(frame, cause, badvaddr);
        return;
    }
    printf("system: task %s crashed (exception %u at 0x%08x, address 0x%08x), stopped\n",
           pcTaskGetName(NULL), (unsigned) ((cause >> 2) & 31), (unsigned) frame[CTX_EPC / 4],
           (unsigned) badvaddr);
    frame[CTX_EPC / 4] = (uint32_t) task_dead;
}

/* ---- background music ---- */

#define MUSIC_DIR       "/MUSIC"
#define MUSIC_MAX       256
#define MUSIC_QUEUED    8192                /* frames kept in the audio ring (170 ms) */

enum { M_STOPPED, M_PLAYING, M_PAUSED };
enum { CMD_PLAY = 1, CMD_PAUSE, CMD_STOP, CMD_NEXT, CMD_CLAIM, CMD_WAKE, CMD_HANDOFF };

static QueueHandle_t music_q;
static char songs[MUSIC_MAX][96];
static int nsongs, cur_song = -1;
static volatile int m_state, app_audio, out_on;
static int m_h = -1;
static u32 m_gen;
static char m_title[64];
static unsigned char m_buf[16384] __attribute__((aligned(8)));
static char m_dir[SDK_PATH_MAX] = MUSIC_DIR;

/* A song handed over by an app (sys_music_play), copied: the app's memory
 * goes when it ends */
static struct {
    char path[SDK_PATH_MAX];
    u32 offset, ms;
    int volume;
} handoff;

static int has_mp3(const char *n) {
    int l = strlen(n);

    return l > 4 && !strcasecmp(n + l - 4, ".mp3");
}

static int m_scan(void) {
    struct sdk_dirent e;
    int h = sdk_dir_open(m_dir), i, j;

    nsongs = 0;
    if (h < 0) {
        return 0;
    }
    while (nsongs < MUSIC_MAX && sdk_dir_read(h, &e)) {
        if (!e.is_dir && has_mp3(e.name)) {
            snprintf(songs[nsongs++], sizeof(songs[0]), "%s", e.name);
        }
    }
    sdk_dir_close(h);
    for (i = 1; i < nsongs; i++) {          /* by name */
        char t[96];

        memcpy(t, songs[i], sizeof(t));
        for (j = i; j > 0 && strcasecmp(songs[j - 1], t) > 0; j--) {
            memcpy(songs[j], songs[j - 1], sizeof(t));
        }
        memcpy(songs[j], t, sizeof(t));
    }
    return nsongs;
}

static int m_refill(unsigned char *dst, int max) {
    long n = sdk_read(m_h, dst, max);

    return n > 0 ? (int) n : 0;
}

static void m_close(void) {
    if (m_h >= 0) {
        sdk_close(m_h);
        m_h = -1;
    }
}

/* Song idx of the list from byte offset on (0 = the start), ms already played */
static int m_open(int idx, u32 offset, u32 ms) {
    char path[SDK_PATH_MAX];
    unsigned char hdr[10];
    u32 start = 0;
    char *dot;

    m_close();
    snprintf(path, sizeof(path), "%s/%s", m_dir, songs[idx]);
    m_h = sdk_open(path);
    if (m_h < 0) {
        return -1;
    }
    if (sdk_read(m_h, hdr, 10) == 10 && !memcmp(hdr, "ID3", 3)) {     /* skip the tag */
        start = ((hdr[6] & 0x7f) << 21 | (hdr[7] & 0x7f) << 14 | (hdr[8] & 0x7f) << 7 |
                 (hdr[9] & 0x7f)) + 10 + ((hdr[5] & 0x10) ? 10 : 0);
    }
    sdk_seek(m_h, offset > start ? offset : start);  /* mid-file: the decoder resyncs */
    helix_used = 0;                         /* new decoder each song */
    mp3s_frames_ok = mp3s_errors = 0;
    mp3s_out_frames = ms / 1000 * AUD_RATE + ms % 1000 * AUD_RATE / 1000;
    mp3s_have = mp3s_pos = mp3s_frac = 0;
    if (mp3s_open_stream(m_buf, sizeof(m_buf), m_refill) < 0) {
        m_close();
        return -1;
    }
    cur_song = idx;
    snprintf(m_title, sizeof(m_title), "%s", songs[idx]);
    dot = strrchr(m_title, '.');
    if (dot) {
        *dot = 0;
    }
    printf("system: music %d/%d %s\n", idx + 1, nsongs, songs[idx]);
    return 0;
}

/* Open the next playable song from idx on; stop if none */
static void m_play_from(int idx) {
    int i;

    for (i = 0; i < nsongs; i++) {
        if (m_open((idx + i) % nsongs, 0, 0) == 0) {
            m_state = M_PLAYING;
            return;
        }
    }
    m_close();
    m_state = M_STOPPED;
}

static void m_output_off(void) {
    if (out_on) {
        audio_stop();
        out_on = 0;
    }
}

/* Play the handed-over song where the app was, then the rest of its folder */
static void m_handoff(void) {
    char path[SDK_PATH_MAX], *slash;
    u32 offset, ms;
    int i;

    vTaskSuspendAll();
    memcpy(path, handoff.path, sizeof(path));
    offset = handoff.offset;
    ms = handoff.ms;
    mp3s_volq = handoff.volume * 256 / 100;
    xTaskResumeAll();
    m_close();
    m_state = M_STOPPED;
    slash = strrchr(path, '/');
    if (!slash) {
        return;
    }
    *slash = 0;
    snprintf(m_dir, sizeof(m_dir), "%s", path[0] ? path : "/");
    if (m_scan() == 0) {
        return;
    }
    m_gen = sdk_storage_gen();
    for (i = 0; i < nsongs && strcmp(songs[i], slash + 1); i++) {
    }
    if (i < nsongs && m_open(i, offset, ms) == 0) {
        m_state = M_PLAYING;
    } else {
        m_play_from(i < nsongs ? i : 0);
    }
}

static void m_command(int cmd) {
    switch (cmd) {
    case CMD_PLAY:
        if (m_state == M_PAUSED) {
            m_state = M_PLAYING;
        } else if (m_state == M_STOPPED && m_scan() > 0) {
            m_gen = sdk_storage_gen();
            m_play_from(cur_song >= 0 && cur_song < nsongs ? cur_song : 0);
        }
        break;
    case CMD_PAUSE:
        m_state = m_state == M_PLAYING ? M_PAUSED : m_state == M_PAUSED ? M_PLAYING : m_state;
        break;
    case CMD_STOP:
        m_close();
        m_state = M_STOPPED;
        break;
    case CMD_NEXT:
        if (m_state != M_STOPPED && nsongs) {
            m_play_from(cur_song + 1);
        }
        break;
    case CMD_HANDOFF:
        m_handoff();
        break;
    case CMD_CLAIM:                         /* an app takes the output now */
        m_output_off();
        break;
    }
    if (m_state != M_PLAYING) {
        m_output_off();
    }
}

static void music_entry(void *arg) {
    int cmd;

    (void) arg;
    for (;;) {
        int playing = m_state == M_PLAYING && !app_audio;

        if (xQueueReceive(music_q, &cmd, playing ? pdMS_TO_TICKS(10) : portMAX_DELAY) == pdPASS) {
            m_command(cmd);
            continue;
        }
        if (!playing) {
            continue;
        }
        if (!sdk_storage_present() || sdk_storage_gen() != m_gen) {
            printf("system: music stopped (stick removed)\n");
            m_close();
            m_state = M_STOPPED;
            m_output_off();
            continue;
        }
        if (!out_on) {
            audio_start();
            out_on = 1;
        }
        while (AUD_BUF_SIZE / AUD_FRAME - audio_space() < MUSIC_QUEUED) {
            if (!mp3s_pump()) {
                m_play_from(cur_song + 1);  /* next song */
                break;
            }
        }
    }
}

static void music_send(int cmd) {
    if (music_q) {
        xQueueSend(music_q, &cmd, pdMS_TO_TICKS(100));
    }
}

/* An app's audio_start (): stop our output before it touches the hardware */
static void sys_audio_claim(void) {
    int i;

    app_audio = 1;
    music_send(CMD_CLAIM);
    for (i = 0; i < 200 && out_on; i++) {
        vTaskDelay(1);
    }
}

static int sys_music_play(const char *path, u32 offset, u32 ms, int volume) {
    if (!has_mp3(path) || strlen(path) >= sizeof(handoff.path)) {
        return -1;
    }
    vTaskSuspendAll();
    snprintf(handoff.path, sizeof(handoff.path), "%s", path);
    handoff.offset = offset;
    handoff.ms = ms;
    handoff.volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    xTaskResumeAll();
    music_send(CMD_HANDOFF);
    return 0;
}

static void sys_audio_release(void) {
    app_audio = 0;
    music_send(CMD_WAKE);
}

void system_music_stop(void) {
    if (running) {
        music_send(CMD_STOP);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int system_music_line(char *buf, int n) {
    static char last[96];

    if (!running) {
        buf[0] = 0;
    } else if (m_state == M_STOPPED) {
        snprintf(buf, n, "PLAY: music from %s", m_dir);
    } else {
        u32 s = mp3s_out_frames / AUD_RATE;

        snprintf(buf, n, "%s %d/%d %.36s %u:%02u", m_state == M_PAUSED ? "||" : ">",
                 cur_song + 1, nsongs, m_title, (unsigned) (s / 60), (unsigned) (s % 60));
    }
    if (strcmp(buf, last)) {
        snprintf(last, sizeof(last), "%s", buf);
        return 1;
    }
    return 0;
}

/* ---- keys: the media keys belong to the music unless an app has the audio ---- */

static int sys_key_read(struct sdk_key *k) {
    int cmd;

    if (!sdk_key_read(k)) {
        return 0;
    }
    if (!k->remote || app_audio) {
        return 1;
    }
    cmd = k->btn == BTN_PLAY ? CMD_PLAY : k->btn == BTN_PAUSE ? CMD_PAUSE :
          k->btn == BTN_STOP ? CMD_STOP : k->btn == BTN_NEXT ? CMD_NEXT : 0;
    if (!cmd) {
        return 1;
    }
    if (!k->repeat) {
        music_send(cmd);
    }
    return 0;
}

/* ---- the service table for apps ---- */

static const struct sdk_sys sys_table = {
    SDK_SYS_MAGIC, SDK_SYS_VERSION, sys_key_read, rtos_idle, rtos_idle_ticks, sdk_raw_read,
    sdk_raw_write, sdk_storage_gen, sdk_storage_present, sys_audio_claim, sys_audio_release,
    sys_music_play,
};

void system_app_arg(char *buf, int n) {
    snprintf(buf, n, "@sys=%08x", (unsigned) &sys_table);
}

/* ---- U-Boot ABI programs: alone, as before ---- */

static u32 legacy_status, legacy_ebase;

#define read_c0(reg, sel) ({ u32 _v; \
    __asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r" (_v)); _v; })
#define write_c0(reg, sel, v) \
    __asm__ volatile("mtc0 %0, $" #reg ", " #sel "\n\tehb" : : "r" ((u32) (v)) : "memory")

void system_legacy_begin(void) {
    u32 ipti = read_c0(12, 1) >> 29;

    if (!running) {
        return;
    }
    sys_audio_claim();
    sdk_usb_lock();                         /* no task is in U-Boot's USB code */
    vTaskSuspendAll();
    legacy_status = read_c0(12, 0);
    legacy_ebase = read_c0(15, 1);
    /* our software interrupt and tick off: the program may use its own vectors */
    write_c0(12, 0, legacy_status & ~((1u << 8) | (1u << (8 + (ipti >= 2 ? ipti : 7)))));
}

void system_legacy_end(void) {
    if (!running) {
        return;
    }
    if (read_c0(15, 1) != legacy_ebase) {   /* it changed EBase: put ours back (BEV set) */
        write_c0(12, 0, (read_c0(12, 0) & ~1u) | (1u << 22));
        write_c0(15, 1, legacy_ebase);
    }
    write_c0(12, 0, legacy_status);
    xTaskResumeAll();
    sdk_usb_unlock();
    sys_audio_release();
}

/* ---- tasks ---- */

static void usb_entry(void *arg) {
    (void) arg;
    for (;;) {
        sdk_storage_poll();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void ui_entry(void *arg) {
    (void) arg;
    ui_rc = ui_fn();
    vTaskEndScheduler();
    for (;;) {
    }
}

int system_running(void) {
    return running;
}

int system_run(int (*ui)(void)) {
    ui_fn = ui;
    if (sdk_timer_own() < 0) {
        printf("system: running without the system\n");
        return -1;
    }
    usb_mutex = xSemaphoreCreateRecursiveMutex();
    music_q = xQueueCreate(8, sizeof(int));
    if (!usb_mutex || !music_q ||
        xTaskCreate(ui_entry, "ui", UI_STACK, NULL, PRIO_UI, &ui_task) != pdPASS ||
        xTaskCreate(usb_entry, "usb", WORK_STACK, NULL, PRIO_USB, &usb_task) != pdPASS ||
        xTaskCreate(music_entry, "music", WORK_STACK, NULL, PRIO_MUSIC, &music_task) != pdPASS) {
        printf("system: cannot create the tasks, running without the system\n");
        usb_mutex = 0;
        sdk_timer_release();
        return -1;
    }
    sdk_idle_hook = rtos_idle;
    sdk_idle_source = rtos_idle_ticks;
    sdk_key_source = sys_key_read;
    sdk_hotplug = 1;
    running = 1;
    printf("system: FreeRTOS %s, multitasking on (music, USB hot-plug)\n",
           tskKERNEL_VERSION_NUMBER);
    vTaskStartScheduler();
    /* the UI returned: plain launcher again (it boots the stock firmware) */
    sdk_timer_release();
    running = 0;
    m_output_off();
    sdk_idle_hook = 0;
    sdk_idle_source = 0;
    sdk_key_source = 0;
    sdk_hotplug = 0;
    usb_mutex = 0;
    return ui_rc;
}
