#!/bin/sh
# Build a C program for U-Boot 'go'.
# Usage (in WSL): ./buildc.sh [main.c] [out-name] [load-addr]
#   load-addr defaults to 0x80008000 (the usual 'go ${a}' address)
set -e
SRC=${1:-main.c}
OUT=${2:-app}
LOAD=${3:-0x80008000}
CROSS=mipsel-linux-gnu-

${CROSS}gcc -march=mips32r2 -EL -Os -ffreestanding -fno-builtin -nostdlib \
    -mno-abicalls -fno-pic -G 0 -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections -static -no-pie \
    -Wl,--no-warn-rwx-segments -Wl,--build-id=none \
    -Wl,--require-defined=_start -Wl,--defsym=LOAD_ADDR=$LOAD \
    -T link.ld -o $OUT.elf start.c $SRC libc.c exports.S -lgcc
${CROSS}objcopy -O binary $OUT.elf $OUT.bin

# Safety check: first function in the binary must be _start
FIRST=$(${CROSS}nm -n $OUT.elf | awk '$2 ~ /[Tt]/ {print $3; exit}')
if [ "$FIRST" != "_start" ]; then
    echo "ERROR: binary starts with '$FIRST', not _start" >&2
    exit 1
fi

echo "Built $OUT.bin, $(wc -c < $OUT.bin) bytes, load at $LOAD"
