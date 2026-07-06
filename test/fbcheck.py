#!/usr/bin/env python3
# Decode the gridtest.html pattern from a captured /dev/fb0 dump.
#   blue gradient  -> scroll position (docY at content top)
#   green vlines   -> horizontal scale / width  (spacing >128 => rendered WIDER than it should be)
#   red hlines     -> vertical scale
#   blue backward-jumps -> tearing (a torn frame mixes two scroll positions)
import sys
from PIL import Image

W, H = 1024, 768
PAGESZ = W * H * 4
raw = open(sys.argv[1], "rb").read()

# pick the content page (most non-black sampled pixels)
best, bimg, bi = -1, None, 0
for p in range(3):
    buf = raw[p*PAGESZ:(p+1)*PAGESZ]
    if len(buf) < PAGESZ:
        break
    im = Image.frombytes("RGBA", (W, H), buf, "raw", "BGRA").convert("RGB")
    px = im.load()
    nz = sum(1 for y in range(0, H, 16) for x in range(0, W, 16) if sum(px[x, y]) > 30)
    if nz > best:
        best, bimg, bi = nz, im, p
img = bimg
px = img.load()
CT, CB = 100, H - 8          # content region (skip the ~96px URL bar)

def avg_blue(y0, y1):
    vals = [px[x, y][2] for y in range(y0, y1) for x in range(200, 824, 8)
            if px[x, y][0] < 60 and px[x, y][1] < 60]   # gradient pixels only (exclude red/green rules)
    return sum(vals) / len(vals) if vals else -1

def group(cols):
    out = []
    for c in cols:
        if out and c - out[-1][-1] <= 3:
            out[-1].append(c)
        else:
            out.append([c])
    return [sum(g)//len(g) for g in out]

btop = avg_blue(CT, CT + 20)
docY_top = btop/255.0*6400 if btop >= 0 else -1

# green vertical lines (width markers)
gcols = [x for x in range(W)
         if sum(1 for y in range(CT, CB, 4) if px[x, y][1] > 120 and px[x, y][0] < 100 and px[x, y][2] < 100) > (CB-CT)/4*0.4]
gl = group(gcols)
gsp = [gl[i+1]-gl[i] for i in range(len(gl)-1)]

# red horizontal lines (vertical scale)
rrows = [y for y in range(CT, CB)
         if sum(1 for x in range(0, W, 4) if px[x, y][0] > 120 and px[x, y][1] < 100 and px[x, y][2] < 100) > W/4*0.4]
rl = group(rrows)
rsp = [rl[i+1]-rl[i] for i in range(len(rl)-1)]

# tearing: blue must increase downward; count big backward jumps
prev, tears = -1, 0
for y in range(CT, CB, 3):
    b = avg_blue(y, y+1)
    if b >= 0 and prev >= 0 and b < prev - 12:
        tears += 1
    if b >= 0:
        prev = b

def med(a): return sorted(a)[len(a)//2] if a else 0
print(f"page={bi}  docY_top~{docY_top:.0f}px (blue={btop:.0f})")
print(f"WIDTH: green vlines={len(gl)} median_spacing={med(gsp)}px  (expect ~128; >140 => rendered TOO WIDE)  {gsp}")
print(f"VSCALE: red hlines={len(rl)} median_spacing={med(rsp)}px  (expect ~128)  {rsp}")
print(f"TEARING: blue backward-jumps={tears}  (0 = clean; >0 = torn/mixed frame)")
