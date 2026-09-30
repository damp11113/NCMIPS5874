"""
spiflash.py: read / write the SPI NOR flash of the T2 box (ALi M3822 board)
with an FT232H in MPSSE SPI mode (pyftdi).

Wiring, FT232H -> flash (SOIC-8, pin 1 = the corner with the dot; 1-4 down
one side, 5-8 back up the other):
    AD0 (SCK)  -> pin 6  CLK
    AD1 (MOSI) -> pin 5  DI
    AD2 (MISO) -> pin 2  DO
    AD3 (CS)   -> pin 1  /CS
    GND        -> pin 4  GND
    3V3        -> pin 8  VCC   (the board itself stays unpowered)
Pins 3 (/WP) and 7 (/HOLD) stay as the board wires them. Keep the wires
short (< 15 cm); lower --freq if reads differ.

Windows, once: Zadig -> Options -> List All Devices -> "USB Serial
Converter" (0403:6014) -> WinUSB -> Replace Driver. (The FT232H is then no
COM port; to get that back, uninstall the device in Device Manager with
"delete the driver".)

    python t2/spiflash.py loopback                 FT232H self-test (AD1 wired to AD2, no board)
    python t2/spiflash.py id
    python t2/spiflash.py read t2/flash_t2.bin     reads twice, compares, prints MD5
    python t2/spiflash.py write file.bin           erase + program + verify (asks first)
    python t2/spiflash.py write part.bin --offset 0x10000
"""
import argparse
import hashlib
import sys
import time

from pyftdi.spi import SpiController

URL = 'ftdi://ftdi:232h/1'
SECTOR = 4096
PAGE = 256

VENDORS = {
    0xEF: 'Winbond', 0xC8: 'GigaDevice', 0x0B: 'XTX', 0x20: 'XMC / Micron', 0x5E: 'Zbit',
    0x68: 'Boya', 0x1C: 'EON', 0xC2: 'Macronix', 0x9D: 'ISSI', 0xA1: 'Fudan', 0x85: 'Puya',
    0xBF: 'SST', 0x1F: 'Adesto',
}


def open_flash(freq):
    ctrl = SpiController()
    try:
        ctrl.configure(URL)
    except Exception as e:
        sys.exit('FT232H not found (%s): plugged in? On Windows its driver must be WinUSB '
                 '(Zadig, see the top of this file)' % e)
    return ctrl, ctrl.get_port(cs=0, freq=freq, mode=0)


def jedec_id(port):
    return bytes(port.exchange([0x9F], 3))


def status(port):
    return port.exchange([0x05], 1)[0]


def wait_ready(port, timeout=5.0):
    t0 = time.time()
    while status(port) & 0x01:                  # WIP
        if time.time() - t0 > timeout:
            raise TimeoutError('flash stays busy')
        time.sleep(0.001)


def write_enable(port):
    port.exchange([0x06])


def chip_size(jid):
    if jid in (b'\x00\x00\x00', b'\xff\xff\xff'):
        return 0
    return 1 << jid[2] if 0x10 <= jid[2] <= 0x18 else 0


def describe(jid):
    size = chip_size(jid)
    vendor = VENDORS.get(jid[0], 'unknown vendor')
    return '%s (%s), %s' % (jid.hex(), vendor, '%d KB' % (size // 1024) if size else 'size unknown')


def read_range(port, addr, n, label):
    out = bytearray()
    chunk = 32 * 1024                           # pyftdi limit is just under 64 KB per transfer
    for a in range(addr, addr + n, chunk):
        m = min(chunk, addr + n - a)
        out += port.exchange([0x03, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF], m)
        print('\r  %s %3d %%' % (label, (a + m - addr) * 100 // n), end='', flush=True)
    print()
    return bytes(out)


def need_chip(port):
    jid = jedec_id(port)
    size = chip_size(jid)
    print('flash: %s, status 0x%02x' % (describe(jid), status(port)))
    if not size:
        sys.exit('no flash answer: check wiring, 3V3 on pin 8, GND; try --freq 100000')
    return size


def cmd_id(port, args):
    need_chip(port)


# FT232H alone, no board: a wire from AD1 to AD2 makes every byte sent come back
def cmd_loopback(port, args):
    out = bytes([0x55, 0xAA, 0x9F, 0x00, 0xFF, 0x3C])
    back = bytes(port.exchange(out, duplex=True))
    print('sent %s, got %s' % (out.hex(' '), back.hex(' ')))
    if back != out:
        sys.exit('LOOPBACK FAILED: check the wire AD1-AD2 and that the pins really are AD1 / AD2 '
                 '(some boards call them D1 / D2)')
    print('loopback ok: the FT232H, its driver and this script work')


def cmd_read(port, args):
    size = need_chip(port)
    t0 = time.time()
    a = read_range(port, 0, size, 'read 1')
    b = read_range(port, 0, size, 'read 2')
    with open(args.file, 'wb') as f:
        f.write(a)
    print('%d bytes in %.0f s, MD5 %s -> %s' % (size, time.time() - t0,
                                               hashlib.md5(a).hexdigest(), args.file))
    if a != b:
        diff = sum(1 for x, y in zip(a, b) if x != y)
        with open(args.file + '.2', 'wb') as f:
            f.write(b)
        sys.exit('READS DIFFER in %d bytes (second read in %s.2): lower --freq, shorter '
                 'wires; if it stays, the SoC is disturbing the bus' % (diff, args.file))
    if a.count(0xFF) == size or a.count(0) == size:
        sys.exit('the dump is all 0x%02x: not a real read' % a[0])
    print('both reads identical')


def cmd_write(port, args):
    size = need_chip(port)
    with open(args.file, 'rb') as f:
        data = f.read()
    off = args.offset
    if off % SECTOR or len(data) % SECTOR or off + len(data) > size:
        sys.exit('offset and length must be 4 KB multiples inside the %d KB chip' % (size // 1024))
    print('about to ERASE and WRITE 0x%06x-0x%06x (%d KB) from %s' %
          (off, off + len(data) - 1, len(data) // 1024, args.file))
    print('only do this with a verified dump of this chip saved elsewhere')
    if input('type YES to go on: ') != 'YES':
        sys.exit('nothing written')
    if status(port) & 0x1C:                     # block protection bits
        print('clearing block protection (status 0x%02x)' % status(port))
        write_enable(port)
        port.exchange([0x01, 0x00])
        wait_ready(port)
    cur = read_range(port, off, len(data), 'compare')
    done = 0
    for s in range(0, len(data), SECTOR):
        new, old = data[s:s + SECTOR], cur[s:s + SECTOR]
        if new != old:
            a = off + s
            write_enable(port)
            port.exchange([0x20, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF])
            wait_ready(port)
            for p in range(0, SECTOR, PAGE):
                page = new[p:p + PAGE]
                if page.count(0xFF) == PAGE:
                    continue
                pa = a + p
                write_enable(port)
                port.exchange(bytes([0x02, (pa >> 16) & 0xFF, (pa >> 8) & 0xFF, pa & 0xFF]) + page)
                wait_ready(port)
            done += 1
        print('\r  write %3d %%  (%d sectors changed)' % ((s + SECTOR) * 100 // len(data), done),
              end='', flush=True)
    print()
    if read_range(port, off, len(data), 'verify') != data:
        sys.exit('VERIFY FAILED: the chip does not hold the file; do not power the box, write again')
    print('written and verified, %d sectors changed' % done)


def main():
    ap = argparse.ArgumentParser(description='SPI flash via FT232H (T2 box)')
    ap.add_argument('--freq', type=int, default=1000000, help='SPI clock in Hz (default 1 MHz)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('id')
    sub.add_parser('loopback', help='FT232H self-test: wire AD1 to AD2, board disconnected')
    r = sub.add_parser('read')
    r.add_argument('file')
    w = sub.add_parser('write')
    w.add_argument('file')
    w.add_argument('--offset', type=lambda s: int(s, 0), default=0)
    args = ap.parse_args()
    ctrl, port = open_flash(args.freq)
    try:
        {'id': cmd_id, 'loopback': cmd_loopback, 'read': cmd_read,
         'write': cmd_write}[args.cmd](port, args)
    finally:
        ctrl.terminate()


if __name__ == '__main__':
    main()
