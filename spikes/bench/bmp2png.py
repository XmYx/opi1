"""Convert an uncompressed 24/32-bit BMP to PNG with the standard library only. usage: bmp2png.py in.bmp out.png"""
import struct, sys, zlib
data = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', data, 10)[0]
w, h = struct.unpack_from('<ii', data, 18)
bpp = struct.unpack_from('<H', data, 28)[0]
bottom_up = h > 0; h = abs(h)
stride = ((w * bpp // 8) + 3) & ~3
rows = []
for y in range(h):
    sy = h - 1 - y if bottom_up else y
    row = data[off + sy * stride: off + sy * stride + w * bpp // 8]
    px = bytearray()
    step = bpp // 8
    for x in range(w):
        b, g, r = row[x * step], row[x * step + 1], row[x * step + 2]
        px += bytes((r, g, b))
    rows.append(b'\x00' + bytes(px))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
print(sys.argv[2], w, h, bpp)
