# Diff two snap logs word by word: prints address, run2 (u-boot) value, run1 (firmware) value.
import sys, re

def load(path):
    regs = {}
    for line in open(path, encoding='latin1'):
        m = re.match(r'\s*S ([0-9a-f]{8}): ([0-9a-f ]+)', line)
        if m:
            base = int(m.group(1), 16)
            for k, w in enumerate(m.group(2).split()[:4]):
                regs[base + 4 * k] = int(w, 16)
    return regs

fw, ub = load(sys.argv[1]), load(sys.argv[2])
blk = None
for a in sorted(fw):
    if a in ub and fw[a] != ub[a]:
        if a >> 12 != blk:
            blk = a >> 12
            print('--- block %05x000' % blk)
        print('%08x  uboot %08x  fw %08x  xor %08x' % (a, ub[a], fw[a], ub[a] ^ fw[a]))
