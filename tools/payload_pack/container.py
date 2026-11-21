# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Signed payload container "MSPK" v1.  Reference packer and verifier, stdlib only.

Layout (big endian), header 136 bytes, then the 64-byte Ed25519 signature over the header, then the stored payload:
  0   magic "MSPK"             4   container version u8 (=1)     5  compression id u8 (0 none, 1 MSLZ)
  6   kind u8 (1 engine)       7   flags u8 (0)                  8  build_seq u32 (monotonic, downgrade protection)
  12  stored_size u32          16  raw_size u32                  20 min_loader u32 (oldest loader that may run it)
  24  build_id[16] ASCII, NUL padded     40 codeset_sha256[32]    72 stored_sha256[32]   104 raw_sha256[32]
  136 signature[64] = Ed25519(header[0:136])     200 stored payload (stored_size bytes)
The signature covers stored_sha256, so the signature is checked BEFORE the decoder sees a byte; raw_sha256 is checked after."""
import hashlib, struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519_ref as ed
import mslz

MAGIC = b"MSPK"
HDR = 136
SIG = 64
OFF_PAYLOAD = HDR + SIG
COMP_NONE, COMP_MSLZ = 0, 1
MAX_RAW = 1 << 20                 # loader buffer budget: 1 MiB raw at most (host check; the console has its own fixed buffer)


class PackError(Exception):
    pass


def sha(b):
    return hashlib.sha256(b).digest()


def build_header(build_seq, build_id, codeset_sha, stored, raw, comp, min_loader=1, kind=1):
    bid = build_id.encode("ascii")
    if not bid or len(bid) > 16 or any(c < 0x20 or c > 0x7E for c in bid):
        raise PackError("build id must be 1 to 16 printable ASCII characters")
    if len(codeset_sha) != 32:
        raise PackError("codeset hash must be 32 bytes")
    return (MAGIC + struct.pack(">BBBBIIII", 1, comp, kind, 0, build_seq, len(stored), len(raw), min_loader)
            + bid.ljust(16, b"\0") + codeset_sha + sha(stored) + sha(raw))


def pack(raw, seed, build_seq, build_id, codeset_sha, compress=True, min_loader=1):
    raw = bytes(raw)
    if not raw or len(raw) > MAX_RAW:
        raise PackError("raw payload empty or over %d bytes" % MAX_RAW)
    if build_seq < 1 or build_seq > 0xFFFFFFFF:
        raise PackError("build_seq must be 1..2^32-1")
    stored, comp = raw, COMP_NONE
    if compress:
        c = mslz.compress(raw)
        if len(c) < len(raw):
            stored, comp = c, COMP_MSLZ
    hdr = build_header(build_seq, build_id, codeset_sha, stored, raw, comp, min_loader)
    return hdr + ed.sign(seed, hdr) + stored


def parse(blob):
    """Structure only (no signature check): -> dict.  PackError on any malformation."""
    blob = bytes(blob)
    if len(blob) < OFF_PAYLOAD or blob[:4] != MAGIC:
        raise PackError("not an MSPK container")
    ver, comp, kind, flags, seq, ssz, rsz, minl = struct.unpack_from(">BBBBIIII", blob, 4)
    if ver != 1:
        raise PackError("unsupported container version %d" % ver)
    if comp not in (COMP_NONE, COMP_MSLZ) or kind != 1 or flags != 0:
        raise PackError("unknown compression id, kind or flags")
    if len(blob) != OFF_PAYLOAD + ssz:
        raise PackError("stored size does not match the file length")
    if rsz == 0 or rsz > MAX_RAW or (comp == COMP_NONE and rsz != ssz):
        raise PackError("raw size out of range")
    bid = blob[24:40].rstrip(b"\0")
    if not bid or any(c < 0x20 or c > 0x7E for c in bid) or blob[24 + len(bid):40].strip(b"\0"):
        raise PackError("bad build id")
    return {"version": ver, "comp": comp, "kind": kind, "build_seq": seq, "stored_size": ssz, "raw_size": rsz,
            "min_loader": minl, "build_id": bid.decode("ascii"), "codeset_sha256": blob[40:72].hex(),
            "stored_sha256": blob[72:104].hex(), "raw_sha256": blob[104:136].hex(),
            "header": blob[:HDR], "sig": blob[HDR:OFF_PAYLOAD], "stored": blob[OFF_PAYLOAD:]}


def verify(blob, pub, min_seq=0, loader_version=1, codeset_sha=None):
    """Console order: structure, signature, downgrade rule, loader version, stored hash, decompress with a cap, raw hash.
    -> (info dict, raw bytes).  PackError names the first failing step."""
    info = parse(blob)
    if not ed.verify(pub, info["header"], info["sig"]):
        raise PackError("signature invalid")
    if info["build_seq"] < min_seq:
        raise PackError("downgrade refused (build_seq %d < %d)" % (info["build_seq"], min_seq))
    if info["min_loader"] > loader_version:
        raise PackError("needs a newer loader (%d)" % info["min_loader"])
    if codeset_sha is not None and info["codeset_sha256"] != codeset_sha.hex():
        raise PackError("codeset hash differs from the card's codeset")
    if sha(info["stored"]).hex() != info["stored_sha256"]:
        raise PackError("stored hash mismatch")
    if info["comp"] == COMP_MSLZ:
        try:
            raw = mslz.decompress(info["stored"], info["raw_size"])
        except mslz.LZError as x:
            raise PackError("decompress: %s" % x)
    else:
        raw = info["stored"]
    if len(raw) != info["raw_size"] or sha(raw).hex() != info["raw_sha256"]:
        raise PackError("raw hash mismatch")
    return info, raw
