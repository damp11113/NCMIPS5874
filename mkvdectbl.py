# Extract the video decoder table the stock firmware builds at run time
# (0x1b00 bytes at 0x806ad08c) from a RAM dump of the running stock firmware
# (app_ram.bin, loaded at 0x80008000) into VDECTBL.BIN for vdectest.
# The table stays on your own stick; it is not part of the project.
# Usage: python mkvdectbl.py app_ram.bin VDECTBL.BIN
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
off = 0x806ad08c - 0x80008000
t = d[off:off + 0x1b00]
if struct.unpack_from('<2I', t) != (0x10000000, 0x0d11c3c0):
    sys.exit('table not found at 0x806ad08c in %s' % src)
open(dst, 'wb').write(t)
print('wrote %s (%d bytes)' % (dst, len(t)))
