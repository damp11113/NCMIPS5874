# Bare-metal apps on a Nationalchip 5874 set-top box

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
| HDMI graphics without the stock firmware | OSD layer, 1280x720 ARGB1555 scaled to 1080i50 |
| `libgfx` (`LinuxGFX` / `DrawReplay`) | new `GFX_NC5874` back-end, soft-float, dirty-rect + vsync |
| Boot straight into my app | flash boot script: `tvapp.bin` from USB, else stock firmware |
| Boot ad removed | ad sector erased; stock firmware shows its fallback splash |

Not done / parked: 1080p output (clock and HDMI TX solved, display mixer
stays interlaced — see `continue.md`), IR remote, USB serial (CH340) driver,
app stored in flash.

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
| `0x0A0000` | boot script (uImage script) — **replaced by `autoboot.scr`** |
| `0x0B0000` | avcpu firmware (LZMA) |
| `0x130000` | 1280x720 PNG |
| `0x300000` | stock main firmware (LZMA, runs at `0x80008000`) |
| `0x700000` | boot ad MP4 — **first 64 KB erased** |
| `0x7B0000..` | other firmware data (untouched) |

A full dump is in `backup.bin` (keep a copy on the USB stick).

## Boot flow (current flash)

`autoboot.scr` in the boot-script sector:

1. starts the AV core and HDMI through U-Boot's own driver (`av_launch`),
2. `usb start`; if the stick has **`tvapp.bin`**, runs it,
3. otherwise (or when the app returns) boots the stock firmware.

Press a key during the countdown to get the `U-boot#` prompt.

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
source 0x80100000                  # AV core + HDMI on (not needed with autoboot)
fatload usb 0 ${a} myapp.bin
go ${a}
```
To make an app start at power-on, name it `tvapp.bin` on the stick.

### Programming notes

- Calls into U-Boot must go through `$t9` (U-Boot is PIC) — `exports.S` does it.
- `start.c` clears `.bss` and runs C++ global constructors; destructors never run.
- `libc.c` has `memcpy`, `memset`, `strlen`, `strcmp`, `parse_hex`, ...
- Use integers where you can: floats are software-emulated.
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
vertical stripes (overlapping spans blended twice).

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
| `0xbf440100..0xbf44012c` | OSD scaler (U-Boot 1080i mode: 16.16 ratios) |
| `0xbf4400b8` | display output size per field (h<<16 \| w) |
| `0xbf47008c` | scan position in pixel clocks (0..2969999 per 1080i frame) |
| `0xbf480000 + reg` | HDMI transmitter byte registers (bank 1 at `+0x100`) |
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
| `avstart.txt`, `autoboot.txt`, `mkscript.py` | U-Boot scripts (`.txt` -> `.scr`) |
| `hook*.c`, `hook_entry.S`, `hookpatch.c`, `buildhook.sh` | run code inside the stock firmware |
| `regwatch.c`, `regdump.c`, `snap*.c`, `regapply.c`, `vicset.c`, `vsyncprobe.c`, `irscan.c` | hardware exploration tools |
| `xref.py`, `ubxref.py`, `accessors.py`, `diffsnap.py` | firmware / U-Boot analysis scripts |
| `backup.bin`, `app_ram.bin`, `uboot_part.bin`, `avcpu.bin` | flash dump and extracted images |
| `continue.md` | full notes |

## Next ideas

- Store the app in flash (free space at `0x700000..0x7affff`, or wipe the
  stock firmware at `0x300000`) so no USB stick is needed.
- CH340 USB-serial driver in U-Boot's USB stack, then an MCU sending
  `DrawReplay` blobs to the box.
- IR remote (receiver not on the GPIO banks; decoder probably unclocked
  until the stock firmware sets it up).
- Full 1920x1080 OSD on the 1080i output.
