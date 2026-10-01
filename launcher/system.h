/*
 * NCAPPS system: the launcher under FreeRTOS (see system.c).
 */
#ifndef LAUNCHER_SYSTEM_H
#define LAUNCHER_SYSTEM_H

#include <stdint.h>

/* Run ui () as the UI task, with the USB and music tasks beside it.
 * Returns ui ()'s value once it has returned (the scheduler is stopped
 * again), or -1 without starting anything if the RTOS could not start. */
int system_run(int (*ui)(void));
int system_running(void);

/* "@sys=<address>" for an SDK app's arguments */
void system_app_arg(char *buf, int n);

/* Around a U-Boot ABI program (no SDK runtime, may use USB / vectors
 * itself): music paused, no task switches, tick off */
void system_legacy_begin(void);
void system_legacy_end(void);

/* Background music: one line for the launcher's header, 1 = it changed */
int system_music_line(char *buf, int n);
void system_music_stop(void);

/* launcher.c: an exception in the UI task (or an app running in it):
 * show the crash screen. ctx = the task's saved registers (port_ctx.h) */
void launcher_rtos_crash(uint32_t *ctx, uint32_t cause, uint32_t badvaddr);

#endif
