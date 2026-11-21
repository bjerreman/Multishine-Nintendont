#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Homebrew Channel assets of the Multishine loader: icon.png (128x48, only our 'Full 5 open' mark on the app's navy #0a1424) and meta.xml.
   python3.14 gen_hbc_assets.py OUT_DIR"""
import math, sys, pathlib
from PIL import Image, ImageDraw

NAVY, CYAN = (10, 20, 36), (55, 214, 255)
W, H, SS = 128, 48, 8

def mark(width):
    d, k = 42, 92.0 / (2 * 42 + 0.866 * 36)
    sw_, sh_ = width * SS, width * SS // 2
    sc = sw_ / 192.0
    im = Image.new("RGBA", (sw_, sh_), (0, 0, 0, 0))
    for n, r, sw, o in [(-2, 36, 2.6 * 1.35, .62), (2, 36, 2.6 * 1.35, .62), (-1, 40, 3.6 * 1.35, .85), (1, 40, 3.6 * 1.35, .85), (0, 44, 4.8 * 1.35, 1.0)]:
        layer = Image.new("RGBA", im.size, (0, 0, 0, 0)); dr = ImageDraw.Draw(layer)
        cx = 100 + n * d
        pts = [(100 + (cx + r * math.cos(math.radians(30 + 60 * i)) - 100) * k, 100 + (100 + r * math.sin(math.radians(30 + 60 * i)) - 100) * k) for i in range(6)]
        pts = [((x - 4) * sc, (y - 52) * sc) for x, y in pts]
        dr.line(pts + [pts[0], pts[1]], fill=CYAN + (int(255 * o),), width=max(1, int(sw * k * sc)), joint="curve")
        im = Image.alpha_composite(im, layer)
    return im.resize((width, width // 2), Image.LANCZOS)

def main(out):
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    # drawn with its true shape: the Homebrew Channel squeezes every icon by 0.75 on a Wii set to 16:9, a setting, not a fault of the file
    m = mark(120); m = m.crop(m.getbbox()); tw = 104; th = round(tw * m.height / m.width)
    m = m.resize((tw, th), Image.LANCZOS)
    bg = Image.new("RGBA", (W, H), NAVY + (255,))
    bg.alpha_composite(m, ((W - m.width) // 2, (H - m.height) // 2))
    bg.convert("RGBA").save(out / "icon.png")
    (out / "meta.xml").write_text("""<?xml version="1.0" encoding="ISO-8859-1" standalone="yes"?>
<app version="1">
\t<name>Nintendont (Multishine)</name>
\t<coder>Nigzel</coder>
\t<version>Multishine loader v1</version>
\t<release_date>20261103000000</release_date>
\t<short_description>Play Melee online with Multishine</short_description>
\t<long_description>This build of Nintendont is made specifically to handle and run Multishine, a way to play Smash Melee online with rollback and new game modes on original hardware.

www.multishine.link

A big thanks go out to the Nintendont and Slippi team whose work made this possible.</long_description>
\t<no_ios_reload/>
\t<ahb_access/>
</app>
""", encoding="latin-1")

if __name__ == "__main__":
    main(sys.argv[1])
