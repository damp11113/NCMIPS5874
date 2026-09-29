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
- Firmware model string `Model_02_cs8001b_64m_internet` (source path `CS8001B_<vendor>_64M_internet`).
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
- 2026-09-30: scope on SDA/SCL (clean 3.3 V, pull-ups on the box): prescaler 0x200 = 10.7 kHz SCL,
  so channel 2's input clock is 27 MHz (SCL = 27 MHz / (5 * (pre + 1))). At that speed one key read
  busy-waited ~1.9 ms every 30 ms (~6 % CPU in the idle launcher). Now `FD650_PRESCALE_100K` = 0x35
  (100 kHz, the stock speed) in the SDK, irpanel and fptest (built, not yet run on HW).
- 2026-09-30, **the front bus can't be shared** (adstest.c, i2cscan example, manual register pokes):
  with the panel connected every address ACKs (status after the address byte 0x44, bit 7 never set)
  and every read from another address fails with status bit 5 (0x6c); only the FD650's own key read
  (0x27) works. Same with the ADS1115 removed, so the FD650 causes it: it ACKs every byte, including
  bytes the master reads, overriding the master's NACK. Panel unplugged: clean NACKs (status 0x84) at
  100 kHz; at 400 kHz bus errors, so the pull-ups are probably on the panel board. The stock driver
  checks the same bits (channel table 0x806f7ef8: pointer 4 = +0x04 status, 6 = +0x14 command).
  i2c.h now takes the speed in kHz from the 27 MHz clock (`i2c_prescale`), plus `i2c_write_read`
  (repeated START); SDK `sdk_i2c_speed/write/read/write_read`, panel keeps 100 kHz. Plan: an ESP32
  (NodeMCU32) as the only slave on the box bus, FD650 on the ESP32's software I2C, sensors on its
  hardware I2C (wiring page "ESP32 Bridge Wiring").
- 2026-09-30: the board is an **ESP32-C3 Super Mini** (one hardware I2C = slave 0x42 for the box on
  GPIO8/9 (strapping pins: box-link pull-ups keep GPIO9 high at boot; an ESP32 reset while the box is
  off can leave it in download mode); panel FD650 on software I2C GPIO6/7, sensors on software I2C
  GPIO4/5, DS1302 module RST/CLK/DAT on 21/10/20 (bridge.ino; the wiring page still shows
  10/21/20), no ADS1115,
  SI4713 RST GPIO2 (+10k pull-up), IR doorbell GPIO3 via Schottky). Firmware `esp32c3/bridge/bridge.ino`
  (register map + mailbox commands in its header; front buttons -> NEC frames user 0xA55A with the
  FD650 key code, events user 0xA55B). Compiles with arduino-cli + esp32 core 3.3.10
  (fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc), not yet flashed / run.
- 2026-09-30, **BriMod** (box with the bridge) SDK driver `sdk/brimod.h` / `sdk/brimod.c`, linked
  into every SDK app (sdk/build.sh, doom/build.sh). Detection in `_start`: FD650 init as before, then
  only if the FD650 key read fails (no panel chip on the bus) read "NCB1" + version from 0x42 ->
  `sdk_brimod = 1`. A box with the panel wired as shipped never gets a transfer to 0x42, and the FD650
  code paths (fp_poll, fd650_show/led) are unchanged; with BriMod `sdk_panel_show/led` and
  `led_green` write the bridge's digit registers 0xA0-A3 (same fd650_char mapping, one transfer),
  sdk_key_poll maps IR user 0xA55A (FD650 key codes) to BTN_* with the remote's 400 ms repeat delay
  and collects user 0xA55B doorbell bits (`brimod_doorbell ()`), no panel polling. API: raw
  `brimod_read/write`, present / config / events / bell mask, clock (`brimod_time` with local date,
  set time / tz), climate, ADC, FM + RDS, panel raw / brightness, mailbox (`brimod_cmd` blocking,
  or start / status / reply). The bus prescaler is shared with the app's `sdk_i2c_*` speed
  (`sdk_i2c_use_prescale`). Read = request {0xFF, reg, n}, 300 us, read n; a failed read is
  followed by a 1-byte read so the stale reply leaves the C3's TX FIFO (the core flushes it at the
  STOP of a read). The C3 stretches SCL after a read address until its slave task runs.
  Bridge changes with it: held front buttons resend the full NEC frame every 110 ms (the box's
  decoder is only verified with full frames + one repeat code), NEC timing by RMT (open drain kept
  via `gpio_od_enable`, one high symbol at init because the channel idles low), reply offset 0xC8
  moves on after each read, the WIFI command is not logged (password).
- 2026-09-30, BriMod settings app `apps/brimod/` = NCAPPS/APPS/BRIMOD: menus Status / WiFi (scan
  list, on-screen keyboard for the password, masked except the last typed character, INFO shows it;
  hidden network by name; forget) / FM transmitter / Clock (time zone, NTP, set by hand) / restart
  the bridge. Keyboard: arrows + OK, digits type directly, RED del, GREEN done, YELLOW shift, BLUE
  space; serial PC keyboard types too. FM page = every SI4713 setting: on/off, frequency (typed or
  0.05 MHz steps), power, antenna cap (auto / manual), stereo / mono, pre-emphasis 50 / 75 / off,
  audio / pilot / RDS deviation (sum shown, warn > 75 kHz), line input range + full scale, mute,
  limiter + release, compressor on / threshold / gain / attack / release, RDS on, PS, RadioText, PI,
  PTY (names), music / speech, TP, TA, AF, clock time (CT), PS share, pilot frequency, noise scan
  (pick one of the 5 quietest), defaults; live noise / antenna / input level meter with
  overmodulation. Bridge side for it: FM flags bit2 mono (pilot + L-R off, RDS DI stereo bit
  follows), 0x44 antenna cap, 0x45 RDS options (bit0 CT: group 4A through the RDS FIFO at each
  minute, FIFO size 4 blocks only while CT is on), 0x30-0x33 noise / antenna in use / ASQ flags /
  input dBFS (TX_ASQ_STATUS 4x a second while on air, overmod held 1 s), property window 0x90
  (table of 16 settable properties with defaults, set / select + read), FM regs + RDS + properties
  saved in NVS 3 s after the last change and applied at boot (the transmitter comes back after
  power-off), `FMDEFAULTS`, `WIFI<tab>ssid<tab>password` (spaces), `WIFI?` = "CONNECTED ip rssi
  dBm ssid" / "DISCONNECTED saved-ssid", clearer connect errors, RadioText A/B flag flips when the
  text changes. SDK: `brimod_fm_write`, `brimod_si_get/set` + `BRIMOD_SI_*`, `brimod_rds_get`,
  `brimod_fm_defaults`, `brimod_fm_scan`, `brimod_wifi_status/scan/connect/forget`,
  `brimod_ntp_sync`, `brimod_make_time`. Built + staged, not yet run on HW (bridge not flashed).
  Unverified assumptions: SI4713 RDS buffer big enough for 16 RT groups + 4 FIFO blocks; the
  box's I2C master waits while the C3 stretches SCL.
- Panel 650G11B: 3 digits + green LED D1 on the FD650's 4 digit registers. Segment wiring is
  not standard: standard bit a b c d e f g dot -> FD650 bit 6 0 2 4 5 7 1 3 (stock font table
  at 0x8076e6ac, character + segments pairs, `fd650_map ()`).
- Layout: digits left to right = regs 2, 3, 1; reg 0 drives nothing. **LED = reg 1 bit 3** (the right
  digit's dot; fptest walk 2026-09-26). The earlier guess "reg 0 bit 1" was wrong: run 1 had reg 1 =
  0x5b (bit 3 set, LED on), run 2 reg 1 = 0x73 (LED off). fd650.h keeps a copy of the registers so
  the digit and the LED do not overwrite each other.
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

## Front panel in NCAPPS (2026-09-26, built, NOT staged / run yet)

- SDK: `sdk_key_poll` reads the front keys every 30 ms on the satellite box (CH+/CH- = UP/DOWN,
  VOL-/VOL+ = LEFT/RIGHT, OK, MENU; auto-repeat after 400 ms, every 120 ms). `sdk_panel_show`,
  `sdk_panel_led` (no-ops on the IPTV box). `fd650_char` now has the stock font's letters
  (A b C c d E F H h L n N O o P r S t U -).
- Launcher: LED on; display "---" at start and while an app starts, the selected entry number in
  the menu, "SEt" on the settings page, "Err" on the crash screen, blank in standby. Settings page
  hides "Big memory" on the satellite box (SETTINGS.TXT bigmem kept for the IPTV box); front MENU
  also wakes from soft standby. About page names the right SoC per box.
- MIDI / music players (built, not staged): front display shows active voices (MIDI) or the
  playing time (music: "123" = 1:23, from 10 min "12-"), "PAU" paused, "U80" / "120" volume for
  1.5 s after LEFT/RIGHT; updated every 200 ms, also with the screen saver on; "---" when leaving
  the player. `sdk_panel_show` only writes when the text changes.
- App config manager (SDK): `sdk_config_load / get / get_int / set / set_int / save`, file
  NCAPPS/APPSDATA/<app>/CONFIG.TXT (512 B, magic "# NCAPPS app config", key=value lines, max 16
  keys), overwritten in place like SETTINGS.TXT; stage.sh creates it per app only when missing.
- Front LED: launcher menu off; standby = 3 blinks then off; crash screen fast blink with "Err".
  MIDI: MENU cycles blink per quarter note / whole note (4 beats) / second (display "b 4" / "b 1" /
  "SEc", saved as led=quarter|whole|second), beat position from `smf_position_beats_q16` (follows
  tempo changes); paused = steady on. Music: blink per second (half on), paused steady; time from
  10 min alternates "12-" / "-34" every second. `sdk_panel_led` / `sdk_panel_show` write only on
  change; the launcher calls `sdk_panel_invalidate` after each app. (Built + staged, not run yet.)

## WARNING: U-Boot "flash window" at 0x80000000 (both boxes, 2026-09-26)

- Vendor U-Boot registers the 8 MB SPI flash as a dataflash bank starting at 0x80000000
  (`addr_dataflash` 0x8011e8d0 in the satellite build: flash_info start <= addr <= start + size - 1).
  `loady` (and U-Boot's `cp`, same check) to 0x80000000-0x807fffff ERASES AND PROGRAMS THE FLASH
  (log: `erase start_from[0x8000]to[0x9000] sec_cnt[0x1]`, `flash_wr_pio`) instead of writing RAM.
- Happened once on the satellite box: `loady 0x80008000` with cpuinfo.bin (3728 B) printed the
  erase/write messages and did not load RAM (go hung). Checked afterwards with sf read + crc32 in
  64 KB / 4 KB pieces against flash_s2.bin: the SPI flash is unchanged except expected stock
  records (0x120000, 0x5a2000-0x5a3000, 0x5b0000 / 0x5d0000 settings copies) and our boot.scr
  at 0xa0000; flash 0x8000 is still 0xff and cpuinfo is nowhere in the flash. So the vendor loady
  writes some other device (unknown), not the SPI flash - still never use the window.
  Note: this chip's `sf erase` works in 64 KB blocks (0x0-0x10000 = bootinit).
- A second try with `loady 0x81800000` printed `erase start_from[0x1800000]` (past the 8 MB chip):
  this loady NEVER loads RAM, at any address. fatload / loadimg / mw are fine.
- Serial uploads therefore use `serload.bin` (0x81f00000, built by buildall.sh): SSerHial writes it
  with `mw.l` to the uncached alias 0xa1f00000 (249 words, checked with crc32, skipped when already
  there), runs `go 0x81f00000 <dest> <size>`, sends the raw bytes (read with U-Boot getc into the
  uncached alias, D-cache written back first, I-cache invalidated after) and compares the CRC32.
  Works for any RAM destination incl. 0x80008000.
