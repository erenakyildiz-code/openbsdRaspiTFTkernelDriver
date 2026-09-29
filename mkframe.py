import numpy as np
from PIL import Image
import sys

W, H = 320, 480

img = Image.open(sys.argv[1]).convert("RGB").resize((W, H), Image.LANCZOS)
a = np.asarray(img, dtype=np.uint32)

# a is an H x W x 3 array of uint8 (RGB888)
red   = a[..., 0]
green = a[..., 1]
blue  = a[..., 2]

# red: keep top 5 bits, then shift into bits 15..11
red_565   = (red   & 0b11111000) << 8

# green: keep top 6 bits, then shift into bits 10..5
green_565 = (green & 0b11111100) << 3

# blue: keep top 5 bits, place in bits 4..0 (no shift needed)
blue_565  = (blue  & 0b11111000) >> 3

# combine into one 16-bit pixel value
px = red_565 | green_565 | blue_565

# convert to big-endian uint16 and serialize to raw bytes
data = px.astype(">u2").tobytes()

# sanity check: 2 bytes per pixel
assert len(data) == W * H * 2


final = b""

# top half: even rows 0, 2, 4, ... 478
row = 0
while row < 480:
    final += data[row * 640: (row + 1) * 640]
    row += 2

# bottom half: the same even rows again
row = 0
while row < 480:
    final += data[row * 640: (row + 1) * 640]
    row += 2

print(len(final))   # 307200
open(sys.argv[2], "wb").write(final)

print(data[150000],data[1])
print(len(data), "bytes")
