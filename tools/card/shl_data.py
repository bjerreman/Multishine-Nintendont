#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""shl_data: build, check and sign the NINTENDONT SHELL container, the OTA kind shl-N.mscf (MSCF ABI 6).

The Multishine Nintendont app on a Wii is three layers:  STUB (apps/Nintendont-Multishine/boot.dol, copied by hand ONCE, never overwritten)  ->  SHELL (the Nintendont loader:
screens, SD / network helpers, the kernel updater; an OTA file SD:/multishine/shell.dol, or the copy embedded in the stub)  ->  KERNEL (krn-N.mscf, tools/card/krn_data.py).
This container carries a SHELL: one PowerPC DOL (the loader image, the embedded fallback kernel included).  The stub verifies it with the pinned release keys, chain-loads it, and
falls back to its embedded shell when it does not verify or never reaches the loader screen.  Only our own code (Nintendont is GPL-2.0); never a Nintendo byte.

  shl_data.py build --dol shell.dol --seq N --out OUT.gci [--stub-abi 1]    the UNSIGNED container; sign with mscf_sign.py sign, publish with the publish tool (outside this repository) (kind shl)
  shl_data.py info FILE                                                      describe a container (.gci or served file)
  shl_data.py dolcheck FILE.dol                                              the DOL rules alone

Payload (big endian, plain, no segments / patches / compression):
  +0 'MSHL'  +4 u8 version 1  +5 u8 flags 0  +6 u16 0  +8 u32 total payload length (32 + dol length)  +12 u32 dol length  +16 u32 CRC-32 of the DOL bytes
  +20 u32 stub ABI the shell needs (1)  +24 u32 0  +28 u32 0  +32 the DOL file
DOL rules (the SAME rules the stub applies before it jumps, offline/nintendont-vbba/ota/ms_ota_core.c msdol_check): at most 3 MiB; 18 section entries; every section is inside the
file, 4-byte aligned in file offset, destination and size, and lands in MEM1 between 0x80003100 and 0x81300000 (the stub's trampoline and the Homebrew Channel arguments live from 0x81300000 up, and the Homebrew
Channel's reload stub below 0x80003100 stays intact); sections and bss do not overlap each other; the entry point is inside a text section.  stdlib only."""
import argparse, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
sys.path.insert(0, os.path.join(REPO, "offline", "provision"))

SHL_ABI = 6
BLOCK, HDR_OFF = 0x2000, 0x1000
GCI_HDR = 0x40
MAX_DOL = 3 * 1024 * 1024
MAX_FILE = (MAX_DOL + 32 + 2 * BLOCK + BLOCK - 1) // BLOCK * BLOCK
MAGIC = b"MSHL"
FILE_NAME = "MultishineShell"
STUB_ABI = 1
DST_LO, DST_HI = 0x80003100, 0x81300000


class ShlError(Exception):
    pass


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def dol_check(d):
    """-> None if the DOL passes the stub's rules, else the reason (a short string)"""
    if len(d) < 0x100 or len(d) > MAX_DOL:
        return "size"
    off = struct.unpack_from(">18I", d, 0)
    addr = struct.unpack_from(">18I", d, 0x48)
    size = struct.unpack_from(">18I", d, 0x90)
    bss, bsz, entry = struct.unpack_from(">III", d, 0xD8)
    ranges, text = [], False
    entry_ok = False
    for i in range(18):
        if size[i] == 0:
            continue
        if off[i] < 0x100 or off[i] & 3 or addr[i] & 3 or size[i] & 3 or off[i] + size[i] > len(d):
            return "section %d" % i
        if addr[i] < DST_LO or addr[i] + size[i] > DST_HI:
            return "section %d address" % i
        ranges.append((addr[i], addr[i] + size[i]))
        if i < 7:
            text = True
            if addr[i] <= entry < addr[i] + size[i]:
                entry_ok = True
    if bsz:
        if bss & 3 or bsz & 3 or bss < DST_LO or bss + bsz > DST_HI:
            return "bss"
        ranges.append((bss, bss + bsz))
    if not text or not entry_ok:
        return "entry"
    ranges.sort()
    for a, b in zip(ranges, ranges[1:]):
        if a[1] > b[0]:
            return "overlap"
    return None


def build_payload(dol, stub_abi=STUB_ABI):
    why = dol_check(dol)
    if why:
        raise ShlError("the DOL fails the stub's rules: %s" % why)
    return MAGIC + struct.pack(">BBHIIIIII", 1, 0, 0, 32 + len(dol), len(dol), crc32(dol), stub_abi, 0, 0) + dol


def parse_payload(p):
    """-> (dol bytes, stub abi); raises ShlError.  Same rules as the stub (ms_ota_core.c msota_check_shell)."""
    if len(p) < 33 or p[:4] != MAGIC or p[4] != 1 or p[5] != 0 or p[6:8] != b"\0\0":
        raise ShlError("header")
    total, dlen, dcrc, sabi, r1, r2 = struct.unpack_from(">IIIIII", p, 8)
    if total != len(p) or dlen != len(p) - 32 or dlen == 0 or dlen > MAX_DOL or r1 or r2:
        raise ShlError("length")
    d = p[32:]
    if crc32(d) != dcrc:
        raise ShlError("dol CRC")
    why = dol_check(d)
    if why:
        raise ShlError("dol: %s" % why)
    return d, sabi


def build_gci(payload, seq):
    import multishine_provision as P
    import mscf as M
    if not 1 <= seq <= 0xFFFFFFFF:
        raise ShlError("seq out of range")
    pay = payload + bytes(-len(payload) % BLOCK)
    h = bytearray(0x80)
    struct.pack_into(">IHHIIIIIIIII", h, 0, M.MAGIC, M.VERSION, SHL_ABI, seq, 0, 0, BLOCK, len(payload), crc32(payload), 0, 0, 0)
    struct.pack_into(">I", h, 0x2C, crc32(bytes(h[:0x2C])))
    blk = bytearray(BLOCK)
    blk[0:32] = b"Multishine".ljust(32, b"\0")
    blk[32:64] = b"Nintendont shell".ljust(32, b"\0")
    P.put_icon(blk, FILE_NAME)
    blk[HDR_OFF:HDR_OFF + 0x80] = h
    data = bytes(blk) + pay
    if len(data) > MAX_FILE:
        raise ShlError("container %d bytes is more than %d" % (len(data), MAX_FILE))
    return P.make_dir_entry(FILE_NAME, 0, len(data) // BLOCK, P.PERM_PUBLIC, None) + data


def guard(src):
    """src: a .gci (with the 0x40 entry) or a served file.  -> (ok, message).  A plain ABI 6 container, no segments / patches / flags, payload parses, every CRC holds, the DOL passes
    the stub's rules, nothing after it.  Run by the publish tool (outside this repository) publish (kind shl) and by the update catalog."""
    import mscf as M
    b = bytes(src)
    if len(b) % BLOCK == GCI_HDR:
        b = b[GCI_HDR:]
    if len(b) < 2 * BLOCK or len(b) % BLOCK or len(b) > MAX_FILE:
        return False, "REFUSED: a shell container is 2..%d blocks (got %d bytes)" % (MAX_FILE // BLOCK, len(b))
    h = b[HDR_OFF:HDR_OFF + 0x80]
    magic, ver, abi, seq, flags, base, size, ilen, icrc, nseg, npatch, entry = struct.unpack_from(">IHHIIIIIIIII", h, 0)
    if magic != M.MAGIC or ver != M.VERSION or abi != SHL_ABI or flags or nseg or npatch:
        return False, "REFUSED: not a plain ABI 6 container (version %d abi %d flags 0x%X segments %d patches %d)" % (ver, abi, flags, nseg, npatch)
    if not 0 < ilen <= len(b) - BLOCK or crc32(b[BLOCK:BLOCK + ilen]) != icrc:
        return False, "REFUSED: payload length or CRC"
    try:
        d, sabi = parse_payload(b[BLOCK:BLOCK + ilen])
        if sabi != STUB_ABI:
            return False, "REFUSED: needs stub ABI %d (this tool knows %d)" % (sabi, STUB_ABI)
    except ShlError as e:
        return False, "REFUSED: %s" % e
    if any(b[BLOCK + ilen:]):
        return False, "REFUSED: bytes after the payload"
    return True, "ok: shell %d: dol %d B" % (seq, len(d))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--dol", required=True)
    b.add_argument("--seq", type=int, required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--stub-abi", type=int, default=STUB_ABI)
    i = sub.add_parser("info")
    i.add_argument("file")
    c = sub.add_parser("dolcheck")
    c.add_argument("file")
    a = ap.parse_args(argv)
    if a.cmd == "dolcheck":
        why = dol_check(open(a.file, "rb").read())
        print("ok" if not why else "REFUSED: " + why)
        return 0 if not why else 1
    if a.cmd == "build":
        try:
            gci = build_gci(build_payload(open(a.dol, "rb").read(), a.stub_abi), a.seq)
        except ShlError as e:
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
