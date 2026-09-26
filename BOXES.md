# Which file works on which box

Two boxes, same chip family (Montage, MIPS 24KEc, U-Boot 2012.04, 38400 baud,
128 MB DDR2, 8 MB SPI flash):

| | IPTV box | Satellite box |
|---|---|---|
| SoC | M88CS8051B (firmware says cs8001b) | M88CS8002B |
| Front | STANDBY button, 2 LEDs | AiP650: 3-digit 7-segment, 6 buttons |
| Extra | RTL8188FTV WiFi on USB port 1 | DVB-S tuner, Ethernet? (U-Boot env has `mt_eth`) |
| Video decoders | H.264, HEVC, MPEG-2/4, AVS, VC-1, VP8, RV | MPEG-2, MPEG-4, H.264 (manual) |
| Status | main platform, NCAPPS in flash | new; only U-Boot + `cpuinfo` tested |

## Folders

| Folder | For |
|---|---|
| root (`*.c`, `*.h`, `build*.sh`, `mk*.py`) | shared: U-Boot runtime, headers, test and exploration tools |
| `sdk/`, `launcher/`, `apps/`, `doom/`, `ncapps/` | shared: NCAPPS SDK, launcher, apps, staging |
| `iptv/firmware/` | IPTV box flash dump + extracted images (gitignored, private) |
| `iptv/hooks/` | IPTV box only: code patched into its stock firmware / U-Boot |
| `iptv/scripts/` | IPTV box only: U-Boot scripts (`.txt` -> `.scr` with `mkscript.py`) |
| `iptv/logs/` | IPTV box serial captures (gitignored) |
| `sat/` | satellite box: logs, U-Boot image `ubsat.bin` (read from RAM), `ubmatch.py`, plan (`sat/nextgx.md`) |

## Status per program

"Shared" = same CPU and U-Boot, expected to run on both. Only what is
marked "tested" has run on the satellite box.

| File | IPTV | Satellite |
|---|---|---|
| `cpuinfo` | works | **tested** (same results: 24KEc, Count 324 MHz, 430 MIPS) |
| `uboot_hello`, `hello`, `main.c`, `mmutest` | works | shared, untested |
| `regdump`, `regwatch`, `snap*` | works | shared, untested (register ranges from the IPTV box) |
| `irscan`, `irkeys`, `irtest` (use `board.h`: IPTV LED / STANDBY GPIOs) | works | **do not run**: use `irpanel` |
| `led`, `board.h` | works | IPTV GPIOs; satellite has AiP650 instead |
| `fptest`, `i2c.h`, `fd650.h` (SoC I2C master, front panel) | no panel (I2C block probably present) | **tested**: display, keys, LED |
| `irpanel`, `ir.h` (IR test with front display, no board GPIOs) | should work (no panel: prints only) | **tested**: same block 0xbf151000, same codes, user fe01 |
| `ramtest` (upper 64 MB, phys 0x04000000-0x07ffffff) | untested (IPTV: AV core lives there) | **tested: PASS**, upper 64 MB is real, separate RAM |
| `osdinit`, `tvapp`, `gfxdemo`, `vsyncprobe` | works | need an AV/HDMI init script for this box first |
| `audplay`, `wavplay`, `mp3play`, `badapple` | works | need AV init first |
| `rtl*` (WiFi), `ch340test`, `picoterm` | works | shared if a USB device is plugged in |
| `flashdiff` | works | shared |
| `usbspeed`, everything using `usbfat.h` / `ubusb.h` (SDK, launcher, apps, DOOM, WiFi, CH340) | works | internal U-Boot addresses per build in `ubaddr.h` (satellite ones from `sat/ubmatch.py`); untested |
| `vicset`, `regapply` (HDMI set_mode call) | works | **no**: refuse to run (entry check); set_mode data offsets differ |
| NCAPPS: `launcher/`, `apps/`, `doom/`, `sdk/` | works | **works** (`sat/scripts/ncboot.txt`, heap 66 MB, audio, DOOM 70 fps) |
| `iptv/hooks/*` (hooks, `hookpatch`, `fwpatch`, `usbfast`, `vdectest`) | works | **no**: addresses of the IPTV stock firmware / U-Boot build |
| `iptv/scripts/*` (`ncboot`, `avstart`, ...) | works | **no**: IPTV AV memory map; make `sat/` versions |
| `xref.py`, `ubxref.py`, `accessors.py`, `m16dis.sh` | default to `iptv/firmware/` images | edit the path for satellite images |
