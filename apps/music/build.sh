#!/bin/sh
# Build the music player (WSL): sh apps/music/build.sh -> apps/music/MUSIC.BIN
# Needs the Helix MP3 sources in helix/mp3 (see buildmp3.sh).
set -e
cd "$(dirname "$0")/../.."
QUIET_SRC="$(echo helix/mp3/*.c) apps/music/stb_impl.c" CFLAGS_EXTRA="-Ihelix/mp3" OBJ=build_music \
    sh sdk/build.sh apps/music/MUSIC.BIN apps/music/music.c
