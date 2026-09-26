#!/bin/sh
# Build all current U-Boot programs and hooks. Usage (from Windows):
#   wsl sh ./buildall.sh
# Prints FAILED <name> for anything that does not build.
# Shared programs build in the project root; IPTV-box-only ones in iptv/.
[ -f sine256.h ] || python3 mksine.py > sine256.h

for p in irtest irkeys audplay wavplay ch340test picoterm rtlhello rtlfw rtlscan rtlprobe rtljoin rtlnet rtlhttp usbspeed fptest; do
    ./buildc.sh $p.c $p || echo "FAILED $p"
done
# IPTV box: stock-firmware hooks, firmware / U-Boot patches, AV core test
H=iptv/hooks
./buildc.sh $H/vdectest.c $H/vdectest || echo "FAILED vdectest"
./buildc.sh $H/hookpatch.c $H/hookpatch 0x82000000 || echo "FAILED hookpatch"
./buildc.sh $H/fwpatch.c $H/fwpatch 0x82000000 || echo "FAILED fwpatch"
./buildc.sh $H/usbfast.c $H/usbfast 0x83c00000 || echo "FAILED usbfast"
./buildhook.sh $H/hook_irsnap.c $H/hook_irsnap || echo "FAILED hook_irsnap"
./buildhook.sh $H/hook_audsnap.c $H/hook_audsnap $H/hook_entry_irkey.S || echo "FAILED hook_audsnap"
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
for app in sysinfo music midi; do sh apps/$app/build.sh || echo "FAILED $app"; done
# IPTV box boot scripts (flash layout and AV memory map of that box)
S=iptv/scripts
python3 mkscript.py $S/ncboot.txt $S/ncboot.scr || echo "FAILED ncboot.scr"
python3 mkscript.py $S/ncbig.txt $S/ncbig.scr || echo "FAILED ncbig.scr"
