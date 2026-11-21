#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""The Multishine look of the Nintendont shell, generated deterministically at build time (stage_loader.py calls it):

  gen_shell_theme.py TREE_LOADER_DATA        writes data/font.zip (docs/brand/Aileron-Regular.ttf as font.ttf; the loader unzips it into GRRLIB_LoadTTF) and data/background.png
  gen_shell_theme.py --preview OUT.png [DIR] an approximate mock-up of the game list and the Update screen (PIL, the same glyph placement rules as the loader; NOT a frame of the real loader)

background.png = flat brand navy #0a1424, 640x480, with our hexagon mark in cyan #37d6ff
with the same opacity fade, 180 px wide, centred horizontally with its centre at y 375.  No Nintendo or Slippi art.  stdlib only for the build (PIL only for --preview).
Aileron (Sora Sagano, dotcolon.net) is CC0 / "No Rights Reserved"; the font file is the one in docs/brand/ and is embedded as is."""
import math, os, struct, sys, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
TTF = os.path.join(REPO, "docs", "brand", "Aileron-Regular.ttf")
NAVY = (10, 20, 36)          # #0a1424
CYAN = (0x37, 0xD6, 0xFF)    # #37d6ff
W, H = 640, 480
LOGO_W, LOGO_CX, LOGO_CY = 180, 320, 375
# colours as the loader's 0xRRGGBBAA: body, muted, accent, warning, ok, error
PALETTE = {"body": 0xE3EDF7FF, "muted": 0x98ABC2FF, "accent": 0x37D6FFFF, "warning": 0xF3DD9AFF, "ok": 0x6FDC9CFF, "error": 0xFF7B7BFF}


def font_zip():
    data = open(TTF, "rb").read()
    import io
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        zi = zipfile.ZipInfo("font.ttf", (2026, 11, 3, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        z.writestr(zi, data)
    return b.getvalue()


def _hexagons():
    """the five hexagons of the project's other tooling _logo_svg in its viewBox (4 52 192 96): [(points, stroke width, opacity)], outermost first"""
    d, k = 42, 92.0 / (2 * 42 + 0.866 * 36)
    out = []
    for n, r, sw, o in [(-2, 36, 2.6 * 1.35, .62), (2, 36, 2.6 * 1.35, .62), (-1, 40, 3.6 * 1.35, .85), (1, 40, 3.6 * 1.35, .85), (0, 44, 4.8 * 1.35, 1.0)]:
        cx = 100 + n * d
        pts = [(100 + (cx + r * math.cos(math.radians(30 + 60 * i)) - 100) * k, 100 + (r * math.sin(math.radians(30 + 60 * i))) * k) for i in range(6)]
        out.append((pts, sw * k, o))
    return out


def _dist2(px, py, a, b):
    ax, ay, bx, by = a[0], a[1], b[0], b[1]
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    ex, ey = ax + t * dx - px, ay + t * dy - py
    return ex * ex + ey * ey


def background_rgb():
    """-> bytearray W*H*3"""
    img = bytearray(bytes(NAVY) * (W * H))
    s = LOGO_W / 192.0                       # px per viewBox unit
    ox, oy = LOGO_CX - 96 * s, LOGO_CY - 48 * s       # the viewBox is 192 x 96 units, origin (4, 52)
    SS = 4
    for pts, sw, op in _hexagons():
        closed = pts + [pts[0]]
        segs = list(zip(closed[:-1], closed[1:]))
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        x0, x1 = int(ox + (min(xs) - 4 - sw) * s) - 1, int(ox + (max(xs) - 4 + sw) * s) + 2
        y0, y1 = int(oy + (min(ys) - 52 - sw) * s) - 1, int(oy + (max(ys) - 52 + sw) * s) + 2
        r2 = (sw / 2) ** 2
        for y in range(y0, y1):
            for x in range(x0, x1):
                hit = 0
                for j in range(SS):
                    for i in range(SS):
                        px = ((x + (i + .5) / SS) - ox) / s + 4
                        py = ((y + (j + .5) / SS) - oy) / s + 52
                        if min(_dist2(px, py, a, b) for a, b in segs) <= r2:
                            hit += 1
                if hit:
                    a = hit / (SS * SS) * op
                    o = (y * W + x) * 3
                    for c in range(3):
                        img[o + c] = int(round(img[o + c] * (1 - a) + CYAN[c] * a))
    return img


def png(rgb):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgb[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def cmap_codepoints(path=TTF):
    """the code points Aileron covers (format 4 cmap): the loader prints '?' for anything else"""
    d = open(path, "rb").read()
    n = struct.unpack_from(">H", d, 4)[0]
    tabs = {d[12 + 16 * i:16 + 16 * i]: struct.unpack_from(">II", d, 20 + 16 * i) for i in range(n)}
    off = tabs[b"cmap"][0]
    cps = set()
    for i in range(struct.unpack_from(">H", d, off + 2)[0]):
        _, _, o = struct.unpack_from(">HHI", d, off + 4 + 8 * i)
        t = off + o
        if struct.unpack_from(">H", d, t)[0] == 4:
            sc = struct.unpack_from(">H", d, t + 6)[0] // 2
            ends = struct.unpack_from(">%dH" % sc, d, t + 14)
            starts = struct.unpack_from(">%dH" % sc, d, t + 16 + 2 * sc)
            for a, b in zip(starts, ends):
                cps.update(range(a, b + 1))
    cps.discard(0xFFFF)
    return cps


def write(dest):
    os.makedirs(dest, exist_ok=True)
    open(os.path.join(dest, "font.zip"), "wb").write(font_zip())
    open(os.path.join(dest, "background.png"), "wb").write(png(background_rgb()))


# ---- the mock-up ----------------------------------------------------------------------------------------------------------------------------------------------------------
def layout(s, size, font, cover):
    """[(x offset px, char)] and the width: the same rules as the loader's GRRLIB_PrintfTTFW (proportional advances; a run of 2+ spaces, or one space before ':', jumps to the next column of the old
    10 px grid; a glyph the font lacks becomes '?')"""
    cell = (size * 5 + 4) // 8
    pen, idx, out, i = 0.0, 0, [], 0
    while i < len(s):
        ch = s[i]
        i += 1
        if ch == " ":
            k = 1
            while i < len(s) and s[i] == " ":
                i += 1; k += 1
            if k >= 2 or (i < len(s) and s[i] == ":"):
                pen = max(pen, (idx + k) * cell)
                idx += k
                continue
        ch = ch if ord(ch) in cover else "?"
        idx += 1
        out.append((round(pen), ch))
        pen += font.getlength(ch)
    return out, round(pen)


def preview(out, outdir=None):
    from PIL import Image, ImageDraw, ImageFont
    cover = cmap_codepoints()
    bg = Image.frombytes("RGB", (W, H), bytes(background_rgb()))
    fonts = {}

    def font(size):
        return fonts.setdefault(size, ImageFont.truetype(TTF, size))

    def width(s, size=16):
        return layout(s, size, font(size), cover)[1]

    def text(im, x, y, s, color, size=16):
        d = ImageDraw.Draw(im)
        c = ((color >> 24) & 255, (color >> 16) & 255, (color >> 8) & 255)
        for dx, ch in layout(s, size, font(size), cover)[0]:
            d.text((x + dx, y + size), ch, fill=c, font=font(size), anchor="ls")

    B, MU, AC, WA, OK, ER = (PALETTE[k] for k in ("body", "muted", "accent", "warning", "ok", "error"))

    def title(im, t):
        text(im, 25, 34, t, B)

    def footer(im, items, disabled=()):
        """the same layout as ms_foot in ms_ui.c: one line, (key, action) pairs in the given order, 24 px gaps squeezed if needed, centred, text top y 430"""
        pairs = [(k + ":", a, i in disabled) for i, (k, a) in enumerate(items)]
        w = [width(k + " " + a) for k, a, _ in pairs]
        gap = 24
        total = sum(w) + gap * (len(pairs) - 1)
        if total > 592 and len(pairs) > 1:
            gap = max(6, gap - (total - 592 + len(pairs) - 2) // (len(pairs) - 1))
            total = sum(w) + gap * (len(pairs) - 1)
        x = max(24, (640 - total) // 2)
        for (k, a, dis), ww in zip(pairs, w):
            text(im, x, 430, k, MU)
            text(im, x + width(k + " "), 430, a, 0x6F829AFF if dis else B)
            x += ww + gap

    def guides(im):
        d = ImageDraw.Draw(im)
        for x in (24, 616):
            d.line([(x, 0), (x, H)], fill=(60, 30, 40), width=1)
        d.line([(0, 450), (W, 450)], fill=(60, 30, 40), width=1)

    def sel_pill(im, x, y, w, h):
        """the selection pill of ms_sel_pill in ms_ui.c: #225E8A, fully round ends, 10 px left of the text x, centred on the text (y + 11); drawn BEFORE the text"""
        ss = 4
        lay = Image.new("L", (w * ss, h * ss), 0)
        ImageDraw.Draw(lay).rounded_rectangle([0, 0, w * ss - 1, h * ss - 1], radius=h * ss // 2, fill=255)
        lay = lay.resize((w, h), Image.LANCZOS)
        im.paste(Image.new("RGB", (w, h), (34, 94, 138)), (x - 10, y + 11 - h // 2), lay)

    def row(im, y, name, ident, sel, color=B):
        if sel:
            sel_pill(im, 25, y, 560, 20); color = B
        text(im, 25, y, name, color); text(im, 25 + 420, y, "[%s]" % ident, color)

    def bar(im, pm, label, err=False):
        x, w, y, h = 140, 360, 112, 12
        dr = ImageDraw.Draw(im)
        dr.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=h // 2, fill=(26, 44, 68))
        fw = max(h, w * pm // 1000)
        dr.rounded_rectangle([x, y, x + fw - 1, y + h - 1], radius=h // 2, fill=(255, 123, 123) if err else (55, 214, 255))
        text(im, 320 - width(label) // 2, y + h + 12, label, ER if err else B)

    shots = []
    a = bg.copy(); title(a, "Select a device")
    wu = width("USB"); pw = wu + 36                  # ms_dev_rows: centred labels, the pill = widest label + 18 px each side
    sel_pill(a, 320 - pw // 2 + 10, 154, pw, 20); text(a, 320 - width("SD") // 2, 154, "SD", B); text(a, 320 - wu // 2, 174, "USB", B)
    text(a, 25, 114, "WARNING: USB FAT device could not be opened.", ER)
    footer(a, [("A", "Select"), ("Start", "About"), ("X", "Download Multishine"), ("Hold B", "Exit")]); shots.append(a)
    b = bg.copy(); title(b, "Select a game")
    text(b, 25, 76, "Colors:", MU, 14); lx = 25 + width("Colors: ", 14)
    for nm, col in (("1:1", B), ("Shrunk", WA), ("FST", OK), ("CISO", AC), ("Multi", 0xC9B5FFFF), ("Over", WA)):
        text(b, lx, 76, nm, col, 14); lx += width(nm, 14) + 12
    row(b, 144, "Boot GC Disc in Drive", "DISC01", False); row(b, 164, "Super Smash Bros. Melee", "GALE01", True); row(b, 184, "a modified image", "GALE01", False)
    footer(b, [("B", "Back"), ("A", "Select"), ("Y", "Settings")]); shots.append(b)
    c = bg.copy(); title(c, "Settings")
    labels = [("Network Profile", "Auto"), ("Auto Boot", "On "), ("Memcard Emulation", "On "), ("Force Widescreen", "Off"), ("Force Progressive", "On "), ("Drive Access LED", "Off"),
              ("Video", "Auto"), ("Video Width", "Auto"), ("Screen Position", "0"), ("WiiU Widescreen", "Off")]
    sel_pill(c, 75, 76 + 16 * 2, 250, 18)           # the cursor stands on Memcard Emulation
    for r, (lab, val) in enumerate(labels):
        text(c, 75, 76 + 16 * r, "%-18s:%s" % (lab, val), B if lab != "WiiU Widescreen" else MU)
    for k, ln in enumerate(["Network Profile", "Selects the network profile", "of the console used by the", "Broadband Adapter emulation."]):
        text(c, 325, 76 + 16 * k, ln, B)
    footer(c, [("B", "Back"), ("A", "Change")]); shots.append(c)
    def wrap(im, x, y, text, color, maxw=590, step=20, size=16):
        """the loader's wrap_text: greedy, at spaces, Aileron widths; -> number of lines"""
        lines, cur = [], ""
        for wd in text.split():
            trial = (cur + " " + wd).strip()
            if cur and width(trial, size) > maxw:
                lines.append(cur); cur = wd
            else:
                cur = trial
        lines.append(cur)
        for i, ln in enumerate(lines):
            text_(im, x, y + i * step, ln, color, size)
        return len(lines)
    text_ = text
    d = bg.copy(); title(d, "About")
    y = 74
    text(d, 25, y, "Multishine", B); y += 24
    y += 20 * wrap(d, 25, y, "This build of Nintendont is made specifically to handle and run Multishine, a way to play Smash Melee online with rollback and new game modes on original hardware.", B)
    y += 6
    text(d, 25, y, "www.multishine.link", AC); y += 28
    y += 20 * wrap(d, 25, y, "A big thanks go out to the Nintendont and Slippi team whose work made this possible.", MU)
    text(d, 25, y + 2, "GPL-2.0 / GPL-3.0. Source: github.com/bjerreman/Multishine-Nintendont", MU, 14)
    y += 24
    text(d, 25, y, "Multishine loader v1, shell 6, kernel inside this app", B, 14)
    text(d, 25, y + 18, "Card file SD:/multishine/saves/GALE.raw: found", B, 14)
    about_y = y
    text(d, 25, y + 36, "Backup card (slot B): ready", B, 14)
    text(d, 25, y + 62, "Up to date", B)
    footer(d, [("B", "Back"), ("A", "Check for updates"), ("X", "Memory card"), ("Y", "Diagnostics"), ("Start", "Reset")]); shots.append(d)
    e = bg.copy(); title(e, "Update")
    text(e, 75, 134, "Update Multishine Nintendont", B)
    text(e, 75, 174, "Kernel: 0 (in this app), shell: 0 (in this app)", B)
    text(e, 75, 194, "Press A to check and install.", B)
    text(e, 75, 214, "Press X to put Multishine on a memory card.", B)
    footer(e, [("B", "Back"), ("A", "Check"), ("X", "Memory card")]); shots.append(e)
    def memcard_shot(slot_a="Slot A: 212 free blocks", slot_b="Slot B: no memory card"):
        """MemoryCardMenu: the virtual card in blocks, the free blocks of both slots (B shown only), today's size, X = slot A only; line_y(n) = 54 + 20 n"""
        f = bg.copy(); title(f, "Memory card")
        ly = lambda n: 34 + 20 * n                    # line_y = MENU_POS_Y (34) + 20 n
        text(f, 75, ly(5), "A: Create a virtual card with Multishine (251 blocks)", B)
        text(f, 75, ly(6) + 2, "File: SD:/multishine/saves/GALE.raw", MU, 14)
        text(f, 75, ly(7) + 2, "An existing card file is kept as GALE.raw.old.", MU, 14)
        text(f, 75, ly(8) + 2, "Backup card (slot B): ready", MU, 14); mark(f, "good", ly(8) - 2)
        text(f, 75, ly(9) + 2, "Your Melee disc image in SD:/games is used for the system files.", MU, 14)
        text(f, 75, ly(11), "X: Install on the memory card in slot A (free blocks only)", B)
        text(f, 75, ly(12) + 2, "%s. %s." % (slot_a, slot_b), MU, 14)
        text(f, 75, ly(13) + 2, "Multishine needs about 186 blocks today. This can change with updates.", MU, 14)
        footer(f, [("B", "Back"), ("A", "Virtual card"), ("X", "Slot A")])
        return f
    # the Virtual card screen at its end state: the real 14 lines (text), scrolled like sayf (ms_say.h: first line y 80, pitch 20, the last line ends above y 328)
    v = bg.copy(); title(v, "Virtual card")
    lines = [("New virtual card (251 blocks), with the Multishine files", B), ("Looking for the Multishine card files...", B), ("Downloaded card files 1193.", B),
             ("Looking for the Multishine system files...", B), ("Downloaded system files 1193.", B), ("Card files 1193 verified (150 blocks).", B),
             ("Building the Multishine system files: 100 %", B), ("The Multishine system files are on the card.", B), ("Your card file is kept as GALE.raw.old.", B), ("Writing the card (251 blocks)...", B),
             ("Done: SD:/multishine/saves/GALE.raw", B), ("251 blocks: 5 files + system files, 71 free.", B),
             ("Start your Melee disc with memory card emulation on.", B)]
    cap = (328 - 20 - 80) // 20 + 1
    for k, (s, c) in enumerate(lines[max(0, len(lines) - cap):]):
        text(v, 25, 80 + 20 * k, s, c)
    footer(v, [("A", "Back")]); shots.append(v)
    h = bg.copy(); bar(h, 560, "Starting Multishine"); shots.append(h)
    k = bg.copy(); bar(k, 650, "Could not start (error 3)", True)
    text(k, 25, 34 + 20 * 9, "Mounting USB/SD device... Error! -3 Shutting down", ER); shots.append(k)
    # ---- the start screen and the quick set-up (ms_ota_loader.c start_draw / setup_draw; texts and footers of ms_start.h) ----
    def ctext(im, y, s, color, size=16):
        text(im, 320 - width(s, size) // 2, y, s, color, size)
    START_FOOT = [("A", "Launch"), ("X", "Browse"), ("Y", "Settings"), ("Start", "About"), ("Hold B", "Exit")]

    def stroke(im, p0, p1, col):
        """ms_ui.c mark_stroke: 2 x 2 px rectangles along the line"""
        (x0, y0), (x1, y1) = p0, p1
        dx, dy = x1 - x0, y1 - y0
        n = max(abs(dx), abs(dy))
        d = ImageDraw.Draw(im)
        for i in range(n + 1):
            x = x0 + (0 if not n else int((dx * i + (-n // 2 if dx < 0 else n // 2)) / n))
            y = y0 + (0 if not n else int((dy * i + (-n // 2 if dy < 0 else n // 2)) / n))
            d.rectangle([x, y, x + 1, y + 1], fill=col)

    def mark(im, kind, y):
        """ms_mark in ms_ui.c: 14 x 14 box, right edge x 590, centred on the row text (text top y)"""
        x, t = 590 - 14, y + 4
        if kind == "good":
            c = (0x6F, 0xDC, 0x9C); stroke(im, (x, t + 7), (x + 4, t + 11), c); stroke(im, (x + 4, t + 11), (x + 12, t + 2), c)
        elif kind == "bad":
            c = (0xFF, 0x7B, 0x7B); stroke(im, (x + 1, t + 1), (x + 11, t + 11), c); stroke(im, (x + 11, t + 1), (x + 1, t + 11), c)
        elif kind == "work":
            d = ImageDraw.Draw(im)
            for dx in (0, 5, 10):
                d.rectangle([x + dx, t + 6, x + dx + 2, t + 8], fill=(0x37, 0xD6, 0xFF))

    def rule(im, y):
        ImageDraw.Draw(im).rectangle([100, y + 24, 589, y + 24], fill=(26, 44, 68))

    def start_shot(kind, pill=False, dev=0, noiso_line="No Melee game found on SD. A: Use the disc drive, X: Browse."):
        """kind: launch (all three good), setup (no card / no files), noiso (no stock image), nofiles (card there, files missing)"""
        im = bg.copy(); title(im, "Multishine")
        disc, card, files = True, True, True
        if kind == "setup": card = files = False
        if kind == "nofiles": files = False
        if kind == "noiso": disc = False
        NOISO_FOOT = [("A", "Use the disc drive"), ("X", "Browse for a game"), ("Y", "Settings"), ("Start", "About"), ("Hold B", "Exit")]
        if kind == "noiso_setup":
            card = files = False; disc = False
            ctext(im, 92, "Multishine is not set up yet.", B)          # no hint line here, the one red line below says what to do
            foot = NOISO_FOOT
        elif kind in ("setup", "nofiles"):
            ctext(im, 92, "Multishine is not set up yet.", B)
            ctext(im, 116, "A downloads the files and makes your memory card.", MU, 14)
            foot = [("A", "Set up Multishine")] + START_FOOT[1:]
        else:
            ctext(im, 92, "Melee disc in the drive" if kind == "drive" else "Super Smash Bros. Melee", B)
            ctext(im, 116, "Memory card: SD:/multishine/saves/GALE.raw", MU, 14)
            foot = START_FOOT
        for i, (lab, good) in enumerate((("Melee disc v1.02", disc), ("Multishine card", card), ("Multishine files", files))):
            y = 156 + 28 * i
            rule(im, y); text(im, 100, y, lab, B); mark(im, "good" if good else "bad", y)
        if kind == "noiso": foot = NOISO_FOOT
        if kind in ("noiso", "noiso_setup"):                                  # ONE red line (msst_none_line), wrapped at 520 px when a long name is in it
            words, cur, lines = noiso_line.split(), "", []
            for wd in words:
                tr = (cur + " " + wd).strip()
                if cur and width(tr, 14) > 520: lines.append(cur); cur = wd
                else: cur = tr
            lines.append(cur)
            for k2, ln in enumerate(lines): ctext(im, 250 + 20 * k2, ln, ER, 14)
        if pill:
            ws, wu = width("SD"), width("USB")
            xs, xu = 320 - 50 - ws // 2, 320 + 50 - wu // 2
            if dev: sel_pill(im, xu, 292, wu + 20, 20)
            else: sel_pill(im, xs, 292, ws + 20, 20)
            text(im, xs, 292, "SD", B); text(im, xu, 292, "USB", B)
            ctext(im, 316, "Left / Right: switch device", MU, 14)
        footer(im, foot)
        return im
    def setup_shot(states, detail="", dcol=None, foot=None, route="", info="", phys=False):
        """states: 4 of 'todo' 'run' 'done' 'fail' for the four steps (msst_step_text_route); route / info: the card line and the blocks line (setup_draw)"""
        im = bg.copy(); title(im, "Set up Multishine")
        names = ["Check the network", "Find your Melee disc", "Download the Multishine files", "Install on the memory card in slot A" if phys else "Create the memory card"]
        for i, (n, s) in enumerate(zip(names, states)):
            col = {"done": B, "run": B, "fail": ER, "todo": MU}[s]
            rule(im, 100 + 28 * i); text(im, 100, 100 + 28 * i, n, col)
            mark(im, {"done": "good", "run": "work", "fail": "bad", "todo": ""}[s], 100 + 28 * i)
        y0 = 100 + 28 * 4 + 14
        if route: ctext(im, y0, route, B, 14); y0 += 20
        if info: ctext(im, y0, info, MU, 14); y0 += 20
        if route or info: y0 += 8
        if detail:
            words, cur, lines = detail.split(), "", []
            for wd in words:
                tr = (cur + " " + wd).strip()
                if cur and width(tr, 14) > 460: lines.append(cur); cur = wd
                else: cur = tr
            lines.append(cur)
            for k, ln in enumerate(lines):
                ctext(im, y0 + 20 * k, ln, dcol or MU, 14)
        if foot: footer(im, foot)
        else: ctext(im, 430, "Please wait...", MU)
        return im

    def browser_shot(path, rows, sel, count, msg="", msgbad=True, markrow=None, markkind="bad", foot=None):
        """the file browser (ms_ota_loader.c br_draw, ms_browse.h): path line y 62, rows from y 90 at a 20 px pitch (name at the margin, size in MB right-aligned at x 550), the red / muted line at y 408 (one line, between the logo and the footer), the footer"""
        im = bg.copy(); title(im, "Browse for a game")
        text(im, 25, 62, path, MU, 14)
        cn = "%d / %d" % (sel + 1, count)
        text(im, 590 - width(cn, 14), 62, cn, MU, 14)
        for i, (nm, size) in enumerate(rows):
            y = 90 + 20 * i
            if i == sel: sel_pill(im, 25, y, 560, 20)
            text(im, 25, y, nm + ("/" if size is None else ""), B)
            if size is not None: text(im, 550 - width(size), y, size, MU)
            if markrow == i: mark(im, markkind, y)
        if msg:
            words, cur, lines = msg.split(), "", []
            for wd in words:
                tr = (cur + " " + wd).strip()
                if cur and width(tr, 14) > 560: lines.append(cur); cur = wd
                else: cur = tr
            lines.append(cur)
            for k2, ln in enumerate(lines): ctext(im, 408 + 20 * k2, ln, ER if msgbad else MU, 14)
        footer(im, foot or [("B", "Up"), ("A", "Open"), ("Left/Right", "Page"), ("Y", "USB")])
        return im

    mark(d, "good", about_y + 14)           # About: the card file line, same mark at the right edge
    mark(d, "good", about_y + 32)           # the slot B line

    def reset_shot(focus, hold_pm=None, line="", done=False, working=False):
        """Reset Multishine (ms_ota_loader.c ms_reset_screen / rs_draw; texts and lists of ms_reset.h)"""
        im = bg.copy(); title(im, "Reset Multishine")
        if done or working:
            text(im, 25, 94, "Done. Starting over." if done else "Removing...", B)
            return im
        y = 74
        y += 20 * wrap(im, 25, y, "Your memory cards and settings will be removed. Your Melee save is on the card: this cannot be undone.", B)
        y += 8
        y += 18 * wrap(im, 25, y, "Removed: your memory cards (slot A and B) and every backup of them, the downloaded Multishine files, your settings and the log.", MU, 590, 18, 14)
        y += 4
        y += 18 * wrap(im, 25, y, "A copy goes to SD:/multishine/reset_backup until the next reset. The app, its updates and your games stay.", MU, 590, 18, 14)
        y += 16
        if focus == 0: sel_pill(im, 75, y, 360, 20)
        text(im, 75, y, "Back", B)
        if focus == 1: sel_pill(im, 75, y + 26, 360, 20)
        text(im, 75, y + 26, "Remove memory cards and settings", B)
        if focus == 1: text(im, 75, y + 52, "Hold A for 2 seconds. A tap does nothing.", MU, 14)
        if hold_pm is not None:
            x, w, yy, h = 200, 240, 316, 8
            dr = ImageDraw.Draw(im)
            dr.rounded_rectangle([x, yy, x + w - 1, yy + h - 1], radius=h // 2, fill=(26, 44, 68))
            dr.rounded_rectangle([x, yy, x + max(h, w * hold_pm // 1000) - 1, yy + h - 1], radius=h // 2, fill=(55, 214, 255))
        if line: text(im, 25, 396, line, ER, 14)
        footer(im, [("B", "Back"), ("Up/Down", "Choose"), ("Hold A", "Remove")] if focus == 1 else [("B", "Back"), ("Up/Down", "Choose"), ("A", "Back")])
        return im
    VINFO = "Virtual card: 251 blocks. About 186 needed today (can change with updates)."

    def ask_shot():
        """setup_ask: a usable card in slot A; on a Wii A takes the virtual card, B the memory card"""
        im = bg.copy(); title(im, "Set up Multishine")
        ctext(im, 140, "A memory card is in slot A.", B)
        ctext(im, 176, "Slot A: 212 free blocks", MU, 14)
        ctext(im, 196, "Slot B: no memory card", MU, 14)
        ctext(im, 226, "Multishine needs about 186 blocks today. This can change with updates.", MU, 14)
        ctext(im, 276, "Where should Multishine go?", B)
        footer(im, [("B", "Memory card in slot A"), ("A", "Virtual card on the SD")])
        return im

    def phys_shot(short=False):
        """card_physical: the free blocks and today's size before anything is written; sayf lines from y 80 at a 20 px pitch"""
        im = bg.copy(); title(im, "Memory card")
        ls = [("Multishine onto the card in slot A", B), ("Card files 1210 verified (150 blocks).", B)]
        if short:
            ls += [("Slot A: 174 free blocks", B), ("Multishine needs about 186 blocks today. This can change with updates.", B),
                   ("Not enough space: 186 blocks needed, 174 free (12 short).", ER), ("Nothing was changed. Free space with a card manager.", ER)]
        else:
            ls += [("Slot A: 212 free blocks", B), ("Multishine needs about 186 blocks today. This can change with updates.", B), ("Writing...", B),
                   ("Done. The 5 Multishine files (150 blocks) are on the card", B), ("in slot A, with the standard Melee save.", B)]
        for k, (t_, c_) in enumerate(ls): text(im, 25, 80 + 20 * k, t_, c_)
        footer(im, [("B", "Back")])
        return im

    onboarding = {
        "about": d.copy(),
        "reset_confirm": reset_shot(0),
        "reset_confirm_remove": reset_shot(1),
        "reset_hold": reset_shot(1, 600),
        "reset_removing": reset_shot(1, working=True),
        "reset_done": reset_shot(1, done=True),
        "reset_failed": reset_shot(1, line="Could not remove saves (-9)."),
        "start_after_reset": start_shot("setup"),
        "start_card_exists": start_shot("launch"),
        "start_disc_in_drive": start_shot("drive"),
        "start_no_card": start_shot("setup"),
        "start_files_missing": start_shot("nofiles"),
        "start_two_devices": start_shot("launch", pill=True, dev=0),
        "start_no_stock_iso": start_shot("noiso"),
        "start_no_iso_setup": start_shot("noiso_setup"),
        "start_no_iso_two_devices": start_shot("noiso", pill=True, dev=0),
        "browse_root": browser_shot("sd:/", [("Backups", None), ("Documents", None), ("games", None), ("isos", None), ("Photos", None), ("Brawl.iso", "7889 MB"), ("melee v1.02.iso", "1392 MB"), ("test.gcm", "12 MB")], 3, 8),
        "browse_folder": browser_shot("sd:/isos/gamecube", [("Animal Crossing.iso", "1362 MB"), ("Kirby Air Ride.iso", "1362 MB"), ("Melee (backup).iso", "1392 MB"), ("Melee 20XX.iso", "1392 MB"), ("Melee.iso", "1392 MB"),
                                    ("Metroid Prime.iso", "1361 MB"), ("Super Mario Sunshine.iso", "1362 MB"), ("Wind Waker.iso", "1362 MB")], 4, 23),
        "browse_refused": browser_shot("sd:/isos/gamecube", [("Animal Crossing.iso", "1362 MB"), ("Kirby Air Ride.iso", "1362 MB"), ("Melee (backup).iso", "1392 MB"), ("Melee 20XX.iso", "1392 MB"), ("Melee.iso", "1392 MB"),
                                    ("Metroid Prime.iso", "1361 MB"), ("Super Mario Sunshine.iso", "1362 MB"), ("Wind Waker.iso", "1362 MB")], 3, 23,
                                    "Not stock Melee v1.02: data window 5 differs", True, 3, "bad", [("B", "Up"), ("A", "Select"), ("Left/Right", "Page"), ("Y", "USB")]),
        "browse_checking": (lambda im: (ctext(im, 200, "Checking the disc...", B), footer(im, [("B", "Stop")]), im)[2])((lambda t: (title(t, "Browse for a game"), t)[1])(bg.copy())),
        "drive_prompt": (lambda im: (ctext(im, 200, "Put your Melee disc in the drive", B), footer(im, [("B", "Back"), ("A", "Continue")]), im)[2])((lambda t: (title(t, "Use the disc drive"), t)[1])(bg.copy())),
        "drive_no_disc": (lambda im: (ctext(im, 200, "No disc found in the drive", ER), footer(im, [("B", "Back")]), im)[2])((lambda t: (title(t, "Use the disc drive"), t)[1])(bg.copy())),
        "setup_progress": setup_shot(["done", "done", "run", "todo"], "Looking for the Multishine card files..."),
        "setup_building": setup_shot(["done", "done", "done", "run"], "Building the Multishine system files:  62 %"),
        "setup_network_error": setup_shot(["fail", "todo", "todo", "todo"], "No network. Check the cable or the Wi-Fi of the console.", ER, [("A", "Retry"), ("B", "Back")]),
        "setup_no_iso": setup_shot(["done", "fail", "todo", "todo"], "Found Super Smash Bros. Melee.iso, not a stock v1.02 image (1392 MB). Go back, then X: Browse.", ER, [("A", "Retry"), ("B", "Back")],
                                   route="No memory card found: using a virtual card", info=VINFO),
        "setup_done": setup_shot(["done", "done", "done", "done"], "Multishine is ready.", OK, [("A", "Launch"), ("B", "Back")], route="No memory card found: using a virtual card", info=VINFO),
        # ---- the memory card route (ms_cardroute.h) ----
        "setup_ask_card": ask_shot(),
        "setup_route_nocard": setup_shot(["run", "todo", "todo", "todo"], "", route="No memory card found: using a virtual card", info=VINFO),
        "setup_route_short": setup_shot(["run", "todo", "todo", "todo"], "", route="The card in slot A is 12 blocks short: using a virtual card", info=VINFO),
        "setup_route_unformatted": setup_shot(["run", "todo", "todo", "todo"], "", route="The card in slot A is not formatted: using a virtual card", info=VINFO),
        "setup_route_wiiu": setup_shot(["run", "todo", "todo", "todo"], "", route="No memory card ports here: using a virtual card", info=VINFO),
        "setup_phys_progress": setup_shot(["done", "done", "run", "todo"], "Looking for the Multishine card files...", route="Using the memory card in slot A",
                                          info="Slot A: 212 free blocks. About 186 needed today (can change with updates).", phys=True),
        "physical_card": phys_shot(),
        "physical_card_short": phys_shot(short=True),
        "memory_card": memcard_shot(),
        "memory_card_unformatted": memcard_shot("Slot A: card not formatted", "Slot B: 40 free blocks"),
        "start_no_iso_wrong_size": start_shot("noiso_setup", noiso_line="Found Super Smash Bros. Melee.iso, not a stock v1.02 image (1392 MB). X: Browse for another."),
    }
    shots += list(onboarding.values())
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        for nm, im in onboarding.items():
            im.save(os.path.join(outdir, nm + ".png"))
    cols = 3
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (W * cols + 8 * (cols - 1), H * rows + 8 * (rows - 1)), (40, 40, 40))
    for i, im in enumerate(shots):
        guides(im)
        sheet.paste(im, ((i % cols) * (W + 8), (i // cols) * (H + 8)))
    sheet.save(out)
    return out


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--preview":
        print(preview(sys.argv[2]))
    elif len(sys.argv) == 4 and sys.argv[1] == "--preview":
        print(preview(sys.argv[2], sys.argv[3]))      # the sheet, plus one PNG per start / set-up screen in the folder
    elif len(sys.argv) == 2:
        write(sys.argv[1])
    else:
        raise SystemExit(__doc__)
