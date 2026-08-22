#!/usr/bin/env python3
"""Convert the background PNG into the raw RGB888 blob the v_mix background layer reads from DDR.

The design's background layer expects 1920x1080, 3 bytes/pixel (R,G,B), row-major (6,220,800 B),
loaded to 0xB4000000 by scripts/program.tcl. The blob is generated (gitignored) - regenerate it
from the committed PNG art whenever it's missing or the art changes.

  python3 tools/png_to_rgb888.py
      assets/scu200_ai_demo01_background.png -> assets/scu200_ai_demo01_background_rgb.bin

BICUBIC resampling reproduces the original demo blob byte-for-byte from the 1280x720 source art.
"""
import argparse, os
from PIL import Image

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument('--png', default=os.path.join(root, 'assets', 'scu200_ai_demo01_background.png'),
                    help='source PNG (any size; resized to WxH)')
    ap.add_argument('--out', default=os.path.join(root, 'assets', 'scu200_ai_demo01_background_rgb.bin'),
                    help='output raw RGB888 .bin')
    ap.add_argument('--width', type=int, default=1920)
    ap.add_argument('--height', type=int, default=1080)
    a = ap.parse_args()

    src = Image.open(a.png)
    im = src.convert('RGB').resize((a.width, a.height), Image.BICUBIC)
    data = im.tobytes()   # R,G,B per pixel, row-major
    with open(a.out, 'wb') as f:
        f.write(data)
    print(f"{a.png} {src.size} -> {a.out}  ({a.width}x{a.height} RGB888, {len(data)} bytes)")

if __name__ == '__main__':
    main()
