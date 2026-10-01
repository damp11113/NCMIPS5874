#!/bin/sh
# Build and (with "run") start the FreeRTOS port test in QEMU. In WSL:
#   sh rtos/qemu/build.sh          build rtos/qemu/rtos_qemu.elf
#   sh rtos/qemu/build.sh run      build, run (ends by itself, PASS / FAIL)
set -e
cd "$(dirname "$0")"
K=../FreeRTOS-Kernel
SDKINC=../../sdk/libc/include
CROSS=mipsel-linux-gnu-
GCCINC=$(${CROSS}gcc -print-file-name=include)
CFLAGS="-march=mips32r2 -mdsp -EL -msoft-float -O2 -g -ffreestanding -mno-abicalls -fno-pic -G 0 \
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-asynchronous-unwind-tables \
    -nostdinc -isystem $GCCINC -I$SDKINC -I. -I../port -I$K/include -Wall -DPORT_DEBUG_FRAMES"
OBJ=build
mkdir -p $OBJ
rm -f $OBJ/*.o
for f in $K/tasks.c $K/queue.c $K/list.c $K/timers.c $K/event_groups.c $K/stream_buffer.c \
         $K/portable/MemMang/heap_4.c ../port/port.c main.c qlibc.c; do
    ${CROSS}gcc $CFLAGS -c $f -o $OBJ/$(basename ${f%.c}).o
done
for f in start.S ../test/regcheck.S ../port/port_asm.S; do
    ${CROSS}gcc $CFLAGS -c $f -o $OBJ/$(basename ${f%.S})_s.o
done
${CROSS}gcc -EL -msoft-float -nostdlib -static -no-pie -Wl,--gc-sections -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments -T link.ld -o rtos_qemu.elf $OBJ/*.o 2>&1 \
    | grep -vE "uses -mhard-float|linking abicalls files with non-abicalls" || true
${CROSS}size rtos_qemu.elf
if [ "$1" = run ]; then
    timeout 60 qemu-system-mipsel -M malta -cpu 24KEc -m 128 -nographic -no-reboot \
        -kernel rtos_qemu.elf 2>&1 | grep -v "could not load MIPS bios"
fi
