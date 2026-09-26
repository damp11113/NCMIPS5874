#!/bin/sh
# Build code that runs INSIDE the original firmware via hookpatch.
# Usage (in WSL): ./buildhook.sh iptv/hooks/hook_draw.c iptv/hooks/hook_draw [entry.S]
# Output is linked at 0x80004000 and starts with the entry file
# (default hook_entry.S; hook_entry_irkey.S for the irkey hook).
# Hooks are for the IPTV box's stock firmware only (sources in iptv/hooks/).
set -e
SRC=${1:-iptv/hooks/hook_dump.c}
OUT=${2:-iptv/hooks/hook_dump}
ENTRY=${3:-iptv/hooks/hook_entry.S}
CROSS=mipsel-linux-gnu-

${CROSS}gcc -march=mips32r2 -EL -Os -ffreestanding -fno-builtin -nostdlib -I. \
    -mno-abicalls -fno-pic -G 0 -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections -static -no-pie \
    -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,--defsym=LOAD_ADDR=0x80004000 \
    -T link.ld -o $OUT.elf $ENTRY $SRC libc.c -lgcc
${CROSS}objcopy -O binary $OUT.elf $OUT.bin

SIZE=$(wc -c < $OUT.bin)
if [ "$SIZE" -gt 12288 ]; then
    echo "ERROR: $OUT.bin is $SIZE bytes, max 12288 (0x80004000..0x80007000)" >&2
    exit 1
fi
echo "Built $OUT.bin, $SIZE bytes, runs inside firmware at 0x80004000"
