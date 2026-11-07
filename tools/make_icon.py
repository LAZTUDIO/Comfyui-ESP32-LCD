#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成托盘/exe 图标 assistant/app.ico（配色与 WebUI 一致：深底 + 金环 + 青点）。

用法：python tools/make_icon.py
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assistant", "app.ico")


def draw(size):
    s = size * 4                      # 先画大图再缩，边缘干净
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = int(s * 0.03)
    d.rounded_rectangle([pad, pad, s - pad, s - pad], radius=int(s * 0.23),
                        fill=(17, 21, 29, 255), outline=(122, 96, 46, 255),
                        width=max(1, int(s * 0.035)))
    r = s * 0.36
    c = s / 2.0
    d.ellipse([c - r, c - r, c + r, c + r], outline=(232, 196, 118, 255),
              width=max(1, int(s * 0.05)))
    r2 = s * 0.15
    d.ellipse([c - r2, c - r2, c + r2, c + r2], fill=(69, 224, 255, 255))
    return img.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = [draw(n) for n in sizes]
    out = os.path.normpath(OUT)
    imgs[-1].save(out, format="ICO", sizes=[(n, n) for n in sizes])
    print("icon ->", out, os.path.getsize(out), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
