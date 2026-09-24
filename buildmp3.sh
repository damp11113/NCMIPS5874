#!/bin/sh
# Build a C program that uses the Helix fixed-point MP3 decoder.
# Usage (in WSL): ./buildmp3.sh <prog.c> <out-name>
#
# Helix sources (RealNetworks RPSL/RCSL, not in git) go in helix/mp3/:
#   git clone --depth 1 https://github.com/pschatzmann/arduino-libhelix.git
#   mkdir -p helix/mp3/utils
#   cp arduino-libhelix/src/libhelix-mp3/*.[ch] helix/mp3/
#   cp arduino-libhelix/src/utils/helix_memory.h helix/mp3/utils/
# (tested with commit 5c0a043). The program must define helix_malloc/free.
set -e
SRC=$1
OUT=$2
CROSS=mipsel-linux-gnu-
CFLAGS="-march=mips32r2 -EL -ffreestanding -fno-builtin -nostdlib -mno-abicalls -fno-pic -G 0 \
    -ffunction-sections -fdata-sections"

mkdir -p build_mp3
for f in helix/mp3/*.c; do
    o=build_mp3/$(basename $f .c).o
    if [ ! -f $o ] || [ $f -nt $o ]; then
        ${CROSS}gcc $CFLAGS -O2 -w -Ihelix/mp3 -c $f -o $o
    fi
done

${CROSS}gcc $CFLAGS -Os -Wall -Ihelix/mp3 -Wl,--gc-sections -static -no-pie \
    -Wl,--no-warn-rwx-segments -Wl,--build-id=none \
    -Wl,--require-defined=_start -Wl,--defsym=LOAD_ADDR=0x80008000 \
    -T link.ld -o $OUT.elf start.c $SRC libc.c exports.S build_mp3/*.o -lgcc
${CROSS}objcopy -O binary $OUT.elf $OUT.bin

FIRST=$(${CROSS}nm -n $OUT.elf | awk '$2 ~ /[Tt]/ {print $3; exit}')
if [ "$FIRST" != "_start" ]; then
    echo "ERROR: binary starts with '$FIRST', not _start" >&2
    exit 1
fi
if ${CROSS}objdump -d $OUT.elf | grep -qE '\s(lwc1|swc1|mtc1|mfc1|add\.s|mul\.s|div\.s|cvt\.)'; then
    echo "ERROR: FPU instructions in $OUT.elf (no FPU on this CPU)" >&2
    exit 1
fi
echo "Built $OUT.bin, $(wc -c < $OUT.bin) bytes"
