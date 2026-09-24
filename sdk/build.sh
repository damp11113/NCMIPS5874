#!/bin/sh
# Build an app with the NC5874 box SDK. Usage (in WSL, any folder):
#   sh path/to/sdk/build.sh OUT.BIN app.c [more.c ...]
# Environment:
#   LOAD=0x80008000       load / link address (the launcher uses 0x80800000)
#   MAX_END=0x80800000    image + .bss must end below this (launcher area)
#   CFLAGS_EXTRA=...      more compiler flags (-D, -I, -O)
#   OBJ=build_sdk         object folder
#   QUIET_SRC="a.c b.c"   third-party sources: built without warnings, only
#                         when changed (e.g. the Helix MP3 decoder)
# Links: sdk/runtime.c (entry, files, input), sdk/libc, U-Boot export
# stubs, soft-float (softfp/libsoftfp.a) and libgcc. No FPU instructions
# are allowed (the 24KEc has none); the build checks that.
set -e
SDK=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SDK/.." && pwd)
OUT=${1:?usage: build.sh OUT.BIN app.c [more.c ...]}
shift
[ $# -gt 0 ] || { echo "usage: build.sh OUT.BIN app.c [more.c ...]" >&2; exit 1; }
LOAD=${LOAD:-0x80008000}
MAX_END=${MAX_END:-0x80800000}
OBJ=${OBJ:-build_sdk}
CROSS=mipsel-linux-gnu-
GCCINC=$(${CROSS}gcc -print-file-name=include)
ELF=${OUT%.*}.elf

CFLAGS="-march=mips32r2 -EL -msoft-float -O2 -ffreestanding -mno-abicalls -fno-pic -G 0 \
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-asynchronous-unwind-tables \
    -nostdinc -isystem $GCCINC -I$SDK/libc/include -I$SDK -I$ROOT -Wall $CFLAGS_EXTRA"

if [ ! -f $ROOT/softfp/libsoftfp.a ]; then
    echo "$ROOT/softfp/libsoftfp.a missing: run buildgfx.sh once (it builds it)" >&2
    exit 1
fi

mkdir -p $OBJ
OBJS=""
for f in "$@"; do
    o=$OBJ/$(basename ${f%.*}).o
    ${CROSS}gcc $CFLAGS -c $f -o $o
    OBJS="$OBJS $o"
done
# Third-party sources: no warnings, rebuilt only when changed
for f in $QUIET_SRC; do
    o=$OBJ/q_$(basename ${f%.*}).o
    if [ ! -f $o ] || [ $f -nt $o ]; then
        ${CROSS}gcc $CFLAGS -w -c $f -o $o
    fi
    OBJS="$OBJS $o"
done
${CROSS}gcc $CFLAGS -c $SDK/runtime.c -o $OBJ/sdk_runtime.o
${CROSS}gcc $CFLAGS -c $SDK/libc/libc.c -o $OBJ/sdk_libc.o
${CROSS}gcc $CFLAGS -c $SDK/libc/ub_exports.S -o $OBJ/sdk_ub_exports.o

${CROSS}gcc -EL -msoft-float -nostdlib -static -no-pie \
    -Wl,--gc-sections -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
    -Wl,--require-defined=_start -Wl,--defsym=LOAD_ADDR=$LOAD -T $ROOT/link.ld \
    -o $ELF $OBJ/sdk_runtime.o $OBJS $OBJ/sdk_libc.o $OBJ/sdk_ub_exports.o \
    $ROOT/softfp/libsoftfp.a -lgcc 2>&1 \
    | grep -vE "uses -mhard-float|linking abicalls files with non-abicalls" || true
${CROSS}objcopy -O binary $ELF $OUT

FIRST=$(${CROSS}nm -n $ELF | awk '$2 ~ /[Tt]/ {print $3; exit}')
if [ "$FIRST" != "_start" ]; then
    echo "ERROR: binary starts with '$FIRST', not _start" >&2
    exit 1
fi
if ${CROSS}objdump -d $ELF | grep -qE '\s(lwc1|swc1|ldc1|sdc1|mtc1|mfc1|add\.[sd]|mul\.[sd]|div\.[sd]|cvt\.)'; then
    echo "ERROR: FPU instructions in $ELF (no FPU on this CPU)" >&2
    exit 1
fi
END=$(${CROSS}nm -n $ELF | awk '$3 == "__bss_end" {print $1}')
if [ $((0x$END)) -gt $(($MAX_END)) ]; then
    echo "ERROR: image ends at 0x$END, past $MAX_END" >&2
    exit 1
fi
echo "Built $OUT, $(wc -c < $OUT) bytes, runs at $LOAD, ends at 0x$END"
