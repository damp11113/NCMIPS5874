# U-Boot string xref: find code that loads a string via GOT page + addiu,
# then list 0xbfXXXXXX addresses nearby. Usage: python3 ubxref.py "text" ...
import struct, sys, re
d = open('iptv/firmware/uboot_part.bin', 'rb').read()
BASE = 0x800ffff0          # link address of file offset 0
GP = 0x90ae0               # gp as file offset (GOT start 0x88af0 + 0x7ff0)
N = len(d) // 4
W = struct.unpack_from('<%dI' % N, d)

def s16(x):
    return x - 0x10000 if x & 0x8000 else x

def mmio_near(i, span=80):
    out = set()
    for k in range(max(0, i - span), min(N - 8, i + span)):
        w = W[k]
        if w >> 26 == 0xf and (w >> 21) & 31 == 0 and 0xbf00 <= (w & 0xffff) <= 0xbfff:
            rt = (w >> 16) & 31
            for j in range(1, 8):
                v = W[k + j]
                op = v >> 26
                if (v >> 21) & 31 == rt and op in (0x09, 0x0d, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2b):
                    a = ((w & 0xffff) << 16) | (v & 0xffff) if op == 0x0d else (((w & 0xffff) << 16) + s16(v & 0xffff)) & 0xffffffff
                    out.add(a)
                    break
    return sorted(out)

refs = {}
for i in range(N - 6):
    w = W[i]
    if w >> 26 == 0x23 and (w >> 21) & 31 == 28:
        ge = (GP + s16(w & 0xffff)) // 4
        if 0 <= ge < N:
            rt = (w >> 16) & 31
            for j in range(1, 16):
                v = W[i + j]
                if v >> 26 == 0x09 and (v >> 21) & 31 == rt:
                    refs.setdefault((W[ge] + s16(v & 0xffff)) & 0xffffffff, []).append(i)
                    break
            # global GOT entry: pointer to the string itself
            refs.setdefault(W[ge], []).append(i)

for pat in sys.argv[1:]:
    for m in re.finditer(re.escape(pat.encode()), d):
        s = m.start()
        while d[s - 1] != 0:
            s -= 1
        for i in refs.get(BASE + s, []):
            print('%-30.30r code@%08x (file %05x) mmio: %s' % (pat, BASE + 4 * i, 4 * i,
                  ' '.join(hex(a) for a in mmio_near(i))))
