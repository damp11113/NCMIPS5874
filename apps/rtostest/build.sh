#!/bin/sh
# Build the FreeRTOS port test app (WSL): sh apps/rtostest/build.sh -> apps/rtostest/RTOSTEST.BIN
set -e
cd "$(dirname "$0")/../.."
K=rtos/FreeRTOS-Kernel
QUIET_SRC="$K/tasks.c $K/queue.c $K/list.c $K/timers.c $K/event_groups.c $K/stream_buffer.c $K/portable/MemMang/heap_4.c" \
CFLAGS_EXTRA="-Iapps/rtostest -Irtos/port -I$K/include" OBJ=build_rtostest \
    sh sdk/build.sh apps/rtostest/RTOSTEST.BIN apps/rtostest/rtostest.c rtos/port/port.c \
    rtos/port/port_asm.S rtos/test/regcheck.S
