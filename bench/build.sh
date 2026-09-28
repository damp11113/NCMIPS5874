#!/bin/sh
# Build the DSP ASE benchmark twice (WSL): sh bench/build.sh
#   bench/BENCHP.BIN  plain (-march=mips32r2, what the SDK used before)
#   bench/BENCHD.BIN  with -mdsp
# Needs the Helix MP3 sources in helix/mp3 (see buildmp3.sh).
set -e
cd "$(dirname "$0")/.."
# SF2 / OPL are also checked against the committed code (bench/ref_*.c,
# refresh with: git show HEAD:apps/midi/synth_sf2.c > bench/ref_sf2.c,
# git show HEAD:sdk/opl.c > bench/ref_opl.c).
SRC="bench/bench.c bench/b_sf2.c bench/b_sf2_ref.c bench/b_opl.c bench/b_opl_ref.c bench/b_mp3.c bench/b_doom.c sdk/opl.c"
H="$(echo helix/mp3/*.c)"
DSP= QUIET_SRC="$H" CFLAGS_EXTRA="-Ihelix/mp3 -Iapps/midi" OBJ=build_benchp \
    sh sdk/build.sh bench/BENCHP.BIN $SRC
DSP=-mdsp QUIET_SRC="$H" CFLAGS_EXTRA="-Ihelix/mp3 -Iapps/midi" OBJ=build_benchd \
    sh sdk/build.sh bench/BENCHD.BIN $SRC
