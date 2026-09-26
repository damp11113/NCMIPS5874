# Which file works on which box

Two boxes, same chip family (Montage, MIPS 24KEc ~648 MHz, U-Boot 2012.04,
128 MB DDR2, 8 MB SPI flash). One USB stick and the same binaries serve
both: the SDK recognises the box from its U-Boot build (`ubaddr.h`).

| | IPTV box | Satellite box |
|---|---|---|
| SoC | M88CS8051B (firmware says cs8001b) | M88CS8002B (firmware says cs8001b 64m) |
| U-Boot build | Nov 21 2022 | May 09 2026 |
| Front | STANDBY button, 2 LEDs (red / green) | FD650 (AiP650): 3-digit 7-segment, green LED, 6 buttons |
| Extra | RTL8188FTV WiFi on USB port 1 | DVB-S tuner, Ethernet? (U-Boot env has `mt_eth`) |
| Video decoders | H.264, HEVC, MPEG-2/4, AVS, VC-1, VP8, RV | MPEG-2, MPEG-4, H.264 (manual) |
| Boot script in flash (0xa0000) | `iptv/scripts/ncboot.txt` | `sat/scripts/ncboot.txt` |

## NCAPPS support

**Both boxes run NCAPPS** (launcher, SDK apps, DOOM) from the same stick:
power-on with the stick boots the launcher, without it the stock firmware.

| Feature | IPTV box | Satellite box |
|---|---|---|
| Launcher, SDK apps, DOOM | yes | yes (verified: launcher, audio, DOOM 70 fps) |
| Heap for apps | 34 MB, ~95 MB with "Big memory" | 66 MB (upper 64 MB free with the stock 64M AV layout) |
| "Big memory" setting | yes (settings page) | hidden (not needed; value in SETTINGS.TXT kept for the IPTV box) |
| HDMI output / OSD | yes | yes |
| Audio | yes | yes |
| USB stick (FAT32, fast reads ~26 MB/s) | yes | yes |
| Remote (IR, user code 0xfe01) | yes | yes (same codes) |
| Front buttons as input | STANDBY | MENU, OK, VOL-/+ (LEFT/RIGHT), CH-/+ (DOWN/UP) |
| Front display (`sdk_panel_show`) | - (no display) | entry number, `---`, `SEt`, `Err`; MIDI voices, music time, `PAU`, volume |
| Front LED (`sdk_panel_led`) | red / green LEDs (standby) | green LED: off in the menu, blinks with the music (MIDI: quarter / whole / second, saved), steady when paused, 3 blinks at standby |
| App config (`sdk_config_*`, APPSDATA/<app>/CONFIG.TXT) | yes | yes |
| Soft standby / reboot | POWER, STANDBY wakes | POWER, front MENU wakes |
| EXIT to the stock firmware | yes (with big memory: via restart) | yes |
| WiFi (`rtl*` programs) | yes (internal RTL8188FTV) | no WiFi module (only a USB one would work) |
| Hardware video decoding | experiments (`iptv/hooks/vdectest`) | not started |

## Folders

| Folder | For |
|---|---|
| root (`*.c`, `*.h`, `build*.sh`, `mk*.py`) | shared: U-Boot runtime, headers, test and exploration tools |
| `sdk/`, `launcher/`, `apps/`, `doom/`, `ncapps/` | shared: NCAPPS SDK, launcher, apps, staging |
| `iptv/firmware/` | IPTV box flash dump + extracted images (gitignored, private) |
| `iptv/hooks/` | IPTV box only: code patched into its stock firmware / U-Boot |
| `iptv/scripts/` | IPTV box only: U-Boot scripts (`.txt` -> `.scr` with `mkscript.py`) |
| `iptv/logs/` | IPTV box serial captures (gitignored) |
| `sat/` | satellite box: `NOTES.md` (all findings), `scripts/` (boot script), `ubmatch.py`, logs; flash dump / U-Boot / firmware images (gitignored) |
| `SSerHial/` | serial hub: box console over SSH + upload tool (gitignored, local only) |

## Status per program

"Shared" = same CPU and U-Boot, expected to run on both. "Tested" = has run
on the satellite box.

| File | IPTV | Satellite |
|---|---|---|
| NCAPPS: `launcher/`, `apps/`, `doom/`, `sdk/` | works | **works** (see above) |
| `usbspeed`, everything using `usbfat.h` / `ubusb.h` | works | **tested**: internal U-Boot addresses per build in `ubaddr.h` |
| `cpuinfo` | works | **tested** (same results: 24KEc, Count 324 MHz, 430 MIPS) |
| `fptest`, `i2c.h`, `fd650.h` (SoC I2C master, front panel) | no panel | **tested**: display, keys, LED (reg 1 bit 3) |
| `irpanel`, `ir.h` | should work (no panel: prints only) | **tested**: same IR block 0xbf151000, same codes |
| `ramtest` (upper 64 MB) | do not run (IPTV: AV core lives there) | **tested: PASS** |
| `timertest` (get_timer vs CP0 Count) | should work | **tested**: get_timer fine |
| `serload` (serial upload loader, used by SSerHial) | should work | **tested** |
| `uboot_hello`, `hello`, `main.c`, `mmutest` | works | shared, untested |
| `regdump`, `regwatch`, `snap*` | works | shared, untested (register ranges from the IPTV box) |
| `irscan`, `irkeys`, `irtest` (use `board.h`: IPTV LED / STANDBY GPIOs) | works | **do not run**: use `irpanel` |
| `led`, `board.h` | works | IPTV GPIOs only; the SDK redirects them on the satellite box |
| `osdinit`, `tvapp`, `gfxdemo`, `vsyncprobe` | works | untested outside NCAPPS (need the AV init from the boot script) |
| `audplay`, `wavplay`, `mp3play`, `badapple` | works | untested outside NCAPPS (need AV init) |
| `rtl*` (WiFi), `ch340test`, `picoterm` | works | shared if such a USB device is plugged in |
| `flashdiff` | works | shared |
| `vicset`, `regapply` (HDMI set_mode call) | works | **no**: refuse to run (entry check); set_mode data offsets differ |
| `iptv/hooks/*` (hooks, `hookpatch`, `fwpatch`, `usbfast`, `vdectest`) | works | **no**: addresses of the IPTV stock firmware / U-Boot build |
| `iptv/scripts/*` (`ncboot`, `avstart`, ...) | works | **no**: IPTV AV memory map; use `sat/scripts/` |
| `xref.py`, `ubxref.py`, `accessors.py`, `m16dis.sh` | default to `iptv/firmware/` images | edit the path for satellite images |

**Both boxes:** never use U-Boot's `loady` / `loadb` (they feed a flash
writer and never load RAM, see `sat/NOTES.md`); upload over serial with
SSerHial (`serload`) or load from the stick with `fatload`.
