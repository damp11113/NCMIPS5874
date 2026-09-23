#!/bin/sh
# Build a C++ app that uses libgfx (GFX_NC5874 back-end) for U-Boot 'go'.
# Usage (in WSL): ./buildgfx.sh app.cpp out-name [load-addr]
#   LIBGFX env var overrides the library path.
set -e
SRC=${1:?usage: buildgfx.sh app.cpp out-name [load-addr]}
OUT=${2:?usage: buildgfx.sh app.cpp out-name [load-addr]}
LOAD=${3:-0x80008000}
LIBGFX=${LIBGFX:-/mnt/e/MFoES02w/libs/libgfx}
CROSS=mipsel-linux-gnu-
OBJ=build_$OUT
mkdir -p $OBJ

# The 24KEc has no FPU: everything is soft-float, and float helpers come from
# softfp/libsoftfp.a (compiler-rt) because the toolchain's libgcc uses FPU ops.
COMMON="-march=mips32r2 -EL -msoft-float -Os -ffreestanding -mno-abicalls -fno-pic -G 0 \
    -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -Wall"
CXXF="$COMMON -std=gnu++14 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
    -DGFX_NC5874 -I$LIBGFX -I."

# Soft-float runtime (compiler-rt builtins, -msoft-float); built once if missing.
# softfp/stubinc stands in for a glibc header <limits.h> drags in.
if [ ! -f softfp/libsoftfp.a ]; then
    echo "Building softfp/libsoftfp.a ..."
    for f in softfp/*.c; do
        ${CROSS}gcc -march=mips32r2 -EL -msoft-float -O2 -ffreestanding -nostdlib -mno-abicalls \
            -fno-pic -G 0 -w -Isoftfp/stubinc -c $f -o ${f%.c}.o
    done
    ${CROSS}ar rcs softfp/libsoftfp.a softfp/*.o
    rm -f softfp/*.o
fi

for f in start.c libc.c gfx_glue.c; do
    ${CROSS}gcc $COMMON -nostdlib -c $f -o $OBJ/${f%.c}.o
done
${CROSS}gcc $COMMON -c exports.S -o $OBJ/exports.o
for f in GFX DrawReplay nc5874_std; do
    ${CROSS}g++ $CXXF -Wno-reorder -c $LIBGFX/$f.cpp -o $OBJ/$f.o
done
${CROSS}g++ $CXXF -c $SRC -o $OBJ/app.o

${CROSS}g++ -EL -msoft-float -nostdlib -static -no-pie \
    -Wl,--gc-sections -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
    -Wl,--require-defined=_start -Wl,--defsym=LOAD_ADDR=$LOAD -T link.ld \
    -o $OUT.elf $OBJ/start.o $OBJ/app.o $OBJ/GFX.o $OBJ/DrawReplay.o $OBJ/nc5874_std.o \
    $OBJ/gfx_glue.o $OBJ/libc.o $OBJ/exports.o softfp/libsoftfp.a -lgcc 2>&1 \
    | grep -vE "uses -mhard-float|linking abicalls files with non-abicalls" || true
${CROSS}objcopy -O binary $OUT.elf $OUT.bin

# Safety checks: entry point first, and no FPU instructions anywhere
FIRST=$(${CROSS}nm -n $OUT.elf | awk '$2 ~ /[Tt]/ {print $3; exit}')
if [ "$FIRST" != "_start" ]; then
    echo "ERROR: binary starts with '$FIRST', not _start" >&2
    exit 1
fi
FPU=$(${CROSS}objdump -d $OUT.elf | grep -cE "\s(add|sub|mul|div|mov|neg|abs|sqrt)\.[sd]\s|\smtc1\s|\smfc1\s|\slwc1\s|\sswc1\s|\sldc1\s|\ssdc1\s|\scvt\.|\sc\.[a-z]+\.[sd]\s" || true)
if [ "$FPU" != "0" ]; then
    echo "ERROR: $FPU FPU instructions in $OUT.elf (would crash on the 24KEc)" >&2
    exit 1
fi

echo "Built $OUT.bin, $(wc -c < $OUT.bin) bytes, load at $LOAD (no FPU instructions)"
