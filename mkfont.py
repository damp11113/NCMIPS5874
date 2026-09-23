# Render DejaVu Sans Mono Bold (free license) into an 8x16 1-bit bitmap font
# for ASCII 32..126 and write it as a C header.
# Usage (in WSL): python3 mkfont.py > font8x16.h
from PIL import Image, ImageDraw, ImageFont

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'
W, H = 8, 16
font = ImageFont.truetype(FONT, 13)

print('/* 8x16 bitmap font, ASCII 32..126, rendered from DejaVu Sans Mono Bold')
print(' * by mkfont.py. Each glyph = 16 bytes, one per row, bit 7 = leftmost. */')
print('#ifndef FONT8X16_H')
print('#define FONT8X16_H')
print('')
print('static const unsigned char font8x16[95][16] = {')
for code in range(32, 127):
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    ch = chr(code)
    left, top, right, bottom = d.textbbox((0, 0), ch, font=font)
    x = (W - (right - left)) // 2 - left
    d.text((x, -2), ch, fill=255, font=font)
    rows = []
    for y in range(H):
        b = 0
        for xx in range(W):
            if img.getpixel((xx, y)) >= 110:
                b |= 0x80 >> xx
        rows.append(b)
    shown = ch if ch not in '\\*/' else ' '
    print('    { %s }, /* %r */' % (', '.join('0x%02x' % r for r in rows), shown))
print('};')
print('')
print('#endif')
