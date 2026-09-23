# Map U-Boot's tiny register-field setter/getter functions:
#   lui v0,0xbfXX ; ori v0,v0,OFF ; ... lw a1,0(v0) ; ins a1,a0,POS,SIZE  (setter)
#   lui v0,0xbfXX ; ori v0,v0,OFF ; ... lw v0,0(v0) ; ext v0,v0,POS,SIZE  (getter)
# Prints function address, GOT offset(s) that point to it, register, bit field.
import struct, sys, collections
d = open('uboot_part.bin', 'rb').read()
BASE, GP = 0x800ffff0, 0x90ae0
N = len(d) // 4
W = struct.unpack_from('<%dI' % N, d)

got = collections.defaultdict(list)
for gi in range(0x88af0 // 4, 0x88af0 // 4 + 4000):
    got[W[gi]].append(gi * 4 - GP)

want = sys.argv[1] if len(sys.argv) > 1 else 'bf44'
rows = []
for i in range(N - 12):
    w = W[i]
    if w >> 26 == 0xf and (w >> 21) & 31 == 0 and '%04x' % (w & 0xffff) == want:
        rt = (w >> 16) & 31
        v = W[i + 1]
        if v >> 26 != 0x0d or (v >> 21) & 31 != rt:
            continue
        reg = ((w & 0xffff) << 16) | (v & 0xffff)
        for j in range(2, 9):
            x = W[i + j]
            op, fn = x >> 26, x & 0x3f
            if op == 0x1f and fn in (0x04, 0x00):   # ins / ext
                pos = (x >> 6) & 31
                msb = (x >> 11) & 31
                size = msb - pos + 1 if fn == 0x04 else msb + 1
                kind = 'set' if fn == 0x04 else 'get'
                # function start = 3 instrs before (gp prologue) when present
                start = i - 3 if (W[i - 3] >> 16) == 0x3c1c else i
                fa = BASE + 4 * start
                rows.append((reg, pos, size, kind, fa, got.get(fa, [])))
                break
for r in sorted(rows):
    print('%08x [%2d+%2d] %s  fn %08x  got %s' % (r[0], r[1], r[2], r[3], r[4], r[5]))
