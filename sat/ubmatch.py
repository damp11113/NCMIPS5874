# Find IPTV U-Boot functions in another U-Boot build by instruction shape.
# Immediates, jump targets and GOT offsets are masked; opcode + registers must
# match. Usage: python sat/ubmatch.py <other_uboot.bin> [link_addr ...]
# Both images start at link 0x800ffff0 (16-byte header, code at 0x80100000).
import struct, sys

BASE = 0x800ffff0
REF = 'iptv/firmware/uboot_part.bin'
N = 40                           # instructions compared per function

def shape(w):
    op = w >> 26
    if op in (2, 3):             # j / jal: target differs
        return op << 26
    if op == 0 or op == 0x1c or op == 0x1f:    # R-type / SPECIAL2 / SPECIAL3
        return w
    return w & 0xffff0000        # I-type: opcode + rs + rt, immediate masked

def words(d):
    return [shape(x) for x in struct.unpack('<%dI' % (len(d) // 4), d[:len(d) // 4 * 4])]

ref = open(REF, 'rb').read()
oth = open(sys.argv[1], 'rb').read()
rw, ow = words(ref), words(oth)

NAMES = {
    0x80122a78: 'usb_get_dev_index', 0x80122cb4: 'usb_bulk_msg',
    0x80122d50: 'usb_control_msg', 0x8012484c: 'usb_stor_get_dev',
    0x80159c7c: 'ehci_submit_async timeout (li s0,10000)',
    0x8014876c: 'hdmi set_mode', 0x80104a64: 'reset helper',
}
addrs = [int(a, 16) for a in sys.argv[2:]] or sorted(NAMES)
for a in addrs:
    i = (a - BASE) // 4
    pat = rw[i:i + N]
    best = []
    for j in range(len(ow) - N):
        s = sum(1 for k in range(N) if ow[j + k] == pat[k])
        if s >= N // 2:
            best.append((s, j))
    best.sort(reverse = True)
    top = ', '.join('0x%08x (%d/%d)' % (BASE + j * 4, s, N) for s, j in best[:3])
    print('0x%08x %-40s -> %s' % (a, NAMES.get(a, ''), top or 'NOT FOUND'))
