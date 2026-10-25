#!/usr/bin/env python3
"""Contact sheet of frame dumps: sheet.py OUT.png COLS FILE... (PPM/PNG), each scaled to 320 wide
with its file name's frame number written under it."""
import sys
from PIL import Image, ImageDraw
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
W = 320
tiles = []
for f in files:
    im = Image.open(f).convert('RGB')
    h = im.height * W // im.width
    im = im.resize((W, h))
    t = Image.new('RGB', (W, h + 14), (40, 40, 40))
    t.paste(im, (0, 0))
    ImageDraw.Draw(t).text((4, h + 1), f.split('/')[-1].split('.')[0], fill=(255, 255, 0))
    tiles.append(t)
rows = (len(tiles) + cols - 1) // cols
th = max(t.height for t in tiles)
sheet = Image.new('RGB', (cols * W, rows * th), (0, 0, 0))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % cols) * W, (i // cols) * th))
sheet.save(out)
