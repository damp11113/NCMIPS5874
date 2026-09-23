#!/bin/sh
# Build code that runs INSIDE the original firmware via hookpatch.
# Usage (in WSL): ./buildhook.sh hook_draw.c hook_draw
# Output is linked at 0x80004000 and starts with hook_entry.S.
set -e
SRC=${1:-hook_dump.c}
OUT=${2:-hook_dump}
CROSS=mipsel-linux-gnu-

${CROSS}gcc -march=mips32r2 -EL -Os -ffreestanding -fno-builtin -nostdlib \
    -mno-abicalls -fno-pic -G 0 -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections -static -no-pie \
    -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,--defsym=LOAD_ADDR=0x80004000 \
    -T link.ld -o $OUT.elf hook_entry.S $SRC libc.c -lgcc
${CROSS}objcopy -O binary $OUT.elf $OUT.bin

SIZE=$(wc -c < $OUT.bin)
if [ "$SIZE" -gt 12288 ]; then
    echo "ERROR: $OUT.bin is $SIZE bytes, max 12288 (0x80004000..0x80007000)" >&2
    exit 1
fi
echo "Built $OUT.bin, $SIZE bytes, runs inside firmware at 0x80004000"
