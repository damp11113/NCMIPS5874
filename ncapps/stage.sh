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
put "$ROOT/iptv/hooks/fwpatch.bin" "$NC/FWPATCH.BIN"
[ -f "$NC/LAUNCHER.INI" ] || put "$ROOT/ncapps/LAUNCHER.INI" "$NC/LAUNCHER.INI"
# Settings: written on the box (launcher overwrites the one sector in place,
# must stay exactly 512 bytes), so only created when missing or broken
if [ "$(wc -c < "$NC/SETTINGS.TXT" 2>/dev/null)" != 512 ]; then
    rm -f "$NC/SETTINGS.TXT"
    put "$ROOT/ncapps/SETTINGS.TXT" "$NC/SETTINGS.TXT"
fi
# WiFi (IPTV box, Multitasking on): the chip's firmware from linux-firmware
# (rtl8188fufw.bin in the project root, not in git) and the chosen network,
# written on the box like SETTINGS.TXT. A new WIFI.TXT takes ssid= / psk=
# from the stick's root WIFI.TXT (the rtl* tools' file) when there is one.
put "$ROOT/rtl8188fufw.bin" "$NC/RTLWIFI.BIN"
if [ "$(wc -c < "$NC/WIFI.TXT" 2>/dev/null)" != 512 ]; then
    rm -f "$NC/WIFI.TXT"
    ssid=
    psk=
    if [ -f "$DST/WIFI.TXT" ]; then
        ssid=$(sed -n 's/^ssid=//p' "$DST/WIFI.TXT" | tr -d '\r' | head -n 1)
        psk=$(sed -n 's/^psk=//p' "$DST/WIFI.TXT" | tr -d '\r' | head -n 1)
    fi
    printf '%s\n%s\n%s\non=1\nssid=%s\npsk=%s\n' "# NCAPPS wifi" \
        "# Written by the launcher (SETTINGS > WiFi). on=0: WiFi off." \
        "# The passphrase is stored as typed: keep this stick private." \
        "$ssid" "$psk" > "$NC/WIFI.TXT"
    head -c $((511 - $(wc -c < "$NC/WIFI.TXT"))) /dev/zero | tr '\0' '#' >> "$NC/WIFI.TXT"
    printf '\n' >> "$NC/WIFI.TXT"
    echo "  ok $NC/WIFI.TXT${ssid:+ (network from $DST/WIFI.TXT)}"
fi

for inf in "$ROOT"/ncapps/APPS/*/*.INF; do
    app=$(basename "$(dirname "$inf")")
    put "$inf" "$NC/APPS/$app/$(basename "$inf")"
    mkdir -p "$NC/APPSDATA/$app"
    # App config (sdk_config_*): written on the box like SETTINGS.TXT, so
    # only created when missing or broken, never overwritten
    cfg=$NC/APPSDATA/$app/CONFIG.TXT
    if [ ! -f "$cfg" ] || [ "$(wc -c < "$cfg")" != 512 ]; then
        rm -f "$cfg"
        put "$ROOT/ncapps/CONFIG.TXT" "$cfg"
    fi
done

put "$ROOT/doom/doom.bin" "$NC/APPS/DOOM/APP.BIN"
for f in DOOM.WAD DOOM2.WAD MARINE1.WAD SUPER2.DEH; do
    put "$ROOT/$f" "$NC/APPS/DOOM/$f"
done
put "$ROOT/badapple.bin" "$NC/APPS/BADAPPLE/APP.BIN"
put "$ROOT/badapple.bav" "$NC/APPS/BADAPPLE/BADAPPLE.BAV"
put "$ROOT/build_sdk/HELLO.BIN" "$NC/APPS/HELLO/APP.BIN"
put "$ROOT/build_sdk/I2CSCAN.BIN" "$NC/APPS/I2CSCAN/APP.BIN"
put "$ROOT/apps/brimod/BRIMOD.BIN" "$NC/APPS/BRIMOD/APP.BIN"
put "$ROOT/apps/rtostest/RTOSTEST.BIN" "$NC/APPS/RTOSTEST/APP.BIN"
put "$ROOT/tvapp.bin" "$NC/APPS/TVDEMO/APP.BIN"
put "$ROOT/apps/sysinfo/SYSINFO.BIN" "$NC/APPS/SYSINFO/APP.BIN"
put "$ROOT/apps/music/MUSIC.BIN" "$NC/APPS/MUSIC/APP.BIN"
put "$ROOT/apps/midi/MIDI.BIN" "$NC/APPS/MIDI/APP.BIN"
put "$ROOT/apps/nettest/NETTEST.BIN" "$NC/APPS/NETTEST/APP.BIN"
put "$ROOT/apps/tjcplay/TJCPLAY.BIN" "$NC/APPS/TJCPLAY/APP.BIN"
# Optional, not in git: your own media / SoundFonts (see ncapps/local.sh.example)
[ -f "$ROOT/ncapps/local.sh" ] && . "$ROOT/ncapps/local.sh"
put "$ROOT/iptv/scripts/ncboot.scr" "$DST/ncboot.scr"
put "$ROOT/iptv/scripts/ncbig.scr" "$DST/ncbig.scr"
put "$ROOT/sat/scripts/ncboot.scr" "$DST/satboot.scr"
echo "NCAPPS staged on $DST"
