#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""MSCF packer: builds the card file "MultishineCode".

  mscf.py gecko CODESET.txt --out code.gci [--hole-base 0x...] [--retail-iso ISO] [--seq N] [--mtime T]
      04 / C2 Gecko ops -> patches + C2 bodies in the hole image (same semantics as Slippi's bootloader and the project's other tooling,
      but at runtime on the card route: the bodies live in the claimed hole, not in a DOL section).
  mscf.py info code.gci          print the header
Library use: build_image(...) / build_gci(...).  stdlib only."""
import argparse, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "offline", "provision"))
import multishine_provision as P  # noqa: E402
sys.path.insert(0, HERE)
import mscf_z as Z  # noqa: E402

MAGIC, VERSION, ABI = 0x4D534346, 1, 1
VERSION_Z = 2        # header layout of a COMPRESSED code file (adds z_len/z_crc at 0x38/0x3C and F_COMP); uncompressed files stay version 1, byte for byte
BLOCK, HDR_OFF, PAYLOAD_OFF = 0x2000, 0x1000, 0x2000
LAYOUT = os.environ.get("CL_LAYOUT", "")      # "generic": Cubeboot generic-VMC-safe layout (cl/mscf.h)
LIST_BASE = {"generic": 0x817E0000, "low": 0x817C0000}.get(LAYOUT, 0x817F4000)    # "low": list 0x817C0000 (far below any Cubeboot reduction)
HOLE_MIN, HOLE_MAX, WA_SIZE = 0x81700000, (LIST_BASE if LAYOUT in ("generic", "low") else 0x817F0000), 0xA000
sys.path.insert(0, os.path.join(REPO, "offline"))
import projname  # noqa: E402
import card_text  # noqa: E402
FILE_NAME = projname.SHORT + "Code"          # the loader (cl_loader.c CL_FILE_NAME) is built with the same name
F_CLAIM, F_ENTRY, F_COMP = 1, 2, 4


def crc32(data, crc=0):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def enc_b(src, dst):
    off = dst - src
    assert -0x2000000 <= off <= 0x1FFFFFC and off % 4 == 0 and src % 4 == 0
    return 0x48000000 | (off & 0x03FFFFFC)


def parse_gecko(path):
    words = []
    for raw in open(path):
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("$"):
            continue
        a, b = line.split()
        words.append((int(a, 16), int(b, 16)))
    return words


def gecko_to_image(words, hole_base):
    """-> (image bytes, [(addr, word)] patches).  04: word write.  C2: body (32-byte aligned) in the image, hook `b body`, last payload word `b hook+4`."""
    img, patches, i = bytearray(), [], 0
    while i < len(words):
        w0, w1 = words[i]
        op, addr = (w0 >> 24) & 0xFE, 0x80000000 | (w0 & 0x01FFFFFF)
        if op == 0x04:
            patches.append((addr, w1)); i += 1
        elif op == 0xC2:
            n = w1
            payload = [x for ln in words[i + 1:i + 1 + n] for x in ln]
            if len(payload) != 2 * n:
                raise ValueError("truncated C2 at line %d" % i)
            while len(img) % 32:
                img.append(0)
            body = hole_base + len(img)
            payload[-1] = enc_b(body + 4 * (len(payload) - 1), addr + 4)
            patches.append((addr, enc_b(addr, body)))
            img += b"".join(struct.pack(">I", x) for x in payload)
            i += 1 + n
        else:
            raise ValueError("unsupported Gecko op %08X" % w0)
    return bytes(img), patches


def build_image_header(image, hole_base, hole_size, patches, segs=(), flags=F_CLAIM, entry=0, seq=1, guard_words=None, z=None):
    # patches: the first len(guard_words) are file-backed (guarded by the CRC of their retail words), the rest are .bss stores
    """-> block 0 bytes (header block).  z = the MSLZ stream of the image (compressed file: version 2, F_COMP, z_len/z_crc at 0x38/0x3C)."""
    if hole_base % 32 or hole_base < HOLE_MIN or hole_base + hole_size > HOLE_MAX - WA_SIZE - BLOCK:
        raise ValueError("hole 0x%08X+0x%X outside [0x%08X, 0x%08X)" % (hole_base, hole_size, HOLE_MIN, HOLE_MAX - WA_SIZE - BLOCK))
    if (len(image) + BLOCK - 1) // BLOCK * BLOCK > hole_size:
        raise ValueError("image %d bytes does not fit the hole (%d bytes)" % (len(image), hole_size))
    tabs = b"".join(struct.pack(">IIII", *s) for s in segs) + b"".join(struct.pack(">II", a, w) for a, w in patches)
    if 0x80 + len(tabs) > BLOCK - HDR_OFF:
        raise ValueError("tables do not fit block 0 (%d bytes)" % len(tabs))
    guard = 0
    if guard_words is not None:
        g = 0
        for w in guard_words:
            g = crc32(struct.pack(">I", w), g)
        guard = g or 1
    h = bytearray(0x80)
    if z is not None:
        flags |= F_COMP
    struct.pack_into(">IHHIIIIIIIII", h, 0, MAGIC, VERSION if z is None else VERSION_Z, ABI, seq, flags, hole_base, hole_size, len(image), crc32(image), len(segs), len(patches), entry)
    struct.pack_into(">II", h, 0x30, guard, len(guard_words) if guard_words is not None else 0)
    if z is not None:
        struct.pack_into(">II", h, 0x38, len(z), crc32(z))
    c = crc32(bytes(h[:0x2C]))
    c = crc32(tabs, c)
    struct.pack_into(">I", h, 0x2C, c)
    blk = bytearray(BLOCK)
    card_text.put(blk, "Code")                    # offline/card_text.py: the one table of comment lines
    P.put_icon(blk, FILE_NAME)
    blk[HDR_OFF:HDR_OFF + len(h)] = h
    blk[HDR_OFF + 0x80:HDR_OFF + 0x80 + len(tabs)] = tabs
    return bytes(blk)


def build_gci(image, hole_base, hole_size, patches, mtime=None, compress=False, **kw):
    """compress=True: the payload is the MSLZ stream of the image, to be read to the tail of the hole and decoded in place (mscf_z.py); the hole must be big
    enough for the in-place margin (ValueError names the size), otherwise the layout is unchanged."""
    z = None
    if compress:
        z, gap, zpad, minsize = Z.plan(image, hole_size)
        if hole_size < minsize:
            raise ValueError("compressed image needs hole_size >= 0x%X (in-place margin %d, stream %d bytes), got 0x%X" % (minsize, gap, len(z), hole_size))
    blk0 = build_image_header(image, hole_base, hole_size, patches, z=z, **kw)
    pay = (z if z is not None else bytes(image)) + b"\0" * (-(len(z) if z is not None else len(image)) % BLOCK)
    data = blk0 + pay
    ent = P.make_dir_entry(FILE_NAME, 0, len(data) // BLOCK, P.PERM_PUBLIC, mtime)
    return ent + data


def retail_words_from_dol(d, addrs):
    sys.path.insert(0, HERE)
    import dol_diff as D
    secs = D.sections(d)
    out = {}
    for a in addrs:
        for s in secs:
            if s["addr"] <= a < s["addr"] + s["size"]:
                o = s["off"] + a - s["addr"]
                out[a] = struct.unpack(">I", d[o:o + 4])[0]; break
    return out


def retail_words(iso, addrs):
    """words of the retail main.dol at addrs (build host only; nothing is stored in the card file except their CRC)."""
    sys.path.insert(0, HERE)
    import dol_diff as D
    d = D.dol_from_iso(iso)
    secs = D.sections(d)
    out = {}
    for a in addrs:
        for s in secs:
            if s["addr"] <= a < s["addr"] + s["size"]:
                o = s["off"] + a - s["addr"]
                out[a] = struct.unpack(">I", d[o:o + 4])[0]; break
    return out      # addresses outside every file-backed section (.bss) are absent


def info(path):
    b = open(path, "rb").read()
    d = b[0x40:]
    h = d[HDR_OFF:HDR_OFF + 0x80]
    f = struct.unpack_from(">IHHIIIIIIIII", h, 0)
    names = "magic version abi seq flags hole_base hole_size img_len img_crc nseg npatch entry".split()
    for n, v in zip(names, f):
        print("%-10s 0x%X" % (n, v))
    if f[4] & F_COMP:
        zl, zc = struct.unpack_from(">II", h, 0x38)
        print("z_len      0x%X (%d)  z_crc 0x%X  stream at hole_base + 0x%X" % (zl, zl, zc, f[6] - (zl + BLOCK - 1) // BLOCK * BLOCK))
    print("hdr_crc    0x%X guard 0x%X" % (struct.unpack_from(">I", h, 0x2C)[0], struct.unpack_from(">I", h, 0x30)[0]))
    print("file blocks", len(d) // BLOCK)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gecko"); g.add_argument("codeset"); g.add_argument("--out", required=True)
    g.add_argument("--hole-base", type=lambda x: int(x, 0)); g.add_argument("--hole-size", type=lambda x: int(x, 0))
    g.add_argument("--retail-iso"); g.add_argument("--seq", type=int, default=1); g.add_argument("--mtime", type=int); g.add_argument("--compress", action="store_true")
    i = sp.add_parser("info"); i.add_argument("file")
    a = ap.parse_args()
    if a.cmd == "info":
        return info(a.file)
    words = parse_gecko(a.codeset)
    # two passes: size the image with a dummy base, then place the hole so that it ends at HOLE_MAX - WA - BLOCK (top of AllM stays untouched)
    img0, _ = gecko_to_image(words, HOLE_MIN)
    size = a.hole_size or ((len(img0) + BLOCK - 1) // BLOCK * BLOCK + BLOCK)
    top = HOLE_MAX - WA_SIZE - BLOCK
    base = a.hole_base or (top - size) & ~31
    img, patches = gecko_to_image(words, base)
    guard = None
    if a.retail_iso:
        rw = retail_words(a.retail_iso, [p[0] for p in patches])
        patches = [p for p in patches if p[0] in rw] + [p for p in patches if p[0] not in rw]
        guard = [rw[p[0]] for p in patches if p[0] in rw]
    gci = build_gci(img, base, size, patches, mtime=a.mtime, seq=a.seq, guard_words=guard, compress=a.compress)
    open(a.out, "wb").write(gci)
    print("code file %s: %d bytes, image %d, hole 0x%08X+0x%X, %d patches, guard %s" % (a.out, len(gci), len(img), base, size, len(patches), "on" if guard else "off"))


if __name__ == "__main__":
    main()
