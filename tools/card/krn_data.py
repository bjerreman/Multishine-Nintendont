#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""krn_data: build, check and sign the NINTENDONT KERNEL container, the OTA kind krn-N.mscf (MSCF ABI 5).

The Multishine Nintendont app on a Wii is a stable SHELL (boot.dol, the loader) that carries one embedded kernel as the fallback.  A newer kernel arrives as this
container: the shell verifies it with the pinned release keys (the same ones as the engine's OTA), loads it instead of the embedded one, and falls back to the
embedded kernel when it does not verify or does not reach the game.  The container carries ONLY our own kernel image (Nintendont is GPL-2.0, the vBBA is ours);
never a Nintendo byte.

  krn_data.py build --kernel kernel.zip --seq N --out OUT.gci [--shell-abi 1]    the UNSIGNED container; sign with mscf_sign.py sign (--key / --dev), publish with the publish tool (outside this repository) (kind krn)
  krn_data.py info FILE                                                           describe a container (.gci or served file)

Payload (the image of the signed MSCF container, big endian, plain, no segments / patches / compression):
  +0 'MKRN'  +4 u8 version 1  +5 u8 flags 0  +6 u16 0  +8 u32 total payload length (32 + kernel length)  +12 u32 kernel length  +16 u32 CRC-32 of the kernel bytes
  +20 u32 shell ABI the kernel needs (1)  +24 u32 0  +28 u32 0  +32 kernel bytes: the ZIP file the loader embeds as kernel.zip (the loader unzips it to 0x92F00000)
The kernel image must contain the string 'RUNID: game' (the line the kernel writes to /ndebug.log once it reached the game): the shell confirms a downloaded kernel by it
(rollback after two starts without it), so a kernel built without the log (NIN_MS_LOG) is refused here.  stdlib only."""
import argparse, os, struct, sys, zlib, zipfile, io

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
sys.path.insert(0, os.path.join(REPO, "offline", "provision"))

KRN_ABI = 5
BLOCK, HDR_OFF = 0x2000, 0x1000
GCI_HDR = 0x40
MAX_KERNEL = 1536 * 1024          # the loader refuses more (ms_ota_core.h MSOTA_MAX_KERNEL); the real kernel zip is far smaller
MAX_FILE = (MAX_KERNEL + 32 + 2 * BLOCK + BLOCK - 1) // BLOCK * BLOCK
MAGIC = b"MKRN"
FILE_NAME = "MultishineKernel"
SHELL_ABI = 1
RUNID = b"RUNID: game"


class KrnError(Exception):
    pass


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def kernel_text(kernel_zip):
    """the decompressed kernel image (what the guard scans for the RUNID marker)"""
    try:
        z = zipfile.ZipFile(io.BytesIO(kernel_zip))
        names = z.namelist()
        if len(names) != 1:
            raise KrnError("the kernel zip must hold exactly one file (got %d)" % len(names))
        return z.read(names[0])
    except zipfile.BadZipFile as e:
        raise KrnError("not a zip file: %s" % e)


def build_payload(kernel_zip, shell_abi=SHELL_ABI, require_runid=True):
    if not kernel_zip or len(kernel_zip) > MAX_KERNEL:
        raise KrnError("kernel size out of range (1..%d)" % MAX_KERNEL)
    img = kernel_text(kernel_zip)
    if require_runid and RUNID not in img:
        raise KrnError("the kernel image has no 'RUNID: game' string: it was built without NIN_MS_LOG, the shell could not confirm it (see the header of this file)")
    total = 32 + len(kernel_zip)
    head = MAGIC + struct.pack(">BBHIIIIII", 1, 0, 0, total, len(kernel_zip), crc32(kernel_zip), shell_abi, 0, 0)
    return head + kernel_zip


def parse_payload(p):
    """-> (kernel zip bytes, shell abi); raises KrnError.  Same rules as the shell (ms_ota_core.c)."""
    if len(p) < 33 or p[:4] != MAGIC or p[4] != 1 or p[5] != 0 or p[6:8] != b"\0\0":
        raise KrnError("header")
    total, klen, kcrc, sabi, r1, r2 = struct.unpack_from(">IIIIII", p, 8)
    if total != len(p) or klen != len(p) - 32 or klen == 0 or klen > MAX_KERNEL or r1 or r2:
        raise KrnError("length")
    k = p[32:]
    if crc32(k) != kcrc:
        raise KrnError("kernel CRC")
    return k, sabi


def build_gci(payload, seq):
    """-> the UNSIGNED .gci: 0x40 directory entry + header block + payload blocks"""
    import multishine_provision as P
    import mscf as M
    if not 1 <= seq <= 0xFFFFFFFF:
        raise KrnError("seq out of range")
    pay = payload + bytes(-len(payload) % BLOCK)
    h = bytearray(0x80)
    struct.pack_into(">IHHIIIIIIIII", h, 0, M.MAGIC, M.VERSION, KRN_ABI, seq, 0, 0, BLOCK, len(payload), crc32(payload), 0, 0, 0)
    struct.pack_into(">I", h, 0x2C, crc32(bytes(h[:0x2C])))
    blk = bytearray(BLOCK)
    blk[0:32] = b"Multishine".ljust(32, b"\0")
    blk[32:64] = b"Nintendont kernel".ljust(32, b"\0")
    P.put_icon(blk, FILE_NAME)
    blk[HDR_OFF:HDR_OFF + 0x80] = h
    data = bytes(blk) + pay
    if len(data) > MAX_FILE:
        raise KrnError("container %d bytes is more than %d" % (len(data), MAX_FILE))
    return P.make_dir_entry(FILE_NAME, 0, len(data) // BLOCK, P.PERM_PUBLIC, None) + data


def guard(src):
    """src: a .gci (with the 0x40 entry) or a served file.  -> (ok, message).  A plain ABI 5 container, no segments / patches / flags, payload parses and every CRC holds, the kernel
    carries the RUNID marker, nothing after it.  Run by the publish tool (outside this repository) publish (kind krn) and by the update catalog."""
    import mscf as M
    b = bytes(src)
    if len(b) % BLOCK == GCI_HDR:
        b = b[GCI_HDR:]
    if len(b) < 2 * BLOCK or len(b) % BLOCK or len(b) > MAX_FILE:
        return False, "REFUSED: a kernel container is 2..%d blocks (got %d bytes)" % (MAX_FILE // BLOCK, len(b))
    h = b[HDR_OFF:HDR_OFF + 0x80]
    magic, ver, abi, seq, flags, base, size, ilen, icrc, nseg, npatch, entry = struct.unpack_from(">IHHIIIIIIIII", h, 0)
    if magic != M.MAGIC or ver != M.VERSION or abi != KRN_ABI or flags or nseg or npatch:
        return False, "REFUSED: not a plain ABI 5 container (version %d abi %d flags 0x%X segments %d patches %d)" % (ver, abi, flags, nseg, npatch)
    if not 0 < ilen <= len(b) - BLOCK or crc32(b[BLOCK:BLOCK + ilen]) != icrc:
        return False, "REFUSED: payload length or CRC"
    try:
        k, sabi = parse_payload(b[BLOCK:BLOCK + ilen])
        if sabi != SHELL_ABI:
            return False, "REFUSED: needs shell ABI %d (this tool knows %d)" % (sabi, SHELL_ABI)
        if RUNID not in kernel_text(k):
            return False, "REFUSED: the kernel has no RUNID marker"
    except KrnError as e:
        return False, "REFUSED: %s" % e
    if any(b[BLOCK + ilen:]):
        return False, "REFUSED: bytes after the payload"
    return True, "ok: kernel %d: zip %d B" % (seq, len(k))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--kernel", required=True, help="the kernel ZIP the loader embeds (loader/data/kernel.zip of the build tree)")
    b.add_argument("--seq", type=int, required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--shell-abi", type=int, default=SHELL_ABI)
    b.add_argument("--no-runid-guard", action="store_true", help="TEST ONLY: accept a kernel without the RUNID marker")
    i = sub.add_parser("info")
    i.add_argument("file")
    a = ap.parse_args(argv)
    if a.cmd == "build":
        try:
            gci = build_gci(build_payload(open(a.kernel, "rb").read(), a.shell_abi, not a.no_runid_guard), a.seq)
        except KrnError as e:
            raise SystemExit("REFUSED: %s" % e)
        ok, msg = guard(gci) if not a.no_runid_guard else (True, "ok (runid guard off)")
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
