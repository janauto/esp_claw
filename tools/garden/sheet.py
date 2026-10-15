#!/usr/bin/env python3
"""把一组截图拼成一张对照图（审截图用）。用法：python3 tools/garden/sheet.py 输出.png 截图1.png 截图2.png ... [--cols 4] [--scale 0.5]"""
import sys
from PIL import Image, ImageDraw

args = sys.argv[1:]
cols = int(args[args.index("--cols") + 1]) if "--cols" in args else 4
scale = float(args[args.index("--scale") + 1]) if "--scale" in args else 0.5
files = [a for i, a in enumerate(args[1:], 1) if not a.startswith("--") and not (i > 0 and args[i - 1] in ("--cols", "--scale"))]
ims = [Image.open(f).convert("RGB") for f in files]
w, h = int(ims[0].width * scale), int(ims[0].height * scale)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + 8) + 8, rows * (h + 26) + 8), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, (im, f) in enumerate(zip(ims, files)):
    x, y = 8 + (i % cols) * (w + 8), 8 + (i // cols) * (h + 26)
    sheet.paste(im.resize((w, h)), (x, y + 18))
    d.text((x, y + 2), f.split("/")[-1][:40], fill=(230, 230, 230))
sheet.save(args[0])
print(args[0], sheet.size)
