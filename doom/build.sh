#!/bin/sh
# Build Doom (doomgeneric) for the NC5874 box. Usage (in WSL, from doom/):
#   sh ./build.sh            -> doom.bin
#
# doomgeneric sources (GPL-2.0-or-later, not in git) go in doom/doomgeneric:
#   git clone https://github.com/ozkl/doomgeneric.git doomgeneric
#   (tested with commit dcb7a8d)
# Built on the box SDK (../sdk: runtime, C library, box headers) with
# soft-float (../softfp) and link.ld from the project root. The WAD is not
# built in: DOOM.WAD is read from the USB stick at start-up.
set -e
cd "$(dirname "$0")"
CROSS=mipsel-linux-gnu-
DG=doomgeneric/doomgeneric
OBJ=build
GCCINC=$(${CROSS}gcc -print-file-name=include)

CFLAGS="-march=mips32r2 -EL -msoft-float -O2 -ffreestanding -mno-abicalls -fno-pic -G 0 \
    -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-asynchronous-unwind-tables \
    -nostdinc -isystem $GCCINC -I../sdk/libc/include -I$DG -I../sdk -I.. \
    -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND"

SRC="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net
    f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound
    i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc
    m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats
    p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw
    r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables
    v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video
    doomgeneric"

# DeHackEd + WAD merging from Chocolate Doom 2.2.1 (same API as doomgeneric;
# GPL-2.0-or-later, not in git):
#   git clone --depth 1 --branch chocolate-doom-2.2.1 \
#       https://github.com/chocolate-doom/chocolate-doom.git chocolate
# The files are copied into $OBJ/deh so they compile against doomgeneric's
# headers (info.h, w_wad.h, ...), and doomgeneric's doomfeatures.h gets
# FEATURE_DEHACKED and FEATURE_WAD_MERGE switched on.
CH=chocolate/src
DEH_SRC="$CH/deh_io.c $CH/deh_main.c $CH/deh_mapping.c $CH/deh_str.c $CH/deh_text.c
    $CH/w_merge.c $CH/doom/deh_ammo.c $CH/doom/deh_bexstr.c $CH/doom/deh_cheat.c
    $CH/doom/deh_doom.c $CH/doom/deh_frame.c $CH/doom/deh_misc.c $CH/doom/deh_ptr.c
    $CH/doom/deh_sound.c $CH/doom/deh_thing.c $CH/doom/deh_weapon.c"
if [ ! -d $CH ]; then
    echo "chocolate/ missing: see the clone command at the top of build.sh" >&2
    exit 1
fi
if grep -q '^#undef FEATURE_DEHACKED' $DG/doomfeatures.h; then
    sed -i 's|^#undef FEATURE_DEHACKED|#define FEATURE_DEHACKED|; s|^#undef FEATURE_WAD_MERGE|#define FEATURE_WAD_MERGE|' \
        $DG/doomfeatures.h
    rm -rf $OBJ                     # every file includes doomfeatures.h
fi

if [ ! -f ../softfp/libsoftfp.a ]; then
    echo "../softfp/libsoftfp.a missing: run ../buildgfx.sh once (it builds it)" >&2
    exit 1
fi

mkdir -p $OBJ
for s in $SRC; do
    o=$OBJ/$s.o
    if [ ! -f $o ] || [ $DG/$s.c -nt $o ] || [ build.sh -nt $o ]; then
        ${CROSS}gcc $CFLAGS -w -c $DG/$s.c -o $o
    fi
done
mkdir -p $OBJ/deh
for h in deh_defs.h deh_io.h deh_mapping.h; do
    cp $CH/$h $OBJ/deh/
done
DEH_OBJ=""
for f in $DEH_SRC; do
    b=$(basename $f .c)
    if [ ! -f $OBJ/deh/$b.o ] || [ $f -nt $OBJ/deh/$b.o ]; then
        cp $f $OBJ/deh/
        ${CROSS}gcc $CFLAGS -w -c $OBJ/deh/$b.c -o $OBJ/deh/$b.o
    fi
    DEH_OBJ="$DEH_OBJ $OBJ/deh/$b.o"
done
${CROSS}gcc $CFLAGS -Wall -c ../sdk/libc/libc.c -o $OBJ/libc.o
${CROSS}gcc $CFLAGS -Wall -c dg_nc5874.c -o $OBJ/dg_nc5874.o
${CROSS}gcc $CFLAGS -Wall -c dg_sound.c -o $OBJ/dg_sound.o
${CROSS}gcc $CFLAGS -Wall -c dg_music.c -o $OBJ/dg_music.o
${CROSS}gcc $CFLAGS -Wall -c ../sdk/opl.c -o $OBJ/opl.o
${CROSS}gcc $CFLAGS -Wall -c dg_debug.c -o $OBJ/dg_debug.o
${CROSS}gcc $CFLAGS -c ../sdk/libc/ub_exports.S -o $OBJ/ub_exports.o
${CROSS}gcc $CFLAGS -Wall -c ../sdk/runtime.c -o $OBJ/runtime.o

${CROSS}gcc -EL -msoft-float -nostdlib -static -no-pie \
    -Wl,--gc-sections -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
    -Wl,--require-defined=_start -Wl,--defsym=LOAD_ADDR=0x80008000 -T ../link.ld \
    -o doom.elf $OBJ/runtime.o $OBJ/dg_nc5874.o $OBJ/dg_sound.o $OBJ/dg_music.o $OBJ/opl.o $OBJ/dg_debug.o $(for s in $SRC; do echo $OBJ/$s.o; done) \
    $DEH_OBJ $OBJ/libc.o $OBJ/ub_exports.o ../softfp/libsoftfp.a -lgcc 2>&1 \
    | grep -vE "uses -mhard-float|linking abicalls files with non-abicalls" || true
${CROSS}objcopy -O binary doom.elf doom.bin

FIRST=$(${CROSS}nm -n doom.elf | awk '$2 ~ /[Tt]/ {print $3; exit}')
if [ "$FIRST" != "_start" ]; then
    echo "ERROR: binary starts with '$FIRST', not _start" >&2
    exit 1
fi
if ${CROSS}objdump -d doom.elf | grep -qE '\s(lwc1|swc1|ldc1|sdc1|mtc1|mfc1|add\.[sd]|mul\.[sd]|div\.[sd]|cvt\.)'; then
    echo "ERROR: FPU instructions in doom.elf (no FPU on this CPU)" >&2
    exit 1
fi
END=$(${CROSS}nm -n doom.elf | awk '$3 == "__bss_end" {print $1}')
echo "Built doom.bin, $(wc -c < doom.bin) bytes, image ends at 0x$END"
