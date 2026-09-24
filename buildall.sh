#!/bin/sh
# Build all current U-Boot programs and hooks. Usage (from Windows):
#   wsl sh ./buildall.sh
# Prints FAILED <name> for anything that does not build.
[ -f sine256.h ] || python3 mksine.py > sine256.h

for p in irtest irkeys audplay wavplay ch340test picoterm rtlhello rtlfw rtlscan rtlprobe; do
    ./buildc.sh $p.c $p || echo "FAILED $p"
done
./buildc.sh hookpatch.c hookpatch 0x82000000 || echo "FAILED hookpatch"
./buildhook.sh hook_irsnap.c hook_irsnap || echo "FAILED hook_irsnap"
./buildhook.sh hook_audsnap.c hook_audsnap hook_entry_irkey.S || echo "FAILED hook_audsnap"
