# Satellite box (M88CS8002B) notes

## Files (gitignored: private / vendor code)

| File | What |
|---|---|
| `flash_s2.bin` | full 8 MB SPI flash, md5 080b9cb902e23b573081014ea22b9b13 (read twice + cmp, read back from stick) |
| `ubsat.bin` | U-Boot image from RAM 0x800ffff0 (0x90000), same as flash 0x10000 |
| `app_s2.bin` | main firmware, LZMA from flash 0x300040 unpacked on the PC (8,351,768 B, runs at 0x80008000) |
| `avcpu_s2.bin` | AV core firmware, LZMA from flash 0xb0004 (674,212 B, runs at 0x83e10000) |
| `app_s2_strings.txt` | `strings` of app_s2.bin |
| `logs/b2uboot.txt` | printenv, bdinfo, cpuinfo run |

## Board facts

- U-Boot 2012.04 (May 09 2026 - 09:21:12), 38400 baud, `ddrsize=128`, `chipver=10`, `pinmux=1`.
- Firmware model string `Model_02_cs8001b_64m_internet` (source path `CS8001B_GMMZ_64M_internet`).
- CPU identical to the IPTV box (cpuinfo: 24KEc, Count 324 MHz, 430 MIPS); USB speeds identical
  (usbspeed: U-Boot 1.6 MB/s, own BOT 26.6 MB/s). Only the stick on the USB bus (no WiFi module).

## Flash layout (same scheme as the IPTV box)

| Offset | Content |
|---|---|
| 0x000000 | bootinit (magic bf9c7d5a) |
| 0x010000 | U-Boot (header c0bb0800 = 0x8bbc0 bytes) |
| 0x0a0000 | boot.scr (994 B) |
| 0x0b0000 | avcpu.bin (4-byte size + LZMA) up to 0x110000 |
| 0x110000-0x1e0000, 0x1f0000 | data (logo? to check) |
| 0x300000 | main firmware header (21436587, model string), LZMA at 0x300040, to ~0x5c0000 |
| 0x5c0000-0x630000 | zero / data blocks (settings?) |
| 0x630000-0x670000, 0x700000-0x730000, 0x7b0000-0x7d0000, 0x7e0000-0x800000 | data (channel DB / settings?) |

## Boot script (stock) - AV memory map is the 64 MB one

```
avenv_AVCPU_CONFIG 64M
VIDEO_FW_CFG  0xa21e0000 size 0x1c01000
AUDIO_FW_CFG  0x82050000 size 0x180000
VID_SD_WR_BACK 0xa1df0400 size 0x25f800, 6 fields
loadimg 1 lzma 0x300000 0x81500000 0x80008000     (main firmware)
loadimg 1 lzma 0xb0000 0x81500000 0x83e10000      (AV core)
av_launch 0x83e10000 ; cpu 1 release 0x83e10000 ; go 0x80008000
```
The stock firmware uses only the low 64 MB (AV core at 0x83e10000, video memory from
0x01df0400). The NCAPPS memory map (heap 0x81600000-0x837f0000, OSD 0x03a00000, audio
0x03800000) collides with this AV layout -> needs a satellite memory map. If all 128 MB work,
0x04000000-0x08000000 (64 MB) is free for apps: to test with a RAM test.

## Front panel (FD650 = AiP650 protocol) - from app_s2.bin

- Driver `_fd650_led_display` around 0x800848d8 (display / key layer), bus device `i2c_FP`.
- **Hardware I2C controller, channel 2, registers at 0xbf158000** (+0x04 .. +0x1c).
  Channel table at 0x806f7ef8 (28 bytes per channel, 7 register pointers):
  ch0 0xbf560000 (`i2c0`), ch1 0xbf570000 (`i2c1`), ch2 0xbf158000 (`i2c_FP`),
  ch3 0xbf5c0000 (`i2c_HDMI`), ch4 0xbf580000 (`i2c_QAM`). Buses set up at 0x80081100..:
  100 kHz, mode 2, channel byte at cfg +73.
- I2C driver (type 0x501, registered 0x8028c750): ops write 0x8028ccc0, read 0x8028cfb0,
  stop 0x8028c9b0; start code 0x8028c818 writes 0x40 to the channel's 7th register (+0x14).
- FD650 command word -> I2C: first byte `((cmd >> 7) & 0x3e) | 0x40`, second byte `cmd & 0xff`
  (write fn 0x80084710). Init (ioctl 1): pinmux `0xbf15b400` low byte = 0x33 (two pins to the
  panel function), then cmd 0x0441 = byte 0x48 data 0x41 (display on). Digits: cmds 0x1400 /
  0x1500 / 0x1600 / 0x1700 = bytes 0x68 / 0x6a / 0x6c / 0x6e. Keys: byte 0x4f then read 1 byte,
  polled by a 200 ms timer.
- Board pinmux at 0x80080e94: `0xbf13c000..c014` and `0xbf15b400/b404`, two layouts chosen by a
  board-type check (== 22).

## Front panel: verified on HW (fptest, 2026-09-26)

- Own driver (`i2c.h` + `fd650.h`) works: prescaler 0x200 on channel 2, display on, keys read.
- Panel 650G11B: 3 digits + green LED D1 on the FD650's 4 digit registers. Segment wiring is
  not standard: standard bit a b c d e f g dot -> FD650 bit 6 0 2 4 5 7 1 3 (stock font table
  at 0x8076e6ac, character + segments pairs, `fd650_map ()`).
- Layout (fptest run 2, panel font '1' '2' '3' '4' in regs 0-3 showed "3 4 2", LED off):
  digits left to right = regs 2, 3, 1; reg 0 = LED only. Run 1 lit the LED with reg 0 = 0x06,
  run 2 did not with 0x05 -> LED = reg 0 bit 1 (not yet walked). Dots are not wired.
  (Stock "LED" call sets bit 7 of reg 3 = a segment here: purpose unknown.)
  Helpers: `fd650_show ("123")`, `fd650_led (on)`. fptest run 3: "123" correct left to right.
- Keys (`sat/frontkey.txt`), bit 6 = pressed: MENU 0x05, OK 0x2d, VOL- 0x15, VOL+ 0x1d,
  CH- 0x25, CH+ 0x35.

## Next

1. Read the I2C register usage in the driver ops (0x8028c818..0x8028d100) -> own bare-metal
   I2C write/read for channel 2.
2. Test program: pinmux 0xbf15b400 = 0x33, display on, write "123", read keys.
3. `regdump` 0xbf158000 / 0xbf15b400 / 0xbf13c000 from U-Boot (before the stock firmware ran),
   compare with the values the stock firmware sets.

## IR remote: verified on HW (irpanel, 2026-09-26)

- Same IR block (0xbf151000) and `ir.h` init as the IPTV box; remote codes match `remoteir.txt`,
  user code 0xfe01. `ir.h` works unchanged. (IPTV tools irkeys / irtest / irscan use `board.h`
  GPIOs: do not run them here.)

## RAM: upper 64 MB verified (ramtest, 2026-09-26)

- Physical 0x04000000-0x07ffffff: not a mirror of the low half; data bus, address bus,
  address-in-address (+ inverse) and random fill all 0 errors (uncached). Random data checked
  afterwards with md.l (0xa4000000: 87985aa5 155b24a3 ...). -> 128 MB, ~64 MB free for apps
  with the stock 64M AV layout.
- ramtest (first build) printed "0 ms" pass time via get_timer; cause unknown. timertest: get_timer
  is fine (1000 ms per 324,000,000 Count ticks for 10 s, across a Count wrap, and during a busy
  loop), so SDK timing works on this box. ramtest now times with CP0 Count anyway.

## NCAPPS on the satellite box (2026-09-26, VERIFIED on HW via source from the stick)

- SDK detects the box from the U-Boot build (`ubaddr.h` `box`, `sdk_box_sat` in runtime.c).
  Satellite: heap 0x04670000-0x08000000 + 0x01600000-0x01de0000 (~66 MB), OSD 0x04400000 and
  audio 0x045d0000 unchanged (already in the free upper 64 MB), no bigmem. `sdk/box.h` redirects
  `standby_pressed` (always 0) and `led_green` (front panel LED) / `led_red` (none); board.h
  untouched (stock-firmware hooks include it). Watchdog reboot sequence same as the IPTV U-Boot.
- `sat/scripts/ncboot.txt` -> `ncboot.scr` (911 B; `satboot.scr` on the stick): usb stop, stock AV
  env (64M), AV core 0xb0000 -> 0x83e10000, usb start, launcher if on the stick, else stock
  firmware (0x300000 -> 0x80008000). No fwpatch, no SETTINGS import, no EXIT marker.
- Test with `source` from 0x81800000: 0x82100000 (IPTV habit) is inside this box's audio firmware
  area (0x02050000-0x021d0000) and the AV init could overwrite the running script.
- HW run: launcher on HDMI (MEM x/66M), audio works, DOOM runs (70 fps, CPU 53 % = game 30 /
  draw 15 / music 6, idle 46 % in E1M1) -> performance same as the IPTV box; the manual's
  "550 MHz" is most likely wrong (absolute clock only confirmed once timed against a stopwatch).
- IPTV box with the same stick: usbspeed prints "U-Boot build: IPTV box" (read fn 0x813a1638 vs
  0x813a1f20 on the satellite box), same speeds -> ubaddr.h detection verified on both boxes.
- IPTV box boots the new launcher from its flashed ncboot: MEM x/89M (big memory on), as before.
- **FLASHED 2026-09-26:** `satboot.scr` (= sat/scripts/ncboot.scr, 911 B) written to the boot.scr sector
  0xa0000 (sf write from 0x81800000, cmp.b 4096 bytes same). Power-on with the stick -> NCAPPS,
  without -> stock firmware (both verified). Undo: `usb start; fatload usb 0 0x82000000
  flash_s2.bin; sf probe 0; sf erase 0xa0000 0x10000; sf write 0x820a0000 0xa0000 0x10000`.
