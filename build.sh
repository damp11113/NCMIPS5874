#!/bin/sh
# Usage (inside WSL): ./build.sh [UART_BASE] [REG_SHIFT]
#   ./build.sh 0xb8000000 0
set -e
BASE=${1:-0xb8000000}
SHIFT=${2:-0}
CROSS=mipsel-linux-gnu-

${CROSS}gcc -c -march=mips32 -EL -mno-abicalls -fno-pic -nostdlib \
    -DUART_BASE=$BASE -DREG_SHIFT=$SHIFT uart_hello.S -o uart_hello.o
${CROSS}ld -T link.ld -nostdlib uart_hello.o -o uart_hello.elf
${CROSS}objcopy -O binary uart_hello.elf uart_hello.bin

echo "Built uart_hello.bin (UART_BASE=$BASE REG_SHIFT=$SHIFT), $(wc -c < uart_hello.bin) bytes"
