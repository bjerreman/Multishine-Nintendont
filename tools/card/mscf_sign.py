#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""MSCF signing tool (OTA stage 0): sign / verify / bump the card code file "MultishineCode".

  mscf_sign.py keygen --out PATH                  new release signing key (32-byte seed, mode 0600, REFUSES a path inside a git work tree)
  mscf_sign.py pubkey (--key PATH | --dev)        64 hex chars
  mscf_sign.py sign IN.gci --out OUT.gci (--key PATH | --dev) [--seq N | --bump]
  mscf_sign.py verify FILE.gci (--pub HEX | --key PATH | --dev) [--min-seq N] [--abi N] [--allow-unsigned]
  mscf_sign.py info FILE.gci
  mscf_sign.py state --seq N --out state.gci      the optional state file MultishineState: floor record (seq; 0 = none) and, with --cand C [--tries T] [--good], the boot record of the
                                                  automatic rollback ( 4.5; the updater writes cand=S tries=0 at install, the engine counts and clears)

The signed message (identical to mscf_verify_sig() in offline/cardloader/cl/mscf_verify.h, used by the loader and the installer):
    uncompressed (MSCF v1):  "MSCFSIGv1\\0" || header[0x00..0x40) || SHA-512(tables) || SHA-512(image[0..image_len))
    compressed   (MSCF v2):  "MSCFSIGz1\\0" || header[0x00..0x40) || SHA-512(tables) || SHA-512(stored MSLZ stream[0..z_len))
A compressed file is signed over its STORED stream, so the loader verifies the signature BEFORE the decoder runs (no pre-authentication decoder); the header
(signed) holds image_len/image_crc/z_len/z_crc, so the decoded image is a deterministic, length- and CRC-checked function of signed bytes.  v1 is unchanged.
header = the MSCF header at file offset 0x1000+0x40 (the 64-byte signature slot 0x40..0x80 is excluded); tables = the segment and patch lists at 0x80.
So build_seq, flags, hole, entry, image CRC, header CRC, guard, every patch word and every image byte are signed; the comment/icon of block 0 are not.

--dev signs with the RFC 8032 test-vector-1 key.  Its private half is a PUBLISHED test vector: it is not a secret, a release loader never pins it
, and a file signed with it is dev-only by definition.  verify returns the loader's error name (E_SIG, E_SEQ, E_ABI, E_HDR, E_CRC, E_RANGE) or OK.
stdlib only; Ed25519 is tools/payload_pack/ed25519_ref.py (reference code: not constant time, sign on a trusted machine)."""
import argparse, hashlib, os, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
sys.path.insert(0, os.path.join(REPO, "tools", "payload_pack"))
import mscf as M          # noqa: E402
import mslz               # noqa: E402
import ed25519_ref as ed  # noqa: E402
sys.path.insert(0, os.path.join(REPO, "offline"))
import card_text as CT  # noqa: E402

TAG = b"MSCFSIGv1\0"
TAG_Z = b"MSCFSIGz1\0"      # compressed files (F_COMP): the stored stream is hashed, not the decoded image
H_SEQ, H_SIG, H_SEGS = 0x08, 0x40, 0x80
DEV_SEED = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")     # RFC 8032 section 7.1 TEST 1 (published)
DEV_PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
KEY_MAGIC = b"MSS1"
GCI_HDR = 0x40
E = dict(OK=0, E_HDR=3, E_RANGE=4, E_READ=5, E_CRC=6, E_SEQ=11, E_SIG=12, E_ABI=13, E_DECODE=14)


class Bad(Exception):
    def __init__(self, code, msg=""):
        super().__init__("%s %s" % (code, msg)); self.code = code


def in_git_tree(path):
    d = os.path.abspath(path)
    while not os.path.isdir(d):
        d = os.path.dirname(d)
    r = subprocess.run(["git", "-C", d, "rev-parse", "--is-inside-work-tree"], capture_output=True, text=True)
    return r.returncode == 0 and r.stdout.strip() == "true"


def keygen(path, allow_in_git=False):
    """New key file.  allow_in_git is for throwaway test keys only (the CLI never sets it)."""
    if not allow_in_git and in_git_tree(os.path.dirname(os.path.abspath(path))):
        raise SystemExit("refusing: the signing key must not live inside a git work tree")
    seed = os.urandom(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(KEY_MAGIC + seed)
    return ed.public_key(seed).hex()


def load_seed(path):
    b = open(path, "rb").read()
    if len(b) != 36 or b[:4] != KEY_MAGIC:
        raise SystemExit("not a signing key file")
    return b[4:]


def parse(gci):
    """-> dict with the header fields, the 0x40 signed header bytes, tables, image, sig."""
    if len(gci) < GCI_HDR + M.BLOCK or (len(gci) - GCI_HDR) % M.BLOCK:
        raise Bad("E_HDR", "size")
    d = gci[GCI_HDR:]
    h = d[M.HDR_OFF:M.HDR_OFF + 0x80]
    magic, ver, abi, seq, flags, base, size, ilen, icrc, nseg, npatch, entry = struct.unpack_from(">IHHIIIIIIIII", h, 0)
    if magic != M.MAGIC or ver not in (M.VERSION, M.VERSION_Z):
        raise Bad("E_HDR", "magic/version")
    comp = bool(flags & M.F_COMP)
    if comp != (ver == M.VERSION_Z):
        raise Bad("E_HDR", "version/flags")
    nt = nseg * 16 + npatch * 8
    if nseg > 64 or npatch > 400 or H_SEGS + nt > M.BLOCK - M.HDR_OFF:
        raise Bad("E_HDR", "table counts")
    tabs = d[M.HDR_OFF + H_SEGS:M.HDR_OFF + H_SEGS + nt]
    img = d[M.PAYLOAD_OFF:M.PAYLOAD_OFF + ilen]
    zlen = zcrc = 0
    if comp:      # compressed file: the signature covers the STORED stream; "img" is the decoded image, only for tests and checks (never signed)
        zlen, zcrc = struct.unpack_from(">II", h, 0x38)
        z = d[M.PAYLOAD_OFF:M.PAYLOAD_OFF + zlen]
        try:
            img = mslz.decompress(z, ilen) if len(z) == zlen and zlen else b""
        except mslz.LZError:
            img = b""
    return dict(zlen=zlen, zcrc=zcrc, zdata=d[M.PAYLOAD_OFF:M.PAYLOAD_OFF + zlen], comp=comp, d=d, h=h, abi=abi, seq=seq, flags=flags, base=base, size=size, ilen=ilen, icrc=icrc, nseg=nseg, npatch=npatch,
                entry=entry, tabs=tabs, img=img, sig=h[H_SIG:H_SIG + 64], hcrc=struct.unpack_from(">I", h, 0x2C)[0])


def signed_message(p):
    if p["comp"]:
        return TAG_Z + bytes(p["h"][:H_SIG]) + hashlib.sha512(p["tabs"]).digest() + hashlib.sha512(p["zdata"]).digest()
    return TAG + bytes(p["h"][:H_SIG]) + hashlib.sha512(p["tabs"]).digest() + hashlib.sha512(p["img"]).digest()


def hdr_crc(h, tabs):
    c = zlib.crc32(bytes(h[:0x2C])) & 0xFFFFFFFF
    return zlib.crc32(tabs, c) & 0xFFFFFFFF


def set_seq(gci, seq):
    """-> a copy with build_seq = seq and the header CRC recomputed (the signature, if any, is cleared: it no longer matches)."""
    p = parse(gci)
    off = GCI_HDR + M.HDR_OFF
    b = bytearray(gci)
    struct.pack_into(">I", b, off + H_SEQ, seq)
    h = bytes(b[off:off + 0x80])
    struct.pack_into(">I", b, off + 0x2C, hdr_crc(h, p["tabs"]))
    b[off + H_SIG:off + H_SIG + 64] = bytes(64)
    return bytes(b)


def sign(gci, seed, seq=None, bump=False):
    p = parse(gci)
    if seq is None:
        seq = p["seq"] + 1 if bump else p["seq"]
    if not 1 <= seq <= 0xFFFFFFFF:
        raise SystemExit("build_seq out of range")
    b = bytearray(set_seq(gci, seq))
    p = parse(bytes(b))
    p["h"] = bytes(b[GCI_HDR + M.HDR_OFF:GCI_HDR + M.HDR_OFF + 0x80])
    sig = ed.sign(seed, signed_message(p))
    off = GCI_HDR + M.HDR_OFF + H_SIG
    b[off:off + 64] = sig
    return bytes(b)


def check(gci, pubs, min_seq=1, abi=M.ABI, allow_unsigned=False):
    """The loader's decision, in the loader's order (cl_loader.c): returns 'OK' or an error name; 'OK-UNSIGNED' for an accepted unsigned dev file."""
    try:
        p = parse(gci)
    except Bad as e:
        return e.code
    if p["abi"] != abi:
        return "E_ABI"
    if p["seq"] < min_seq:
        return "E_SEQ"
    if hdr_crc(p["h"], p["tabs"]) != p["hcrc"]:
        return "E_HDR"
    if p["comp"]:
        # the loader's order for a compressed file: read the stream (a file shorter than z_len cannot be read), stream CRC, SIGNATURE, only then decode, image CRC
        if p["zlen"] == 0 or p["zlen"] > p["size"] or len(p["zdata"]) != p["zlen"]:
            return "E_RANGE" if p["zlen"] == 0 or p["zlen"] > p["size"] else "E_READ"
        if zlib.crc32(p["zdata"]) & 0xFFFFFFFF != p["zcrc"]:
            return "E_CRC"
        msg = signed_message(p)
        ok = any(ed.verify(pub, msg, p["sig"]) for pub in pubs)
        if not ok and not (allow_unsigned and p["sig"] == bytes(64)):
            return "E_SIG"
        if len(p["img"]) != p["ilen"] or p["ilen"] == 0:
            return "E_DECODE"
        if zlib.crc32(p["img"]) & 0xFFFFFFFF != p["icrc"]:
            return "E_CRC"
        # in-place margin (the loader does not compute it, it relies on the packer / the signer): a hole that is too small makes the decoder read bytes it
        # has already overwritten, so the decoder meets a damaged stream (E_DECODE, what Dolphin shows) or, if it still finishes, the image CRC refuses it
        import mscf_z
        if p["size"] - (p["zlen"] + M.BLOCK - 1) // M.BLOCK * M.BLOCK < mscf_z.inplace_gap(p["zdata"], p["ilen"]):
            return "E_DECODE"
        return "OK" if ok else "OK-UNSIGNED"
    if len(p["img"]) != p["ilen"] or p["ilen"] == 0:
        return "E_RANGE"
    if zlib.crc32(p["img"]) & 0xFFFFFFFF != p["icrc"]:
        return "E_CRC"
    msg = signed_message(p)
    for pub in pubs:
        if ed.verify(pub, msg, p["sig"]):
            return "OK"
    if allow_unsigned and p["sig"] == bytes(64):
        return "OK-UNSIGNED"
    return "E_SIG"


def rename_gci(gci, name):
    """-> the same file under another card file name (directory entry only)."""
    nb = name.encode("ascii")
    if not nb or len(nb) > 32:
        raise SystemExit("file name must be 1-32 ASCII characters")
    out = gci[:8] + nb.ljust(32, b"\0") + gci[40:]
    suf = CT.suffix_of(name)
    if suf and len(out) >= GCI_HDR + 64 and not out[GCI_HDR:GCI_HDR + 4] == b"DEV-":     # the renamed copy shows its own readable text (CodeB: "(old)"); a DEV watermark stays
        b = bytearray(out)
        a, c = CT.TEXT[suf]
        b[GCI_HDR:GCI_HDR + 32] = a.encode().ljust(32, b"\0"); b[GCI_HDR + 32:GCI_HDR + 64] = c.encode().ljust(32, b"\0")
        out = bytes(b)
    return out


def mark_dev(gci):
    """Watermark for dev-signed files: the comment lines the card manager shows (not signed, cosmetic)."""
    b = bytearray(gci)
    b[GCI_HDR:GCI_HDR + 32] = b"DEV-SIGNED CODE FILE".ljust(32, b"\0")
    b[GCI_HDR + 32:GCI_HDR + 64] = b"TEST KEY - NEVER A RELEASE".ljust(32, b"\0")
    return bytes(b)


def boot_record(cand, tries=0, good=0):
    """the 16-byte boot record at block offset 0x1000 + 16: 'MSBT', candidate build_seq u32, tries u16, good u16, CRC-32 of those 12 bytes (ota_core.c bootrec_*, cl_loader.c read_floor)."""
    rec = b"MSBT" + struct.pack(">IHH", cand, tries, good)
    return rec + struct.pack(">I", zlib.crc32(rec) & 0xFFFFFFFF)


def make_state(seq, mtime=None, boot=None):
    """GCI of the optional state file: floor record at block offset 0x1000 ('MSST', floor, CRC-32 of those 8 bytes; omitted when seq is 0) and, when `boot` = (cand, tries, good) is given, the boot record at +16."""
    sys.path.insert(0, os.path.join(REPO, "offline", "provision"))
    import multishine_provision as P
    blk = bytearray(M.BLOCK)
    CT.put(blk, "State")
    P.put_icon(blk, M.projname.SHORT + "State")
    if seq:
        rec = b"MSST" + struct.pack(">I", seq)
        blk[M.HDR_OFF:M.HDR_OFF + 12] = rec + struct.pack(">I", zlib.crc32(rec) & 0xFFFFFFFF)
    if boot is not None:
        blk[M.HDR_OFF + 16:M.HDR_OFF + 32] = boot_record(*boot)
    return P.make_dir_entry(M.projname.SHORT + "State", 0, 1, P.PERM_PUBLIC, mtime) + bytes(blk)


def pubs_from(a):
    if getattr(a, "dev", False):
        return [bytes.fromhex(DEV_PUB)]
    if getattr(a, "pub", None):
        return [bytes.fromhex(a.pub)]
    return [ed.public_key(load_seed(a.key))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    k = sp.add_parser("keygen"); k.add_argument("--out", required=True)
    for n in ("pubkey", "sign", "verify"):
        c = sp.add_parser(n)
        if n != "pubkey":
            c.add_argument("file")
        g = c.add_mutually_exclusive_group(required=True)
        g.add_argument("--key"); g.add_argument("--dev", action="store_true")
        if n == "verify":
            g.add_argument("--pub")
            c.add_argument("--min-seq", type=int, default=1); c.add_argument("--abi", type=int, default=M.ABI); c.add_argument("--allow-unsigned", action="store_true")
        if n == "sign":
            c.add_argument("--out", required=True); c.add_argument("--seq", type=int); c.add_argument("--bump", action="store_true")
    i = sp.add_parser("info"); i.add_argument("file")
    s = sp.add_parser("state"); s.add_argument("--seq", type=int, required=True); s.add_argument("--out", required=True)
    s.add_argument("--cand", type=int); s.add_argument("--tries", type=int, default=0); s.add_argument("--good", action="store_true")
    a = ap.parse_args()
    if a.cmd == "keygen":
        print(keygen(a.out))
    elif a.cmd == "pubkey":
        print(pubs_from(a)[0].hex())
    elif a.cmd == "sign":
        seed = DEV_SEED if a.dev else load_seed(a.key)
        raw = open(a.file, "rb").read()
        out = sign(mark_dev(raw) if a.dev else raw, seed, a.seq, a.bump)
        open(a.out, "wb").write(out)
        p = parse(out)
        print("signed %s: build_seq %d, %d bytes, signed by %s%s" % (a.out, p["seq"], len(out), ed.public_key(seed).hex()[:16], " (DEV KEY)" if a.dev else ""))
    elif a.cmd == "verify":
        r = check(open(a.file, "rb").read(), pubs_from(a), a.min_seq, a.abi, a.allow_unsigned)
        print(r)
        sys.exit(0 if r.startswith("OK") else 1)
    elif a.cmd == "info":
        p = parse(open(a.file, "rb").read())
        print("build_seq %d abi %d flags 0x%X hole 0x%08X+0x%X image %d nseg %d npatch %d signed %s" % (
            p["seq"], p["abi"], p["flags"], p["base"], p["size"], p["ilen"], p["nseg"], p["npatch"], "yes" if p["sig"] != bytes(64) else "no"))
        if p["comp"]:
            print("compressed (MSCF v2): stream %d bytes crc 0x%08X, signed message hashes the stored stream (tag MSCFSIGz1)" % (p["zlen"], p["zcrc"]))
    elif a.cmd == "state":
        open(a.out, "wb").write(make_state(a.seq, boot=(a.cand, a.tries, 1 if a.good else 0) if a.cand is not None else None))
        print("state file %s: floor %d" % (a.out, a.seq))


if __name__ == "__main__":
    main()
