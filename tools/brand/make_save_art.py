#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Memory card icons for the Multishine save files (GameCube BIOS card screen, Swiss, card managers).  Stdlib only, deterministic, no fonts.

One base logo plus a small corner glyph per file kind.
Output is the exact on-card layout of ONE icon frame, CI8 with its own 256 entry RGB5A3 palette:
    0x000..0x3FF  32x32 pixels, 8x4 tiles (8 wide, 4 high), row-major tile order, 1 byte per pixel (palette index)
    0x400..0x5FF  256 x u16 big-endian RGB5A3 palette (index 0 = transparent)
= ICON_LEN 0x600 bytes.  In the directory entry this is icon format 1 (CI8, palette after the last icon) at image offset 0x40 with no banner, so a
file that already had a 0x800 byte RGB5A3 placeholder icon at 0x40 does not grow (its payload starts at 0x1000).  A 96x32 banner (CI8 3,584 bytes)
does not fit before 0x1000 together with the icon, so there is none (it would cost one more block per file).

  make_save_art.py --sheet OUT.png        contact sheet of all kinds (4x, dark background), order = KINDS
  make_save_art.py --hashes               sha256 of every kind's icon bytes
"""
import argparse
import hashlib
import math
import struct
import sys
import zlib

ICON_W = ICON_H = 32
ICON_LEN = 0x600                     # 0x400 pixels + 0x200 palette
KINDS = ("code", "ovl", "identity", "options", "online", "codes", "state", "pack", "bridge")
NAVY, CYAN, WHITE, AMBER, BORDER = (0x0A, 0x18, 0x30), (0x37, 0xD6, 0xFF), (0xEA, 0xF9, 0xFF), (0xF1, 0xC3, 0x6B), (0x1F, 0x3A, 0x5C)
PRIVATE = ("identity", "online")      # amber glyph: the file holds something private

# the five hexagons of f5_42w.svg: (slot, radius, stroke, opacity), drawn in this order (outer first, centre on top); pointy-top, centres 42 apart
HEXES = ((-2, 36, 3.38, 0.50), (2, 36, 3.38, 0.50), (-1, 40, 5.40, 0.75), (1, 40, 5.40, 0.75), (0, 44, 7.77, 1.00))
HEX_SPAN = 2 * 84 + 2 * 36 * math.cos(math.radians(30))      # x extent of the logo in svg units (230.4)
HEX_HEIGHT = 88.0
HEAVY = 1.7                                                  # stroke boost for a 32 px icon

GLYPHS = {
    "ovl": ("#.#.###", "#.#..#.", "#.#..#.", "#.#..#.", "###.###"),                                  # "UI"
    "identity": (".###........", "#...#.......", "#...########", "#...#..#.#..", ".###........"),   # a key (a symbol only)
    "options": ("...#...", ".#.#.#.", "..###..", "###.###", "..###..", ".#.#.#.", "...#..."),         # cog
    "online": ("..###..", ".#.#.#.", "#..#..#", "#######", "#..#..#", ".#.#.#.", "..###.."),         # globe
    "codes": (".#...#.", ".#...#.", "#######", ".#...#.", "#######", ".#...#.", ".#...#."),           # '#' of a connect code
    "state": (".#######.", ".........", "#########", ".........", ".#######."),                       # stack
    "pack": (".#######.", "#########", "#...#...#", "#...#...#", "#...#...#", "#########"),            # crate
    "bridge": ("..#####..", ".#.....#.", "#.......#", "#########"),                                    # arch bridge
}


def _seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def _hex_pts(cx, r):
    return [(cx + r * math.cos(math.radians(30 + 60 * i)), 100 + r * math.sin(math.radians(30 + 60 * i))) for i in range(6)]


def _coverage(width_px, cy, ss=6):
    """-> {(px, py): alpha 0..1} of the logo (over-composited layers, ss x ss supersampling)."""
    k = width_px / HEX_SPAN
    x0 = 16.0 - width_px / 2
    polys = []
    for slot, r, sw, op in HEXES:
        pts = _hex_pts(100 + slot * 42, r)
        polys.append(([(x0 + (x + 15.18) * k, cy + (y - 100) * k) for x, y in pts], sw * k * HEAVY / 2, op))
    out = {}
    ylo, yhi = int(cy - HEX_HEIGHT * k / 2) - 2, int(cy + HEX_HEIGHT * k / 2) + 3
    for py in range(max(0, ylo), min(ICON_H, yhi)):
        for px in range(ICON_W):
            tot = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    x, y = px + (sx + 0.5) / ss, py + (sy + 0.5) / ss
                    a = 0.0
                    for pts, hw, op in polys:
                        d = min(_seg_dist(x, y, pts[i][0], pts[i][1], pts[(i + 1) % 6][0], pts[(i + 1) % 6][1]) for i in range(6))
                        if d <= hw:
                            a = a + (1.0 - a) * op
                    tot += a
            v = tot / (ss * ss)
            if v > 0.004:
                out[(px, py)] = v
    return out


def _mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def _in_tile(x, y, r=4):
    """Rounded square mask (corner radius r, pixel centres)."""
    cx = min(max(x + 0.5, r), ICON_W - r)
    cy = min(max(y + 0.5, r), ICON_H - r)
    return math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= r - 0.15


def _palette():
    """-> list of 256 RGB tuples (index 0 transparent) and the ramp starts."""
    pal = [(0, 0, 0)]
    pal += [_mix(NAVY, CYAN, i / 55.0) for i in range(56)]            # 1..56   navy -> cyan
    pal += [_mix(NAVY, WHITE, i / 23.0) for i in range(24)]           # 57..80  navy -> white
    pal += [_mix(NAVY, AMBER, i / 15.0) for i in range(16)]           # 81..96  navy -> amber
    pal += [_mix(NAVY, BORDER, i / 3.0) for i in range(4)]            # 97..100 navy -> border
    while len(pal) < 256:
        pal.append(NAVY)
    return pal


def _rgb5a3(rgb):
    r, g, b = (int(round(c)) for c in rgb)
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def _nearest(pal_q, rgb, lo, hi):
    best, bi = None, lo
    for i in range(lo, hi):
        p = pal_q[i]
        d = (p[0] - rgb[0]) ** 2 + (p[1] - rgb[1]) ** 2 + (p[2] - rgb[2]) ** 2
        if best is None or d < best:
            best, bi = d, i
    return bi


def render(kind):
    """-> (32x32 list of palette indices, palette of 256 RGB tuples)."""
    if kind not in KINDS:
        raise ValueError("unknown icon kind %r" % (kind,))
    pal = _palette()
    # the palette as the card stores it (5 bit channels): quantise against that, so the picture is what the BIOS shows
    pq = [tuple(((_rgb5a3(c) >> s) & 31) * 255 // 31 for s in (10, 5, 0)) for c in pal]
    img = [[NAVY if _in_tile(x, y) else None for x in range(ICON_W)] for y in range(ICON_H)]
    for y in range(ICON_H):
        for x in range(ICON_W):
            if img[y][x] is not None and (x in (0, ICON_W - 1) or y in (0, ICON_H - 1) or not all(
                    _in_tile(x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if 0 <= x + dx < ICON_W and 0 <= y + dy < ICON_H)):
                img[y][x] = BORDER
    if kind == "code":
        cov = _coverage(30.0, 16.0)
    else:
        cov = _coverage(28.0, 11.0)
    for (x, y), a in cov.items():
        if img[y][x] is not None and x not in (0, ICON_W - 1):
            img[y][x] = _mix(img[y][x], CYAN, a)
    gl = GLYPHS.get(kind)
    gmask = {}
    if gl:
        w, h = len(gl[0]), len(gl)
        gx, gy = 16 - w // 2, 29 - h
        for j, row in enumerate(gl):
            for i, ch in enumerate(row):
                if ch == "#":
                    gmask[(gx + i, gy + j)] = 81 + 15 if kind in PRIVATE else 57 + 23      # full amber / full white
    out = []
    for y in range(ICON_H):
        row = []
        for x in range(ICON_W):
            c = img[y][x]
            if c is None:
                row.append(0)
            elif (x, y) in gmask:
                row.append(gmask[(x, y)])
            else:
                # colour on the navy->cyan ramp (the logo) or the border ramp
                if c == BORDER:
                    row.append(97 + 3)
                else:
                    row.append(_nearest(pq, c, 1, 57))
        out.append(row)
    return out, pal


def icon_ci8(kind):
    """-> ICON_LEN bytes: the 32x32 CI8 pixels in 8x4 tiles, then the 256 entry RGB5A3 palette (index 0 = transparent)."""
    idx, pal = render(kind)
    px = bytearray()
    for ty in range(ICON_H // 4):
        for tx in range(ICON_W // 8):
            for y in range(4):
                for x in range(8):
                    px.append(idx[ty * 4 + y][tx * 8 + x])
    tl = bytearray()
    for i, c in enumerate(pal):
        tl += struct.pack(">H", 0x0000 if i == 0 else _rgb5a3(c))
    out = bytes(px) + bytes(tl)
    assert len(out) == ICON_LEN
    return out


def decode_ci8(data):
    """ICON_LEN bytes -> 32x32 rows of (r, g, b, a) (what a reader of the card shows)."""
    if len(data) != ICON_LEN:
        raise ValueError("an icon is %d bytes" % ICON_LEN)
    pal = []
    for i in range(256):
        v = struct.unpack_from(">H", data, 0x400 + 2 * i)[0]
        if v & 0x8000:
            pal.append((((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255))
        else:
            pal.append((((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7))
    img = [[None] * ICON_W for _ in range(ICON_H)]
    o = 0
    for ty in range(ICON_H // 4):
        for tx in range(ICON_W // 8):
            for y in range(4):
                for x in range(8):
                    img[ty * 4 + y][tx * 8 + x] = pal[data[o]]
                    o += 1
    return img


def kind_of(name):
    """File name ('MultishineCodes', 'MultishineCodeB', ...) -> icon kind, or None for a name that is not ours / has no icon."""
    rest = name[len("Multishine"):] if name.startswith("Multishine") else None
    if rest is None:
        return None
    if rest == "Codes":
        return "codes"
    for pre, kind in (("Code", "code"), ("Ovl", "ovl"), ("Identity", "identity"), ("Options", "options"), ("Online", "online"),
                      ("State", "state"), ("Pack", "pack"), ("Bridge", "bridge")):
        if rest.startswith(pre):
            return kind
    return None


def write_png(path, rows):
    """rows: list of rows of (r, g, b, a); plain 8 bit RGBA PNG."""
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for px in r for c in px) for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sheet(path, scale=4, cols=5, gap=12, bg=(0x05, 0x08, 0x0F, 255)):
    n = len(KINDS)
    rows_n = (n + cols - 1) // cols
    cell = ICON_W * scale
    W, H = gap + cols * (cell + gap), gap + rows_n * (cell + gap)
    img = [[bg] * W for _ in range(H)]
    for k, kind in enumerate(KINDS):
        ic = decode_ci8(icon_ci8(kind))
        ox, oy = gap + (k % cols) * (cell + gap), gap + (k // cols) * (cell + gap)
        for y in range(ICON_H):
            for x in range(ICON_W):
                r, g, b, a = ic[y][x]
                col = bg if a == 0 else (r, g, b, 255)
                for dy in range(scale):
                    row = img[oy + y * scale + dy]
                    for dx in range(scale):
                        row[ox + x * scale + dx] = col
    write_png(path, img)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sheet")
    ap.add_argument("--hashes", action="store_true")
    a = ap.parse_args(argv)
    if a.sheet:
        sheet(a.sheet)
        print("wrote %s (%s)" % (a.sheet, ", ".join(KINDS)))
    if a.hashes or not a.sheet:
        for k in KINDS:
            print("%-9s %s" % (k, hashlib.sha256(icon_ci8(k)).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
