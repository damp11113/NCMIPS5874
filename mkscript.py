# Wrap a text file of U-Boot commands into a legacy uImage "script" image,
# the same format as the board's boot.scr, so U-Boot can run it with 'source'.
# Usage: python3 mkscript.py avstart.txt avstart.scr
import struct
import sys
import time
import zlib

src, dst = sys.argv[1], sys.argv[2]
text = open(src, 'rb').read().replace(b'\r\n', b'\n')
if not text.endswith(b'\n'):
    text += b'\n'

# Script payload: table of part lengths (big endian, 0 terminated), then the parts
payload = struct.pack('>II', len(text), 0) + text

IH_MAGIC = 0x27051956
IH_OS_LINUX, IH_ARCH_MIPS, IH_TYPE_SCRIPT, IH_COMP_NONE = 5, 5, 6, 0
name = b'Script'.ljust(32, b'\0')


def header(hcrc):
    return struct.pack('>7I4B32s', IH_MAGIC, hcrc, int(time.time()), len(payload),
                       0, 0, zlib.crc32(payload) & 0xffffffff,
                       IH_OS_LINUX, IH_ARCH_MIPS, IH_TYPE_SCRIPT, IH_COMP_NONE, name)


hdr = header(0)
hdr = header(zlib.crc32(hdr) & 0xffffffff)
open(dst, 'wb').write(hdr + payload)
print('wrote %s (%d bytes)' % (dst, len(hdr) + len(payload)))
