#!/usr/bin/env python3
"""Render theme contact sheets for the ComfyUI panel frameworks.

- single MONITOR page for all 10 themes
- dual DASH page for all 10 themes
- dual LOGS / OUTPUTS samples (theme 01)
Geometry mirrors the firmware (480x320 logical, 2x supersampled).
"""
import math
import pathlib
from PIL import Image, ImageDraw, ImageFont

S = 2
W, H = 480, 320
OUT = pathlib.Path(__file__).resolve().parent.parent / "docs" / "themes"
OUT.mkdir(parents=True, exist_ok=True)
CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"
MONO = "/System/Library/Fonts/Menlo.ttc"
THUMB = "/tmp/thumb.jpg"

THEMES = [
    ("Dark Glass",   0x0d1220, 0x111a2e, 0x18203a, 0x101828, 0x4f8cff, 0x2ecc71, 0xf5a623, 0xe5484d, 0xdfe7f5, 0x8494ad, 0x223154, 8, False),
    ("Minimal Light",0xf4f5f7, 0xffffff, 0xffffff, 0xf8f9fb, 0x151a22, 0x16a34a, 0xd97706, 0xdc2626, 0x151a22, 0x7b8496, 0xdfe3ea, 14, True),
    ("Cyber Neon",   0x08060f, 0x0c0818, 0x120c22, 0x180e2c, 0x00f0ff, 0xff2d95, 0xffd60a, 0xff3b30, 0xf5f0ff, 0x9682be, 0xff2d95, 2, False),
    ("Terminal",     0x020a04, 0x020a04, 0x021406, 0x021406, 0x39ff14, 0x39ff14, 0xa6ff00, 0xff4d4d, 0x39ff14, 0x1e8c28, 0x14501e, 0, False),
    ("Industrial",   0x1b1f24, 0x14181c, 0x262c33, 0x1e2328, 0xff9f1c, 0x33cccc, 0xff9f1c, 0xff5757, 0xe8edf2, 0x96a0aa, 0x3a424b, 2, False),
    ("Synthwave",    0x2b1055, 0x1a0c2c, 0x2e1656, 0x241244, 0xff4ecd, 0x00e5ff, 0xffd166, 0xff5c8a, 0xfff0ff, 0xbea0de, 0x6a3a9a, 4, False),
    ("Paper Ink",    0xf6f1e7, 0xf6f1e7, 0xfffcf6, 0xf9f4ea, 0xb23a2f, 0x3a6e48, 0xb26a00, 0xa51f1f, 0x2b2620, 0x7a6e5e, 0xd8cfc0, 3, True),
    ("Blueprint",    0x0a1f3c, 0x0a1f3c, 0x0c284c, 0x0e2f58, 0x5fd4ff, 0x78ffd6, 0xffd166, 0xff6b6b, 0xd7f2ff, 0x8cbedc, 0x285a8c, 0, False),
    ("Material You", 0xf3edf7, 0xf3edf7, 0xffffff, 0xe8def8, 0x6750a4, 0x388e3c, 0xb26a00, 0xb3261e, 0x1d1b20, 0x79747e, 0xe7e0ec, 16, True),
    ("Aurora",       0x071a1f, 0x0a2029, 0x0e2a33, 0x0c242d, 0x2de2c4, 0x2de2c4, 0xffb347, 0xff6b6b, 0xe6fbf6, 0x8cbeb9, 0x1e5055, 10, False),
]

_fonts = {}


def F(path, size):
    key = (path, size)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(path, int(size * S), index=0)
    return _fonts[key]


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def sc(v):
    return int(round(v * S))


class C:
    def __init__(self, pal):
        (self.name, bg, head, card, inner, accent, ok, warn, err, txt, mut, line, rad, light) = pal
        self.bg, self.head, self.card, self.inner = rgb(bg), rgb(head), rgb(card), rgb(inner)
        self.accent, self.ok, self.warn, self.err = rgb(accent), rgb(ok), rgb(warn), rgb(err)
        self.txt, self.mut, self.ln = rgb(txt), rgb(mut), rgb(line)
        self.rad = rad
        self.light = light
        self.img = Image.new("RGB", (W * S, H * S), self.bg)
        self.d = ImageDraw.Draw(self.img)

    def rrect(self, x, y, w, h, r, fill=None, outline=None, width=1):
        self.d.rounded_rectangle([sc(x), sc(y), sc(x + w), sc(y + h)], radius=sc(r), fill=fill,
                                 outline=outline, width=max(1, sc(width)))

    def text(self, x, y, s, font, color, anchor="la"):
        self.d.text((sc(x), sc(y)), s, font=F(size_font(font), size_px(font)), fill=color, anchor=anchor)

    def dot(self, x, y, r, color):
        self.d.ellipse([sc(x - r), sc(y - r), sc(x + r), sc(y + r)], fill=color)

    def line(self, x1, y1, x2, y2, color, w=1):
        self.d.line([sc(x1), sc(y1), sc(x2), sc(y2)], fill=color, width=max(1, sc(w)))

    def gear(self, x, y, color):
        r = 3.5
        self.d.ellipse([sc(x - r), sc(y - r), sc(x + r), sc(y + r)], outline=color, width=sc(1.2))
        for k in range(6):
            a = math.radians(k * 60)
            self.line(x + math.cos(a) * (r + 0.8), y + math.sin(a) * (r + 0.8),
                      x + math.cos(a) * (r + 2.2), y + math.sin(a) * (r + 2.2), color, 1.2)

    def bar(self, x, y, w, h, pct, col):
        self.rrect(x, y, w, h, h / 2, self.ln)
        if pct > 0.01:
            self.rrect(x, y, max(h, w * pct), h, h / 2, col)

    def thumb(self, x, y, w, h):
        try:
            im = Image.open(THUMB).convert("RGB")
            s = (h * S) / im.height
            im = im.resize((int(im.width * s), h * S))
            left = max(0, (im.width - w * S) // 2)
            im = im.crop((left, 0, left + w * S, h * S))
            mask = Image.new("L", im.size, 0)
            ImageDraw.Draw(mask).rounded_rectangle([0, 0, im.width - 1, im.height - 1], radius=sc(4), fill=255)
            self.img.paste(im, (sc(x), sc(y)), mask)
        except Exception:
            self.rrect(x, y, w, h, 4, self.inner)

    def save(self, name):
        self.img.save(OUT / name, "PNG")
        return self.img.copy()


def size_font(kind):
    return MONO if kind == "mono" else CJK


def size_px(kind):
    return 1


# tiny wrapper so calls read as text(x,y,s,size,color,mono)
class FontSpec:
    def __init__(self, size, mono=False):
        self.size = size
        self.mono = mono


def F_(size, mono=False):
    return ("mono" if mono else "cjk", size)


# Rebind C.text to accept (size, mono)
def text(self, x, y, s, size=12, color=None, anchor="la", mono=False):
    self.d.text((sc(x), sc(y)), s, font=F(MONO if mono else CJK, size), fill=color, anchor=anchor)


C.text = text


def header(c, host_label, page_label, dots=2, active=0):
    c.rrect(0, 0, W, 34, 0, c.head)
    c.text(12, 7, "ComfyUI", 16, c.accent)
    c.dot(92, 17, 4, c.ok)
    c.text(104, 11, host_label, 11, c.mut)
    x = 236
    for i in range(dots):
        act = i == active
        w = 14 if act else 6
        c.rrect(x, 14, w, 6, 3, c.accent if act else c.mut)
        x += w + 7
    c.text(316, 10, page_label, 11, c.mut)
    c.text(424, 11, "23:59:42", 12, c.txt, anchor="ra")
    c.gear(462, 17, c.mut)


def monitor_page(c):
    header(c, "192.168.1.10:8188", "MON", 4, 0)
    # status band
    c.rrect(4, 38, 472, 74, c.rad, c.card)
    c.text(16, 42, "GENERATING", 20, c.ok)
    c.text(466, 42, "00:32", 20, c.txt, anchor="ra")
    c.text(16, 72, "node 14", 12, c.mut)
    c.text(466, 72, "queue 0", 12, c.mut, anchor="ra")
    c.bar(16, 92, 286, 10, 0.33, c.ok)
    c.text(310, 89, "step 2/6  33%  1.1s", 12, c.txt)
    # left card
    c.rrect(4, 118, 176, 198, c.rad, c.card)
    c.text(12, 122, "VRAM", 12, c.mut)
    c.text(172, 122, "13.3/20G", 12, c.txt, anchor="ra")
    c.bar(12, 140, 164, 10, 0.665, c.accent)
    c.text(12, 160, "RAM", 12, c.mut)
    c.text(172, 160, "42.2/63G", 12, c.txt, anchor="ra")
    c.bar(12, 178, 164, 10, 0.67, c.accent)
    c.text(12, 198, "GPU", 10, c.mut)
    c.text(12, 212, "cuda:0 AMD Radeon RX 7900 XT", 11, c.txt)
    c.text(12, 246, "ComfyUI v0.42.5", 11, c.mut)
    c.text(12, 292, "swipe for outputs >", 11, c.mut)
    # log card
    c.rrect(186, 118, 290, 198, c.rad, c.card)
    c.text(194, 122, "RUN LOG", 11, c.mut)
    logs = ["23:59:12 follow client-a", "23:59:13 node 9", "23:59:30 step 1/6 17.9s",
            "23:59:31 step 2/6 1.1s", "23:59:44 running 00:32", "23:59:48 node 14",
            "23:59:52 step 1/6 1.0s", "23:59:53 step 2/6 1.1s"]
    for i, s in enumerate(logs):
        c.text(194, 142 + i * 18, s, 11, c.txt)


def dual_dash(c):
    header(c, "DUAL 2 hosts", "DASH", 3, 0)
    for h, (host, state, dur, node, step, pct, vram, vp, ram, rp, gpu, ver) in enumerate([
        ("192.168.1.10", "GENERATING", "00:32", "node 15", "nodes 1/15", 0.07,
         "15.9/20G", 0.795, "43.1/63G", 0.68, "cuda:0 AMD Radeon RX 7900 XT", "ComfyUI v0.42.5"),
        ("192.168.1.11", "IDLE", "00:00", "no job", "", 0.0,
         "67.9/104G", 0.65, "80.4/123G", 0.65, "cuda:0 Radeon 8060S Graphics", "ComfyUI v0.42.0"),
    ]):
        x = 4 + h * 240
        c.rrect(x, 38, 232, 278, c.rad, c.card)
        c.text(x + 10, 44, host, 11, c.mut)
        c.text(x + 10, 62, state, 20, c.ok if state == "GENERATING" else c.accent)
        c.text(x + 222, 62, dur, 20, c.txt, anchor="ra")
        c.text(x + 10, 94, node, 12, c.mut)
        c.text(x + 222, 94, step, 12, c.txt, anchor="ra")
        c.bar(x + 10, 112, 212, 9, pct, c.ok)
        c.text(x + 10, 134, "VRAM", 11, c.mut)
        c.text(x + 222, 134, vram, 11, c.txt, anchor="ra")
        c.bar(x + 10, 152, 212, 9, vp, c.accent)
        c.text(x + 10, 174, "RAM", 11, c.mut)
        c.text(x + 222, 174, ram, 11, c.txt, anchor="ra")
        c.bar(x + 10, 192, 212, 9, rp, c.accent)
        c.text(x + 10, 214, "GPU", 10, c.mut)
        c.text(x + 10, 228, gpu, 11, c.txt)
        c.text(x + 10, 262, ver, 11, c.mut)
        c.text(x + 10, 282, "queue 0", 11, c.mut)


def dual_logs(c):
    header(c, "DUAL 2 hosts", "LOGS", 3, 1)
    data = [
        ["00:01:02 follow client-a", "00:01:03 ws connected", "00:01:03 node 15",
         "00:01:20 running 00:18", "00:01:35 step 1/6 17.9s", "00:01:36 step 2/6 1.1s"],
        ["00:01:02 follow client-b", "00:01:03 ws connected", "00:01:05 node 9",
         "00:01:20 running 00:18", "00:01:35 running 00:33", "00:01:50 step 1/12 2.1s"],
    ]
    for h in range(2):
        x = 4 + h * 240
        c.rrect(x, 38, 232, 278, c.rad, c.card)
        c.text(x + 10, 44, ("192.168.1.10" if h == 0 else "192.168.1.11"), 11, c.mut)
        c.text(x + 222, 44, "GENERATING", 11, c.ok, anchor="ra")
        for i, s in enumerate(data[h]):
            c.text(x + 10, 66 + i * 18, s, 11, c.txt)


def dual_outputs(c):
    header(c, "DUAL 2 hosts", "OUT", 3, 2)
    names = [("demo_a_00042", "23:48 167s"), ("demo_a_00041", "23:45 157s"),
             ("demo_b_00018", "23:41 421s"), ("demo_b_00017", "23:34 413s")]
    for h in range(2):
        x = 4 + h * 240
        c.rrect(x, 38, 232, 278, c.rad, c.card)
        c.text(x + 10, 44, ("192.168.1.10" if h == 0 else "192.168.1.11"), 11, c.mut)
        for k in range(2):
            y = 62 + k * 122
            c.rrect(x + 4, y, 220, 114, max(2, c.rad - 2), c.inner)
            c.thumb(x + 10, y + 6, 100, 84)
            nm, meta = names[h * 2 + k]
            c.text(x + 116, y + 6, nm, 11, c.txt)
            c.text(x + 116, y + 24, "00001.mp4", 11, c.txt)
            c.text(x + 116, y + 48, meta + " ok", 11, c.mut)


def sheet(images, path, cols=2):
    cw, ch, pad, lab = 480, 320, 12, 34
    rows = (len(images) + cols - 1) // cols
    sh = Image.new("RGB", (cols * cw + (cols + 1) * pad, rows * (ch + lab) + (rows + 1) * pad), (18, 20, 26))
    d = ImageDraw.Draw(sh)
    for i, (title, im) in enumerate(images):
        r, ci = divmod(i, cols)
        x = pad + ci * (cw + pad)
        y = pad + r * (ch + lab + pad)
        sh.paste(im.resize((cw, ch), Image.LANCZOS), (x, y))
        d.text((x + 4, y + ch + 8), title, font=ImageFont.truetype(CJK, 16), fill=(230, 235, 245))
    sh.save(OUT / path)
    print("wrote", OUT / path)


def main():
    single, dual, pages = [], [], []
    for pal in THEMES:
        c = C(pal)
        monitor_page(c)
        single.append((pal[0], c.save(f"single_{pal[0].lower().replace(' ', '_')}.png")))

        c = C(pal)
        dual_dash(c)
        dual.append((pal[0], c.save(f"dual_{pal[0].lower().replace(' ', '_')}.png")))

    c = C(THEMES[0])
    dual_dash(c)
    pages.append(("DUAL DASH", c.save("dual_page_dash.png")))
    c = C(THEMES[0]); dual_logs(c); pages.append(("DUAL LOGS", c.save("dual_page_logs.png")))
    c = C(THEMES[0]); dual_outputs(c); pages.append(("DUAL OUTPUTS", c.save("dual_page_outputs.png")))
    sheet(single, "single_themes_contact.png")
    sheet(dual, "dual_themes_contact.png")
    sheet(pages, "dual_pages_contact.png", cols=3)


if __name__ == "__main__":
    main()
