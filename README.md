# Bare-metal apps on a cheap set-top box

<img width="4080" height="2296" alt="20260925_055207" src="https://github.com/user-attachments/assets/ade16948-5731-4803-a8c1-39fbd4badce7" />
<img width="4080" height="2296" alt="20260925_055132" src="https://github.com/user-attachments/assets/fcc1f144-5607-45e4-ae96-faa9fe81e5af" />
<img width="4080" height="2296" alt="20260925_055010" src="https://github.com/user-attachments/assets/1b6652eb-61f0-435a-8a57-7b39b3037c01" />
<img width="4080" height="2296" alt="20260923_235438" src="https://github.com/user-attachments/assets/69d968d6-3ad3-44dc-a6e6-2b390f6ccd4e" />

Running my own code on a cheap DVB-T2/IPTV set-top box (board
`PCB-CS8051M`) — no Linux, no stock firmware, straight from U-Boot — with
graphics on HDMI, the front-panel button and LED, and the [libgfx](https://github.com/DPSoftware-Technologies/MFoES02w/tree/master/libs/libgfx) graphics
library ported to it.

Everything here was found by reverse engineering (there is no datasheet).
The detailed lab notebook, with every experiment and register value, is
[`continue.md`](continue.md).

## Status

| Works | Notes |
|---|---|
| C and C++ programs started from U-Boot (`go`) | `buildc.sh` (C), `buildgfx.sh` (C++ + libgfx) |
| Serial console in/out, `printf`, timers | via U-Boot's exported functions |
| STANDBY button, red/green power LED | GPIO, see `board.h` |
| HDMI graphics without the stock firmware | OSD layer, 1280x720 ARGB1555 scaled to the output |
| 1080p output | **1080p60** (default), 1080p50 or 1080i50, set in the launcher; see [Video mode](#video-mode-1080p) |
| `libgfx` (`LinuxGFX` / `DrawReplay`) | new `GFX_NC5874` back-end, soft-float, dirty-rect + vsync |
| Boot straight into my apps | flash boot script: NCAPPS launcher from USB, else stock firmware |
| Boot ad removed | ad sector erased; stock firmware shows its fallback splash |

Not done / parked: app stored in flash, hardware video decoding
(experiments in `iptv/hooks/vdectest`, see `continue.md`).

## Hardware

- SoC: Nationalchip "Symphony", chip id `5874`, **MIPS 24KEc** @ ~648 MHz,
  little-endian, 16 KB I + 16 KB D cache, **no FPU**, DSP ASE, MIPS16e.
- RAM: 128 MB DDR2 (U-Boot manages 20 MB). Flash: 8 MB SPI (XM25QH64A).
- Second CPU core runs the AV decoder firmware (`avcpu`).
- Front: STANDBY button, IR receiver, bi-colour power LED, WiFi LED.
  Rear: HDMI, CVBS + stereo RCA, USB-A. No Ethernet.
- Console: UART (soldered wires). U-Boot 2012.04, 3 s boot delay.

## Flash map

| Offset | Contents |
|---|---|
| `0x000000` | first-stage boot |
| `0x010000` | U-Boot |
| `0x0A0000` | boot script (uImage script) — **replaced by `ncboot.scr`** |
| `0x0B0000` | avcpu firmware (LZMA) |
| `0x130000` | 1280x720 PNG |
| `0x300000` | stock main firmware (LZMA, runs at `0x80008000`) |
| `0x700000` | boot ad MP4 — **first 64 KB erased** |
| `0x7B0000..` | other firmware data (untouched) |

A full dump is in `backup.bin` (keep a copy on the USB stick).

## Boot flow (current flash)

`ncboot.scr` (from `iptv/scripts/ncboot.txt`; the satellite box has its own,
`sat/scripts/ncboot.txt`) in the boot-script sector:

1. reads `NCAPPS/SETTINGS.TXT` from the stick (`env import`): big memory,
   video mode, ...,
2. patches U-Boot's display mode for 1080p (see [Video mode](#video-mode-1080p)),
3. starts the AV core and HDMI through U-Boot's own driver (`av_launch`),
4. `usb start`; if the stick has **`NCAPPS/LAUNCHER.BIN`**, runs the
   NCAPPS launcher (app menu),
5. otherwise (or when the launcher exits) boots the stock firmware.

Without the stick nothing is patched: 1080i50 and the stock firmware, as
before. Press a key during the countdown to get the `U-boot#` prompt.

**Restore the original boot script:**
```
usb start
fatload usb 0 0x82000000 backup.bin
sf probe 0
sf erase 0xa0000 0x10000
sf write 0x820a0000 0xa0000 0x10000
reset
```
(Same pattern with `0x700000` restores the ad sector.)

## Video mode (1080p)

U-Boot's `av_launch` sets up the display in **1080i50**: its display init
calls the mode setter with a constant, one `li a2,9` instruction (HD mode
9). The boot script overwrites that instruction in RAM before `av_launch`,
and U-Boot then programs the mixer, the 148.5 MHz pixel clock and the HDMI
transmitter for the new mode by itself:

| `SETTINGS.TXT` | Word written | HD mode | Output |
|---|---|---|---|
| `video=1080p60` (default with the stick) | `0x2406000a` | 10 | 1920x1080p 60 Hz |
| `video=1080p50` | `0x2406000e` | 14 | 1920x1080p 50 Hz |
| `video=1080i` | (not patched) | 9 | 1920x1080i 50 Hz |

Address (uncached, U-Boot relocation offset `0x0127c000`): `0xa13d382c` on
the IPTV box, `0xa13d4114` on the satellite box. The script only writes it
when the word still reads `0x24060009`, so an unknown U-Boot build stays at
1080i. Change the mode in the launcher (SETTINGS → Video output, RED
restarts) or edit the stick's `SETTINGS.TXT`. By hand from the prompt:
```
md.l 0xa13d382c 1                  # IPTV box: must read 24060009
mw.l 0xa13d382c 0x2406000a         # 1080p60; before av_launch / avstart.scr
```

The OSD needs two things at 1080p, both in `osdsetup.h`: the stock
firmware's progressive scaler mode (`0xbf440100` = `0x000c0003`, 4.12
ratios), and clearing bit 8 of `0xbf440124`, a vertical-scaler bypass that
U-Boot sets at 1080p (the 720 OSD lines otherwise show 1:1, the picture
fills only the top 2/3). The SDK clears that bit again from its key / idle
loop. On every 1080p start U-Boot's first HDMI setup reports "Video not
stable", the retry right after works.

## Building

Toolchain: WSL with `mipsel-linux-gnu-gcc` / `g++`. Programs load at
`0x80008000`.

**C program** (U-Boot services via `uboot.h`, board I/O via `board.h`,
OSD via `osdsetup.h` + `osd.h`):
```
./buildc.sh myapp.c myapp          # -> myapp.bin
```

**C++ program with libgfx** (library at `E:\MFoES02w\libs\libgfx`,
override with `LIBGFX=...`):
```
./buildgfx.sh gfxdemo.cpp gfxdemo  # -> gfxdemo.bin
```
`buildgfx.sh` refuses to produce a binary that contains FPU instructions,
and builds `softfp/libsoftfp.a` from the sources on first use.

**Git:** `.gitignore` keeps only sources and docs. Build outputs, generated
`.scr` scripts, serial captures and the **firmware dumps** (`backup.bin` and
the images extracted from it) are ignored on purpose: the dump contains saved
WiFi passwords and vendor keys, and serial logs of the stock firmware print
the WiFi passwords too. Keep `backup.bin` safe outside the repo.

**Run from U-Boot:**
```
usb start
fatload usb 0 0x80100000 avstart.scr
source 0x80100000                  # AV core + HDMI on, 1080i (not needed with ncboot)
fatload usb 0 ${a} myapp.bin
go ${a}
```
For 1080p, write the mode word first (see [Video mode](#video-mode-1080p)).
To make an app start at power-on, add it to the NCAPPS menu (`ncapps/`)
or set `autostart=` in `NCAPPS/LAUNCHER.INI`.

### Programming notes

- Calls into U-Boot must go through `$t9` (U-Boot is PIC) — `exports.S` does it.
- `start.c` clears `.bss` and runs C++ global constructors; destructors never run.
- `libc.c` has `memcpy`, `memset`, `strlen`, `strcmp`, `parse_hex`, ...
- Use integers where you can: floats are software-emulated.
- The CPU has the MIPS DSP ASE rev 1 (U-Boot leaves it enabled): SDK, DOOM,
  MP3 and libgfx builds use `-mdsp` (MP3 decoding 1.27x faster).
  `bench/` measures the hot loops with and without it.
- `RGB (0, 0, 255)` exactly (`0x801f`) is the OSD colour key (transparent);
  `osd.h` / libgfx nudge it to `0x801e`.
- U-Boot accepts at most 16 words per command.

## libgfx on the box (`GFX_NC5874`)

Added to the library itself (`E:\MFoES02w\libs\libgfx`, uncommitted):

- `GFX.h` / `GFX.cpp`: `#ifdef GFX_NC5874` back-end. `LinuxGFX gfx (1280, 720);`
  draws into an ARGB8888 RAM surface; `swapBuffers ()` converts the dirty
  rectangle to the ARGB1555 OSD plane, synced to the next field (vsync).
- Common code: `writeFastHLine` / `writeFastVLine` fast paths when
  `m_pBuffer` is a real ARGB8888 buffer (speeds up every back-end).
- `nc5874_std.h` / `.cpp`: the libc / libstdc++ / libm subset the library
  uses, on four platform hooks (`gfx_nc5874_malloc/free/log/millis`).
- `CMakeLists.txt`: option `GFX_NC5874_SUPPORT`.

Project side: `gfx_glue.c` implements the hooks (26 MB heap at
`0x81600000..0x82ff0000`), `softfp/libsoftfp.a` provides soft-float
(LLVM compiler-rt 18.1.8 builtins, Apache-2.0 WITH LLVM-exception, license in
`softfp/LICENSE.TXT`) because the toolchain's `libgcc` uses FPU instructions.

Measured: full-screen convert ~40 ms; small updates (bouncing ball demo)
93 fps before vsync, 50 updates/s with vsync. `DrawReplay` blobs render via
`DRRender ()` (237-byte blob in the demo vs 3.7 MB raw frame).

Known libgfx issue (all back-ends): `fillCircle` with alpha < 255 shows
vertical stripes (overlapping spans blended twice). The vsync wait assumes
1080i timing (2640x1125 per frame, two fields): not yet adapted to 1080p.

## Memory map (while an app runs)

| Address | Use |
|---|---|
| `0x80000000..0x80000dff` | exception vectors |
| `0x80004000..0x80006fff` | hook code (stock-firmware hooks only) |
| `0x80008000` | app load address |
| `0x80100000` | U-Boot load area (scripts) |
| `0x81600000..0x82ff0000` | libgfx heap (`gfx_glue.c`) |
| `0x83000000` / `0x83001000` | OSD region header / pixels (phys `0x03000000`) |
| `0x849c8000`, `0xa469dc00..`, `0xa4b58000..` | AV core buffers (don't touch) |
| `0x87e10000` | avcpu firmware |

## Key registers

| Register | Meaning |
|---|---|
| `0xbf0a0008` bit 11 | STANDBY button, 0 = pressed |
| `0xbf155000` bits 6 / 7 | power LED red / green, 1 = on (GPIO bank 64-71) |
| `0xbf15c004` | key ADC (63 = no key) |
| `0xbf441028` | OSD layer 6 region header address >> 3 |
| `0xbf440100..0xbf44012c` | OSD scaler (`0x000e0001` = 1080i mode, 16.16 ratios; `0x000c0003` = progressive, 4.12) |
| `0xbf440124` bit 8 | OSD vertical scaler bypass (U-Boot sets it at 1080p; must be 0) |
| `0xbf440144` | mixer lines + 1 (`0x21d` 1080i field, `0x439` 1080p) |
| `0xbf4400b8` | display output size per field (h<<16 \| w; 540 = 1080i) |
| `0xbf47008c` | scan position in pixel clocks (0..2969999 per 1080i frame) |
| `0xbf480000 + reg` | HDMI transmitter byte registers (bank 1 at `+0x100`); reg `0x26/0x27` = measured Htotal (2200 = 60 Hz, 2640 = 50 Hz) |
| `0xbf5d005c` low byte | video clock select (`0x14` 74.25 MHz, `0x25` 148.5 MHz) |

More (GPIO banks, OSD header layout, HDMI mode table, U-Boot function
addresses) in `continue.md`.

## Files

| File(s) | Purpose |
|---|---|
| `start.c`, `exports.S`, `libc.c`, `uboot.h`, `link.ld` | runtime for U-Boot programs |
| `board.h` | LEDs + STANDBY button helpers |
| `osdsetup.h`, `osd.h`, `font8x16.h` | OSD from plain U-Boot + tiny C drawing lib |
| `tvapp.c`, `osdinit.c`, `main.c`, `led.c`, `cpuinfo.c` | example / test programs |
| `gfxdemo.cpp`, `gfx_glue.c`, `buildgfx.sh`, `softfp/` | libgfx on the box |
| `iptv/scripts/*.txt`, `mkscript.py` | U-Boot scripts (`.txt` -> `.scr`) |
| `iptv/hooks/` (`hook*.c`, `hookpatch.c`, `fwpatch.c`, `usbfast.c`), `buildhook.sh` | run code inside the stock firmware |
| `regwatch.c`, `regdump.c`, `snap*.c`, `regapply.c`, `vicset.c`, `vsyncprobe.c`, `irscan.c` | hardware exploration tools |
| `xref.py`, `ubxref.py`, `accessors.py`, `diffsnap.py` | firmware / U-Boot analysis scripts |
| `rtos/`, `apps/rtostest/` | FreeRTOS on the boxes: our MIPS 24KEc port, QEMU test, box test app |
| `bench/` | dspbench: SF2 / OPL / MP3 / DOOM hot loops, plain vs `-mdsp`, checked against the committed code |
| `sdk/brimod.h`, `sdk/brimod.c`, `esp32c3/bridge/bridge.ino`, `apps/brimod/` | BriMod: ESP32-C3 bridge on the satellite box's front I2C bus (panel, front buttons as IR frames, clock, climate, FM + RDS, WiFi), its SDK driver and settings app |
| `iptv/firmware/` (`backup.bin`, `app_ram.bin`, `uboot_part.bin`, `avcpu.bin`) | flash dump and extracted images |
| `BOXES.md` | which file works on which box (IPTV / satellite) |
| `continue.md` | full notes |

## Next ideas

- Store the app in flash (free space at `0x700000..0x7affff`, or wipe the
  stock firmware at `0x300000`) so no USB stick is needed.
- CH340 USB-serial driver in U-Boot's USB stack, then an MCU sending
  `DrawReplay` blobs to the box.
- Full 1920x1080 OSD plane (the output is 1080p now; the OSD is still
  1280x720 scaled up).
- libgfx vsync for 1080p timing.

## License

This project is **GPL-2.0-or-later** (full text in `LICENSE`): you may use it
under version 2 of the GNU GPL or, at your option, any later version.

Parts under other licenses:

- `rtl8188.h`, `rtl8188_tables.h`, `wlan.h`: **GPL-2.0-only**, derived from
  the Linux `rtl8xxxu` driver (Jes Sorensen, Bitterblue Smith, Realtek).
  Programs built with them (the WiFi programs) are therefore GPL-2.0-only.
- `softfp/`: LLVM compiler-rt builtins, Apache-2.0 WITH LLVM-exception
  (`softfp/LICENSE.TXT`; the exception allows combining with GPL-2.0).
- Not in this repository, fetched or supplied separately: Helix MP3 decoder
  (`helix/`, RPSL/RCSL), doomgeneric and Chocolate Doom (GPL-2.0-or-later),
  RTL8188F firmware (`rtl8188fufw.bin`, linux-firmware), game data (WADs),
  and anything taken from the stock firmware or flash of the box.
