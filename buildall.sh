#!/bin/sh
# Build all current U-Boot programs and hooks. Usage (from Windows):
#   wsl sh ./buildall.sh
# Prints FAILED <name> for anything that does not build.
[ -f sine256.h ] || python3 mksine.py > sine256.h

for p in irtest irkeys audplay wavplay ch340test picoterm rtlhello rtlfw rtlscan rtlprobe usbspeed vdectest; do
    ./buildc.sh $p.c $p || echo "FAILED $p"
done
./buildc.sh hookpatch.c hookpatch 0x82000000 || echo "FAILED hookpatch"
./buildc.sh fwpatch.c fwpatch 0x82000000 || echo "FAILED fwpatch"
./buildhook.sh hook_irsnap.c hook_irsnap || echo "FAILED hook_irsnap"
./buildhook.sh hook_audsnap.c hook_audsnap hook_entry_irkey.S || echo "FAILED hook_audsnap"
if [ -f helix/mp3/mp3dec.c ]; then
    ./buildmp3.sh mp3play.c mp3play || echo "FAILED mp3play"
    ./buildmp3.sh badapple.c badapple || echo "FAILED badapple"
else
    echo "SKIPPED mp3play, badapple (Helix sources missing, see buildmp3.sh)"
fi
# SDK apps and the NCAPPS launcher
sh sdk/build.sh build_sdk/HELLO.BIN sdk/examples/hello.c || echo "FAILED hello"
LOAD=0x80800000 MAX_END=0x80a00000 OBJ=build_launcher sh sdk/build.sh launcher/LAUNCHER.BIN launcher/launcher.c launcher/crash_entry.S || echo "FAILED launcher"
[ -d doom/doomgeneric ] && [ -d doom/chocolate ] && { sh doom/build.sh || echo "FAILED doom"; }
python3 mkscript.py ncboot.txt ncboot.scr || echo "FAILED ncboot.scr"
for app in sysinfo music midi; do sh apps/$app/build.sh || echo "FAILED $app"; done
python3 mkscript.py ncbig.txt ncbig.scr || echo "FAILED ncbig.scr"
