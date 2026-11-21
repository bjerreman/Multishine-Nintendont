#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""crd_data: build, check and sign the MEMORY CARD container, the OTA kind crd-N.mscf (MSCF ABI 8; ABI 7, the old four-file card, is no longer built, served or accepted).

The Multishine Nintendont shell on a Wii can seed Multishine onto a card: a NEW virtual card (SD:/multishine/saves/GALE.raw) or a physical GameCube card in slot A / B.  Both need the
same five files, a COMPLETE card: the loader save (the Name Entry exploit save with OUR banner, icon and digest), MultishineCode, MultishineOvl,
MultishineState and the standard Melee save MultishineMelee (tools/card/standard_save.py: our art, game-state body, 11 blocks), as standard .gci files (0x40-byte directory entry + the
file's blocks).  Nothing else: no account, no identity, no Slippi login, no Nintendo byte (the loader save and the Melee save carry our own art).  The card is the tester card
 plus the standard Melee save (standard_save.py apply).  The system files (MultishinePack) are NOT on it: the first start
builds them from the player's own disc.  The shell downloads this container like the kernel and the shell, checks it with the pinned
release keys and a monotonic floor, and builds the raw card or writes the files itself.

  crd_data.py build --card CARD.raw --seq N --out OUT.gci     the UNSIGNED container from a finished 5-file card; sign with mscf_sign.py sign
  crd_data.py info FILE                                       describe a container (.gci or served file)

Payload (big endian, plain, no segments / patches / compression):
  +0 'MCRD'  +4 u8 version 1  +5 u8 flags 0  +6 u16 file count (5)  +8 u32 total payload length  +12 u32 0  +16 u32 CRC-32 of everything from +32  +20 u32 shell ABI the card needs (1)
  +24 u32 0  +28 u32 0  +32 for each file: u32 length of the .gci, then the .gci bytes (0x40 + blocks * 0x2000)
Rules (the shell applies the same): exactly the five known file names, each once, game code GALE, maker 01, the block count in the directory entry matches the length, the loader save and
the Melee save are 11 blocks each, the whole set is at most 161 blocks (150 today), nothing after the files.  stdlib only."""
import argparse, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
sys.path.insert(0, os.path.join(REPO, "offline", "provision"))

CRD_ABI = 8
BLOCK, HDR_OFF = 0x2000, 0x1000
GCI_HDR = 0x40
MAGIC = b"MCRD"
FILE_NAME = "MultishineCard"
SHELL_ABI = 1
NAMES = ("SuperSmashBros0110290334", "MultishineCode", "MultishineOvl", "MultishineState", "MultishineMelee")
LOADER_BLOCKS = 11
MELEE_BLOCKS = 11
MAX_BLOCKS = 161
MAX_PAYLOAD = 32 + 5 * 4 + 5 * GCI_HDR + MAX_BLOCKS * BLOCK
MAX_FILE = (MAX_PAYLOAD + 2 * BLOCK + BLOCK - 1) // BLOCK * BLOCK


class CrdError(Exception):
    pass


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def check_gci(g):
    """-> (name, blocks) for a well-formed .gci of ours; raises CrdError"""
    if len(g) < GCI_HDR + BLOCK or (len(g) - GCI_HDR) % BLOCK:
        raise CrdError("gci length")
    e = g[:GCI_HDR]
    name = e[8:40].split(b"\0")[0].decode("ascii", "replace")
    blocks = struct.unpack_from(">H", e, 0x38)[0]
    if e[0:4] != b"GALE" or e[4:6] != b"01" or name not in NAMES:
        raise CrdError("not one of our files: %r" % name)
    if blocks * BLOCK != len(g) - GCI_HDR or blocks == 0:
        raise CrdError("%s: directory says %d blocks, data is %d" % (name, blocks, (len(g) - GCI_HDR) // BLOCK))
    if name == NAMES[0] and blocks != LOADER_BLOCKS:
        raise CrdError("the loader save must be %d blocks" % LOADER_BLOCKS)
    if name == NAMES[4] and blocks != MELEE_BLOCKS:
        raise CrdError("the Melee save must be %d blocks" % MELEE_BLOCKS)
    return name, blocks


def build_payload(gcis):
    seen, total = set(), 0
    for g in gcis:
        n, b = check_gci(g)
        if n in seen:
            raise CrdError("%s twice" % n)
        seen.add(n)
        total += b
    if seen != set(NAMES):
        raise CrdError("the set must be exactly %s" % ", ".join(NAMES))
    if total > MAX_BLOCKS:
        raise CrdError("%d blocks is more than %d" % (total, MAX_BLOCKS))
    body = b"".join(struct.pack(">I", len(g)) + g for g in gcis)
    return MAGIC + struct.pack(">BBHIIIIII", 1, 0, len(gcis), 32 + len(body), 0, crc32(body), SHELL_ABI, 0, 0) + body


def parse_payload(p):
    """-> list of .gci bytes; raises CrdError.  Same rules as the shell (ms_card_core.c mscrd_parse)."""
    if len(p) < 33 or p[:4] != MAGIC or p[4] != 1 or p[5] != 0:
        raise CrdError("header")
    cnt = struct.unpack_from(">H", p, 6)[0]
    total, r0, bcrc, sabi, r1, r2 = struct.unpack_from(">IIIIII", p, 8)
    if total != len(p) or cnt != 5 or r0 or r1 or r2 or sabi > SHELL_ABI:
        raise CrdError("length or fields")
    if crc32(p[32:]) != bcrc:
        raise CrdError("CRC")
    out, off = [], 32
    for _ in range(cnt):
        if off + 4 > len(p):
            raise CrdError("truncated")
        n = struct.unpack_from(">I", p, off)[0]
        off += 4
        if n > len(p) - off:
            raise CrdError("truncated file")
        out.append(p[off:off + n])
        off += n
    if off != len(p):
        raise CrdError("bytes after the files")
    build_payload(out)                               # the file rules
    return out


def build_gci(payload, seq):
    import multishine_provision as P
    import mscf as M
    if not 1 <= seq <= 0xFFFFFFFF:
        raise CrdError("seq out of range")
    pay = payload + bytes(-len(payload) % BLOCK)
    h = bytearray(0x80)
    struct.pack_into(">IHHIIIIIIIII", h, 0, M.MAGIC, M.VERSION, CRD_ABI, seq, 0, 0, BLOCK, len(payload), crc32(payload), 0, 0, 0)
    struct.pack_into(">I", h, 0x2C, crc32(bytes(h[:0x2C])))
    blk = bytearray(BLOCK)
    blk[0:32] = b"Multishine".ljust(32, b"\0")
    blk[32:64] = b"Memory card files".ljust(32, b"\0")
    P.put_icon(blk, FILE_NAME)
    blk[HDR_OFF:HDR_OFF + 0x80] = h
    data = bytes(blk) + pay
    if len(data) > MAX_FILE:
        raise CrdError("container %d bytes is more than %d" % (len(data), MAX_FILE))
    return P.make_dir_entry(FILE_NAME, 0, len(data) // BLOCK, P.PERM_PUBLIC, None) + data


def gcis_from_card(card_raw):
    """the five .gci files of a finished card"""
    sys.path.insert(0, os.path.join(REPO, "tools", "vmc_builder"))
    import vmc_card as C
    import vmc_gci as G
    card = C.Card(card_raw)
    out = {}
    for e in card.entries():
        if e["name"] in NAMES:
            out[e["name"]] = G.export_gci(card, e)
    if set(out) != set(NAMES):
        raise CrdError("the card does not hold exactly our five files (found %s; the standard Melee save: tools/card/standard_save.py apply)" % ", ".join(sorted(out)))
    return [out[n] for n in NAMES]


def guard(src):
    """src: a .gci (with the 0x40 entry) or a served file.  -> (ok, message).  Run by the publish tool (outside this repository) publish (kind crd) and by the update catalog."""
    import mscf as M
    b = bytes(src)
    if len(b) % BLOCK == GCI_HDR:
        b = b[GCI_HDR:]
    if len(b) < 2 * BLOCK or len(b) % BLOCK or len(b) > MAX_FILE:
        return False, "REFUSED: a card container is 2..%d blocks (got %d bytes)" % (MAX_FILE // BLOCK, len(b))
    h = b[HDR_OFF:HDR_OFF + 0x80]
    magic, ver, abi, seq, flags, base, size, ilen, icrc, nseg, npatch, entry = struct.unpack_from(">IHHIIIIIIIII", h, 0)
    if magic != M.MAGIC or ver != M.VERSION or abi != CRD_ABI or flags or nseg or npatch:
        return False, "REFUSED: not a plain ABI 8 container (version %d abi %d flags 0x%X segments %d patches %d)" % (ver, abi, flags, nseg, npatch)
    if not 0 < ilen <= len(b) - BLOCK or crc32(b[BLOCK:BLOCK + ilen]) != icrc:
        return False, "REFUSED: payload length or CRC"
    try:
        files = parse_payload(b[BLOCK:BLOCK + ilen])
    except CrdError as e:
        return False, "REFUSED: %s" % e
    if any(b[BLOCK + ilen:]):
        return False, "REFUSED: bytes after the payload"
    return True, "ok: card %d: %d files, %d blocks" % (seq, len(files), sum(check_gci(g)[1] for g in files))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--card", required=True)
    b.add_argument("--seq", type=int, required=True)
    b.add_argument("--out", required=True)
    i = sub.add_parser("info")
    i.add_argument("file")
    a = ap.parse_args(argv)
    if a.cmd == "build":
        try:
            gci = build_gci(build_payload(gcis_from_card(open(a.card, "rb").read())), a.seq)
        except CrdError as e:
            raise SystemExit("REFUSED: %s" % e)
        ok, msg = guard(gci)
        if not ok:
            raise SystemExit(msg)
        open(a.out, "wb").write(gci)
        print(msg)
        return 0
    ok, msg = guard(open(a.file, "rb").read())
    print(msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
