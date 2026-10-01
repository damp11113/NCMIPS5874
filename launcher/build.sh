#!/bin/sh
# Build the NCAPPS launcher (WSL): sh launcher/build.sh -> launcher/LAUNCHER.BIN
# Includes the system (system.c): FreeRTOS from rtos/, background music
# with the Helix MP3 decoder from helix/mp3.
set -e
cd "$(dirname "$0")/.."
K=rtos/FreeRTOS-Kernel
QUIET_SRC="$K/tasks.c $K/queue.c $K/list.c $K/timers.c $K/event_groups.c $K/stream_buffer.c \
$K/portable/MemMang/heap_4.c $(echo helix/mp3/*.c)" \
CFLAGS_EXTRA="-Ilauncher -Irtos/port -I$K/include -Ihelix/mp3" \
LOAD=0x80800000 MAX_END=0x80a00000 OBJ=build_launcher \
    sh sdk/build.sh launcher/LAUNCHER.BIN launcher/launcher.c launcher/system.c \
    launcher/crash_entry.S rtos/port/port.c rtos/port/port_asm.S
