#!/bin/sh
# Put the NCAPPS tree on the USB stick. Usage (Git Bash or WSL, project root):
#   sh ncapps/stage.sh /h            (the stick's mount point)
# Copies the menu entries (ncapps/), the built binaries as each app's
# APP.BIN, the app data found in the project root (WADs, .bav), and makes
# NCAPPS/APPSDATA/<app>. Big files are only copied when they differ.
# Every copy is checked with cmp. Build first: launcher, doom, badapple,
# tvapp, hello, apps/*/build.sh (see continue.md). ncapps/local.sh (not in
# git) may add your own files with put, e.g. MP3s, MIDIs, a SoundFont.
set -e
DST=${1:?usage: stage.sh <stick mount point>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NC=$DST/NCAPPS

put () {                            # put <source> <destination>
    if [ ! -f "$1" ]; then
        echo "  skip (missing): $1"
        return
    fi
    mkdir -p "$(dirname "$2")"
    if ! cmp -s "$1" "$2" 2>/dev/null; then
        cp "$1" "$2"
    fi
    cmp "$1" "$2"
    echo "  ok $2"
}

[ -d "$DST" ] || { echo "no $DST (stick not in the PC?)" >&2; exit 1; }

put "$ROOT/launcher/LAUNCHER.BIN" "$NC/LAUNCHER.BIN"
[ -f "$NC/LAUNCHER.INI" ] || put "$ROOT/ncapps/LAUNCHER.INI" "$NC/LAUNCHER.INI"

for inf in "$ROOT"/ncapps/APPS/*/*.INF; do
    app=$(basename "$(dirname "$inf")")
    put "$inf" "$NC/APPS/$app/$(basename "$inf")"
    mkdir -p "$NC/APPSDATA/$app"
done

put "$ROOT/doom/doom.bin" "$NC/APPS/DOOM/APP.BIN"
for f in DOOM.WAD DOOM2.WAD MARINE1.WAD SUPER2.DEH; do
    put "$ROOT/$f" "$NC/APPS/DOOM/$f"
done
put "$ROOT/badapple.bin" "$NC/APPS/BADAPPLE/APP.BIN"
put "$ROOT/badapple.bav" "$NC/APPS/BADAPPLE/BADAPPLE.BAV"
put "$ROOT/build_sdk/HELLO.BIN" "$NC/APPS/HELLO/APP.BIN"
put "$ROOT/tvapp.bin" "$NC/APPS/TVDEMO/APP.BIN"
put "$ROOT/apps/sysinfo/SYSINFO.BIN" "$NC/APPS/SYSINFO/APP.BIN"
put "$ROOT/apps/music/MUSIC.BIN" "$NC/APPS/MUSIC/APP.BIN"
put "$ROOT/apps/midi/MIDI.BIN" "$NC/APPS/MIDI/APP.BIN"
# Optional, not in git: your own media / SoundFonts (see ncapps/local.sh.example)
[ -f "$ROOT/ncapps/local.sh" ] && . "$ROOT/ncapps/local.sh"
put "$ROOT/ncboot.scr" "$DST/ncboot.scr"
put "$ROOT/ncbig.scr" "$DST/ncbig.scr"
echo "NCAPPS staged on $DST"
