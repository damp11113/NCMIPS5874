/*
 * NCAPPS system services: the launcher runs FreeRTOS (background music,
 * USB hot-plug) and starts apps as part of its UI task. It passes
 * "@sys=<address of a struct sdk_sys>" to every SDK app; the app's
 * runtime then
 *   reads keys through key_read     (the system catches media keys),
 *   idles through idle              (other tasks run meanwhile),
 *   shows CPU load from idle_ticks  (time the RTOS idle task slept),
 *   reads / writes stick sectors through raw_read / raw_write (one owner
 *                                   for USB; the app keeps its own FAT code),
 *   remounts when storage_gen changes (stick pulled out / put back),
 *   claims the audio output on audio_start () (background music pauses)
 *                                   and gives it back when the app ends,
 *   hands a song to the background player with music_play (version 2:
 *                                   the MUSIC app on HOME).
 * Started from U-Boot's go, or by a launcher without the system, there is
 * no "@sys=" and the runtime works alone as before.
 */
#ifndef SDK_SYS_H
#define SDK_SYS_H

#define SDK_SYS_MAGIC       0x4e435359u     /* "NCSY" */
#define SDK_SYS_VERSION     2       /* the table only grows: check version >= n
                                         * before using a field added in n */

struct sdk_key;

struct sdk_sys {
    u32 magic, version;
    int (*key_read)(struct sdk_key *k);         /* 1 = got one (raw: no saver / MUTE) */
    void (*idle)(u32 us);
    u32 (*idle_ticks)(void);                    /* CP0 Count ticks idle so far */
    int (*raw_read)(u32 start, u32 count, void *buf);
    int (*raw_write)(u32 sector, const void *buf);
    u32 (*storage_gen)(void);                   /* changes on every pull / put back */
    int (*storage_present)(void);               /* 1 = mounted and readable */
    void (*audio_claim)(void);
    void (*audio_release)(void);
    /* version 2: play path (an MP3) in the background from byte offset
     * (0 = start), showing ms as the time played, at volume % (0-100);
     * then the rest of its folder. Strings are copied. 0 = taken over. */
    int (*music_play)(const char *path, u32 offset, u32 ms, int volume);
};

extern const struct sdk_sys *sdk_sys;           /* NULL: no system */

#endif
