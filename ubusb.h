/*
 * Use U-Boot's own USB stack from our programs (after 'usb start').
 *
 * Functions (addresses per U-Boot build in ubaddr.h):
 *   usb_get_dev_index (i)
 *       returns &usb_dev[i] (struct 1352 bytes) or NULL if unused
 *   usb_bulk_msg (dev, pipe, data, len, &actual, timeout_ms)
 *       returns 0 ok / -1; actual = bytes moved
 *   usb_control_msg (dev, pipe, request, type, value, index,
 *                    data, size, timeout_ms)
 *       returns bytes moved / -1
 * On an unknown U-Boot build every call fails (NULL / -1).
 * EHCI transfers are synchronous: a bulk IN with no data blocks until
 * EHCI's own timeout (~5 s, prints "EHCI timed out on TD"). Use it
 * request/response style only.
 *
 * struct usb_device offsets: devnum +0, speed +4, maxpacketsize +104
 * (code 0-3, part of every pipe), descriptor +256 (idVendor +264,
 * idProduct +266), status +1300, act_len +1304.
 *
 * U-Boot is PIC: its functions must be entered with $t9 = own address.
 * ub_thunk loads $t9 from ub_target and jumps, so any number of normal
 * o32 arguments (registers + stack) pass straight through.
 */
#ifndef UBUSB_H
#define UBUSB_H

#include "uboot.h"
#include "ubaddr.h"

#define UB_USB_MAX_DEVICE       32

/* Pipe = type << 30 | speed << 26 | ep << 15 | devnum << 8 | dir | mps */
#define UB_PIPE_CONTROL         2u
#define UB_PIPE_BULK            3u
#define UB_DIR_IN               0x80u

#define UB_DEV_DEVNUM(d)        (*(volatile int *) ((char *) (d) + 0))
#define UB_DEV_SPEED(d)         (*(volatile int *) ((char *) (d) + 4))
#define UB_DEV_MPS(d)           (*(volatile int *) ((char *) (d) + 104))
#define UB_DEV_VID(d)           (*(volatile unsigned short *) ((char *) (d) + 264))
#define UB_DEV_PID(d)           (*(volatile unsigned short *) ((char *) (d) + 266))
#define UB_DEV_STATUS(d)        (*(volatile u32 *) ((char *) (d) + 1300))

u32 ub_target __attribute__((used));
extern char ub_thunk[];     /* asm below; called through the typedefs */

__asm__(
    ".text\n"
    ".globl ub_thunk\n"
    ".set push\n"
    ".set noreorder\n"
    "ub_thunk:\n"
    "    lui   $25, %hi(ub_target)\n"
    "    lw    $25, %lo(ub_target)($25)\n"
    "    jr    $25\n"
    "    nop\n"
    ".set pop\n");

typedef void *(*ub_get_dev_t) (int index);
typedef int(*ub_bulk_t) (void *dev, u32 pipe, void *data, int len, int *actual, int timeout);
typedef int(*ub_control_t) (void *dev, u32 pipe, u32 request, u32 type, u32 value,
                             u32 index, void *data, u32 size, int timeout);

static inline void *ub_usb_dev(int index) {
    const struct ub_build *b = ub_build();

    if (!b) {
        return 0;
    }
    ub_target = b->usb_get_dev_index + ub_reloc_off();
    return ((ub_get_dev_t) (void *) ub_thunk) (index);
}

static inline u32 ub_pipe(void *dev, u32 type, u32 ep) {
    return (type << 30) | ((u32) UB_DEV_SPEED(dev) << 26) | ((ep & 0x0f) << 15) |
           ((u32) UB_DEV_DEVNUM(dev) << 8) | (ep & UB_DIR_IN) | (u32) UB_DEV_MPS(dev);
}

static inline int ub_bulk(void *dev, u32 ep, void *data, int len, int *actual, int timeout) {
    const struct ub_build *b = ub_build();

    if (!b) {
        return -1;
    }
    ub_target = b->usb_bulk_msg + ub_reloc_off();
    return ((ub_bulk_t) (void *) ub_thunk) (dev, ub_pipe(dev, UB_PIPE_BULK, ep), data, len,
                                   actual, timeout);
}

/* Control transfer on endpoint 0; type bit 7 (0x80) = device to host */
static inline int ub_control(void *dev, u32 request, u32 type, u32 value, u32 index,
                              void *data, u32 size, int timeout) {
    const struct ub_build *b = ub_build();

    if (!b) {
        return -1;
    }
    ub_target = b->usb_control_msg + ub_reloc_off();
    return ((ub_control_t) (void *) ub_thunk) (dev, ub_pipe(dev, UB_PIPE_CONTROL, type & UB_DIR_IN),
                                      request, type, value, index, data, size, timeout);
}

/* First enumerated device with this VID/PID, or 0 */
static inline void *ub_find_device(u32 vid, u32 pid) {
    int i;

    for (i = 0; i < UB_USB_MAX_DEVICE; i++) {
        void *dev = ub_usb_dev(i);

        if (dev && UB_DEV_VID(dev) == vid && UB_DEV_PID(dev) == pid) {
            return dev;
        }
    }
    return 0;
}

#endif
