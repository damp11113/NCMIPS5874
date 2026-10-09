#!/bin/sh
# Build the NCAPPS launcher (WSL): sh launcher/build.sh -> launcher/LAUNCHER.BIN
# Includes the system (system.c): FreeRTOS from rtos/, background music
# with the Helix MP3 decoder from helix/mp3, the network (net/netsvc.c:
# WiFi + lwIP from net/lwip).
set -e
cd "$(dirname "$0")/.."
K=rtos/FreeRTOS-Kernel
L=net/lwip/src
QUIET_SRC="$K/tasks.c $K/queue.c $K/list.c $K/timers.c $K/event_groups.c $K/stream_buffer.c \
$K/portable/MemMang/heap_4.c $(echo helix/mp3/*.c) \
$(echo $L/core/*.c $L/core/ipv4/*.c $L/api/*.c) $L/netif/ethernet.c net/lwip/port/sys_arch.c" \
CFLAGS_EXTRA="-Ilauncher -Irtos/port -I$K/include -Ihelix/mp3 \
-Inet -Inet/port -I$L/include -Inet/lwip/port/include" \
LOAD=0x80800000 MAX_END=0x80c00000 OBJ=build_launcher \
    sh sdk/build.sh launcher/LAUNCHER.BIN launcher/launcher.c launcher/system.c \
    net/netsvc.c launcher/crash_entry.S rtos/port/port.c rtos/port/port_asm.S
