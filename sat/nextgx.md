# Next box: <vendor> HD Lite Pro+ (satellite receiver)

> **Correction 2026-09-26 (box arrived):** the chip is NOT a GX6605S / C-SKY.
> It is a Montage **M88CS8002B**: MIPS 24KEc like the IPTV box, U-Boot 2012.04,
> 128 MB DDR2 (Hynix H5PS1G63EFR), 8 MB SPI flash (25VQ64), AiP650 front panel
> (3-digit 7-segment, 6 buttons). The C-SKY / Linux sections below are obsolete;
> the IPTV box's path applies (see `BOXES.md`). U-Boot layout from `printenv`
> (bootmedia=0): bootinit 0x0, U-Boot 0x10000, boot.scr 0xa0000.


Plan for the second set-top box, written 2026-09-25 while it was on its way.
The first box (NC5874 / Montage M88CS8051B, MIPS) stays the main platform;
see `continue.md` for everything done on it.

## The box

- **Model:** <vendor> HD Lite Pro+ satellite receiver (same brand as the first box, whose
  firmware talks to ota.<vendor>.tv).
- **Bought:** 2026-09-25, 502 baht, expected delivery 2026-09-26 .. 09-28.
- **Front:** 7-segment display (3 digits), 6 buttons (MENU, OK, CH+/CH-, VOL+/VOL-), power
  button, IR receiver. The first box had only 2 LEDs + 1 button.
- **Back:** satellite tuner input (LNB), HDMI, AV, USB.
- **Remote:** same layout as the first box's <vendor> remote, so probably the same NEC codes
  (user code 0xfe01, keys as in `remoteir.txt`, `IR_KEY_*` in `ir.h`). To verify: satellite
  remotes may add keys (EPG, FAV, SAT, audio) or use another user code.

## Expected chip: NationalChip GX6605S (to confirm)

Many boxes of this class use it. From C-SKY's development board page:

| | GX6605S | first box (NC5874) |
|---|---|---|
| CPU | C-SKY CK610M, ISA v1 (abiv1), MMU, 16 KB I + 16 KB D cache, no FPU | MIPS 24KEc ~648 MHz, 64-entry TLB, 16 + 16 KB cache, no FPU |
| RAM | 64 MB DDR2 inside the package | 128 MB (apps get ~52 MB, ~95 MB with big memory) |
| USB | EHCI + OHCI (USB 2.0 / 1.1) | EHCI (used through U-Boot) |
| Display | HDMI, 1280x720 framebuffer; media player decodes 1080p | HDMI, 1280x720 OSD (reverse engineered) |
| Flash | 4 MB SPI on the dev board (bootloader + media player) | 8 MB SPI |

Software support that exists:

- Linux mainline `arch/csky` supports abiv1 (CK610) and abiv2 (807/810/860):
  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/csky
- C-SKY buildroot releases (toolchain for abiv1 + kernel + rootfs):
  https://github.com/c-sky/buildroot/releases
- GX6605S development board (39 RMB) runs that Linux with the console on HDMI and USB
  working (hub, RTL8152B Ethernet, MT7601U WiFi, keyboard). Its 1080p video playback is the
  vendor media player in SPI flash, not Linux.
- Unknown under Linux: audio, IR, front panel (probably not in mainline).

## Decision: bare metal in the end, Linux only as a mapping tool

Linux would cost RAM and CPU on a 64 MB box, so the final software is bare metal, like the
first box: the NCAPPS SDK with a C-SKY backend, launcher and apps on top. Linux is booted
only during research, to find and record the hardware quickly, then dropped.

## Step 1: when it arrives (before powering on)

1. Photos of both sides of the board, readable chip markings: main SoC (under the heatsink),
   flash chip, front-panel controller, tuner / demodulator.
2. Look for the serial header (3-4 pins near the SoC, like J1 on the first box). 3.3 V
   USB-serial adapter.
3. Measure nothing on the tuner / LNB side while the box is on (LNB power is 13/18 V).

## Step 2: first power-on on the serial console

- Capture the complete boot log. It tells:
  - bootloader: **U-Boot** (then the first box's tricks work: `go`, jump table, USB storage
    driver, scripts) or **gxloader** (NationalChip's own; then our own USB driver and loading
    path are needed);
  - chip ID and CPU (confirms GX6605S / C-SKY, or something else);
  - RAM size, flash type, memory layout, boot arguments.
- Try to stop the boot (key during the countdown) and list the commands.

## Step 3: full flash backup (nothing is written before this)

- Dump the whole SPI flash (bootloader command, or a clip programmer on the SOIC-8 flash if
  the bootloader cannot). Keep two copies; verify by dumping twice and comparing.
- Satellite boxes may check signatures: no flash writes until the backup is verified and the
  boot chain is understood.

## Step 4: mapping with Linux (research only)

1. Build C-SKY's buildroot for the GX6605S dev board (gives the abiv1 GCC too).
2. Boot the kernel from RAM through the box's bootloader (serial / USB / TFTP, whatever it
   has), without touching flash. Expect device-tree changes first (RAM size, flash, pins);
   the boot log shows where it stops.
3. Read the map from the running system: `/proc/iomem`, `/proc/interrupts`,
   `/proc/device-tree`, `dmesg`.
4. Probe registers live with `devmem`; dump blocks before / after a driver acts (the
   snapshot-diff method from the first box, with real drivers doing the work).
5. Record init sequences: rebuild the kernel with logging in the register write helpers
   (address + value), then bring up display, USB etc.; the log is the sequence to replay in
   bare metal.
6. GPIO via `/sys/class/gpio`: toggle outputs (LEDs, 7-segment), watch inputs (buttons),
   like `irscan` / `regwatch` but interactive.
7. What Linux does not drive (audio, IR, front panel, tuner): dump and read the stock
   firmware, as on the first box.

## Step 5: bare-metal backend for the SDK

Needed pieces (C-SKY abiv1 GCC from buildroot, no kernel):

1. Start-up: stack, `.bss`, C-SKY exception vectors, cache setup.
2. Display: framebuffer init from the recorded sequence; the SDK's 1280x720 drawing on top.
3. Input: IR decoder (codes probably identical), front buttons, 7-segment output.
4. Audio: hardest part (probably no Linux driver to copy); stock firmware analysis.
5. USB + FAT: with U-Boot, borrow its storage driver as on the first box; with gxloader,
   our own EHCI driver (planned anyway: faster, non-blocking USB on the first box too).
6. Timer / time, reset, standby.

## Work on the first box that prepares this

- **Portable SDK:** move the remaining NC5874 details behind SDK interfaces
  (`sdk_display_open` with width / height / pitch / pixel format, `sdk_audio_*`,
  `sdk_time_ms` / `sdk_delay_us`, `sdk_power_button` / `sdk_led`), NC5874 code into
  `sdk/backend/nc5874/`. Apps then only need a recompile per backend.
- **PC simulator backend (SDL2, Windows / Linux):** window = TV, keyboard = remote, folder =
  USB stick. For testing apps before either box; never runs on the boxes.
- **Memory diet for 64 MB:** Doom reads WAD lumps on demand (`sdk_open` + seek) instead of
  loading whole WADs (saves 12-20 MB, faster start); MIDI loads only the SoundFont samples a
  song uses. Biggest app then ~15-20 MB.
- **Own EHCI driver** in portable C (see Step 5).

## What carries over unchanged

- Methods: boot log -> flash dump -> find display / input / audio -> record -> replay -> SDK.
- Code (recompile only): FAT reader logic, launcher, music / MIDI players, OPL and SoundFont
  synths, MP3 (Helix), stb_image, Doom, seek / overlay logic, remote key map.
- Needs a new version: anything touching MIPS CP0 (timers, crash vectors, TLB / MMU code),
  U-Boot calls, the NC5874 register headers.

## Open questions

- Is it really a GX6605S (or another chip)? U-Boot or gxloader? Signed boot?
- RAM actually free for apps after the video decoder's reservation.
- Where are the IR decoder, front-panel controller and audio blocks?
- Same remote codes as the first box?
