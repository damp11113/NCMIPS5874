# Find code that references a string, then list MMIO addresses near that code.
import struct, sys, re
d = open('iptv/firmware/app_ram.bin', 'rb').read()
BASE = 0x80008000
N = len(d) // 4
W = struct.unpack_from('<%dI' % N, d)

def mmio_near(idx, span=120):
    out = []
    for i in range(max(0, idx - span), min(N - 1, idx + span)):
        w = W[i]
        if w >> 26 == 0xf and (w >> 21) & 31 == 0 and 0xbf00 <= (w & 0xffff) <= 0xbfff:
            rt = (w >> 16) & 31
            for j in range(1, 8):
                v = W[i + j] if i + j < N else 0
                op = v >> 26
                if (v >> 21) & 31 == rt and op in (0x09, 0x0d, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2b):
                    off = v & 0xffff
                    a = ((w & 0xffff) << 16) | off if op == 0x0d else (((w & 0xffff) << 16) + (off - 0x10000 if off & 0x8000 else off)) & 0xffffffff
                    out.append((BASE + 4 * i, a))
                    break
    return out

def str_refs(addr):
    hi = (addr + 0x8000) >> 16
    lo = addr & 0xffff
    refs = []
    for i in range(N - 8):
        w = W[i]
        if w >> 26 == 0xf and (w & 0xffff) == hi:
            rt = (w >> 16) & 31
            for j in range(1, 10):
                v = W[i + j]
                if v >> 26 == 0x09 and (v >> 21) & 31 == rt and (v & 0xffff) == lo:
                    refs.append(i)
                    break
    return refs

for pat in sys.argv[1:]:
    for m in re.finditer(re.escape(pat.encode()), d):
        s = m.start()
        while s > 0 and d[s - 1] != 0:
            s -= 1
        text = d[s:d.index(b'\0', s)].decode('latin1')
        for i in str_refs(BASE + s):
            near = sorted(set(a for _, a in mmio_near(i)))
            print('%-40.40r code@%08x mmio: %s' % (text, BASE + 4 * i, ' '.join(hex(a) for a in near[:12])))
