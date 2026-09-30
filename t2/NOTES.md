# T2 box (ALi M3822) notes

Third box: DVB-T2 receiver sold as "SMART TV ANDROID DVB-T2 HD-001" (it is
not Android and not 4K). Arrived 2026-09-30.

## Board facts (photos 2026-09-30)

- Board `2+5 168-M3822-850 V11 20260611`.
- SoC **ALi M3822** (LQFP, under a glued heatsink; marking `ALi M3822 ...
  XT3K5900..`). Not Montage like the other two boxes, so no U-Boot is
  expected (ALi's own bootloader); unverified until the flash is read.
- RAM EtronTech EM68B16CWQH-25H: DDR2, 512 Mbit x16 = **64 MB** (the listing's
  "512MB" is wrong).
- Flash: SOIC-8 left of the SoC, most likely 4 MB (32 Mbit); marking only
  partly readable. `spiflash.py id` will tell.
- Power supply on the same board: mains flyback inside the isolation slot
  (AC in = white 2-pin connector), secondary about 5 V.
  **Powered without mains** by feeding 5 V from a phone charger into the USB
  port's VBUS / GND (the port's 5 V pin is the board's 5 V rail): boots to
  the DVB-T2 splash on HDMI.
- Front on the main board: 4-digit display (12-pin), buttons POWER, MENU,
  VOL+/-, CH+/-, OK, IR receiver.
- **No UART header or test pads**: all vias on the bottom are covered by
  solder mask. The UART is only on SoC pins. Lead to check (unconfirmed,
  from a web search, and for the M3821): TX pin 114, RX pin 115.
- Menu: Software Update has only "USB Upgrade" (no backup / dump seen); the
  62 GB FAT32 stick was not detected ("No USB Device Inset").

## Flash dump with an FT232H

`spiflash.py` (pyftdi, installed in the Windows Python). Pins on the real
chip: `flash_pinout.png`. Wiring and the
one-time Zadig step are at the top of the file. Board unpowered, the FT232H
feeds the flash:

```
python t2/spiflash.py id
python t2/spiflash.py read t2/flash_t2.bin
```

In-circuit reading does not work (2026-09-30, scope on the FT232H side):
- board unpowered, FT232H 3V3 on flash pin 8: the whole board's 3.3 V rail
  hangs on it, levels only ~1.7 V;
- board's 3.3 V from a bench supply (0.3 A at 3.3 V): SCK / MOSI still only
  ~2 V, full 3.3 V swing only with the wire to the board unplugged, so the
  unpowered-core ALi chip holds its SPI pins low;
- board running from 5 V: the ALi chip clocks the flash all the time (a
  burst every ~12 us), so it keeps the bus.
So the flash has to come off the board to be read (pins 3 and 7 to 3.3 V
when it is on its own).

`read` reads twice and compares. Dumps are gitignored (`*.bin`): they hold
the vendor's code. Never `write` before a verified dump is saved in two
places.

## Status: parked (2026-09-30)

A try to lift the flash with only a soldering iron, then resoldering, left
the board dead: it takes ~0.1 A at 5 V instead of ~0.5 A and shows nothing
(likely an open joint, a lifted pad or a knocked-off part at the flash; not
diagnosed). No dump was made. Project stopped; the board is kept as
soldering practice.

Later the same day the board was broken up (PCB snapped, RAM torn off): the
T2 box is gone. These notes stay for reference.
