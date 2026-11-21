#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""multishine-provision: put a Slippi identity onto a GameCube memory card image.

Stdlib only, Python 3.9+. See offline/provision/ and
.

Commands
  import  --from user.json --out card.gci|card.raw   validate user.json, write card file
  show    --card card.gci|card.raw                   print connectCode and displayName only
  export-placeholder --out card.gci|card.raw         card.example

Secrets (uid, playKey) are never printed, logged or put in error messages.

Formats (big-endian everywhere, like the console):

GCI file = 0x40-byte directory entry + blockcount * 0x2000 data bytes.
  entry: 00 gamecode "GALE" | 04 maker "01" | 06 0xFF | 07 banner/anim flags |
         08 filename[32] | 28 modtime u32 (secs) | 2C image offset u32 |
         30 icon format u16 | 32 anim speed u16 | 34 permissions | 35 copy counter |
         36 first block u16 | 38 block count u16 | 3A 0xFFFF | 3C comment addr u32
  data:  0x000 comment (2 x 32 bytes) | 0x040 icon 0 (32x32 CI8 + palette, 0x600 bytes) |
         0x1000 Multishine identity payload (below), rest 0x00.
  (icon: now ONE 32x32 CI8 frame = 0x400 pixels + 0x200 RGB5A3 palette, 0x600 bytes at 0x040, icon format 1; see icon_for / tools/brand/make_save_art.py)
Payload v1 (0x128 bytes): "SDID" | u8 version | u8 flags | u16 reserved |
  uid[64] | playKey[128] | connectCode[32] | displayName[64] (UTF-8, NUL padded) |
  u32 CRC32 (zlib) over all preceding payload bytes.

Raw card image (Dolphin .raw / .gcp-less): block 0 header, blocks 1-2 directory
(primary/backup), blocks 3-4 block allocation map (primary/backup), blocks 5.. data.
All checksums are the additive 16-bit pair (sum, sum of ~word; a result of 0xFFFF
becomes 0), see Dolphin GCMemcard.cpp CalculateChecksums.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

BLOCK = 0x2000
GCI_ENTRY = 0x40
DEFAULT_NAME = "MultishineIdentity"
GAME_CODE = b"GALE"
MAKER = b"01"
MAGIC = b"SDID"
VERSION = 1
PAYLOAD_OFFSET = 0x1000
ICON_OFFSET = 0x40
ICON_LEN = 0x600          # one CI8 frame: 0x400 pixels + 256 entry RGB5A3 palette (tools/brand/make_save_art.py); a banner (3,584 B more) would not fit before the payload at 0x1000
ICON_FORMAT = 0x0001      # directory entry 0x30: icon 0 = CI8 with the shared palette that follows the last icon
UID_LEN, KEY_LEN, CODE_LEN, NAME_LEN = 64, 128, 32, 64
PAYLOAD_BODY = 8 + UID_LEN + KEY_LEN + CODE_LEN + NAME_LEN
PAYLOAD_LEN = PAYLOAD_BODY + 4
PERM_PUBLIC, PERM_NOCOPY, PERM_NOMOVE = 0x04, 0x08, 0x10
MIN_CARD_BYTES = 0x80000  # 4 Mbit, 64 blocks, 59 usable
DIR_ENTRIES = 127
EPOCH_2000 = 946684800

CONNECT_RE = re.compile(r"^[^\s#]{1,8}#[0-9]{1,4}$")
PLACEHOLDER_REL = os.path.join("docs", "research", "user.json.example")


class ProvisionError(Exception):
    """User-facing error. Messages must never contain secret values."""


# ---------------------------------------------------------------- validation

def _is_str(v):
    return isinstance(v, str)


def validate_identity(obj):
    """Return a dict with uid, playKey, displayName, connectCode. Never echoes values."""
    if not isinstance(obj, dict):
        raise ProvisionError("user.json must be a JSON object")
    for k in ("uid", "playKey", "displayName", "connectCode"):
        if k not in obj:
            raise ProvisionError("user.json is missing required field '%s'" % k)
        if not _is_str(obj[k]) or not obj[k]:
            raise ProvisionError("field '%s' must be a non-empty string" % k)
        if any(ord(c) < 0x20 or ord(c) == 0x7F for c in obj[k]):
            raise ProvisionError("field '%s' contains control characters" % k)
    limits = {"uid": UID_LEN, "playKey": KEY_LEN, "connectCode": CODE_LEN, "displayName": NAME_LEN}
    for k, lim in limits.items():
        try:
            n = len(obj[k].encode("utf-8"))
        except UnicodeEncodeError:
            raise ProvisionError("field '%s' is not valid text" % k)
        if n >= lim:  # keep one NUL
            raise ProvisionError("field '%s' is too long" % k)
    if not CONNECT_RE.match(obj["connectCode"]):
        raise ProvisionError("field 'connectCode' must look like TAG#123")
    if "latestVersion" in obj and not _is_str(obj["latestVersion"]):
        raise ProvisionError("field 'latestVersion' must be a string")
    if "chatMessages" in obj:
        cm = obj["chatMessages"]
        if not isinstance(cm, list) or len(cm) != 16 or not all(_is_str(x) for x in cm):
            raise ProvisionError("field 'chatMessages' must be a list of exactly 16 strings")
    return {k: obj[k] for k in ("uid", "playKey", "displayName", "connectCode")}


def load_user_json(path):
    try:
        with open(path, "rb") as f:
            raw = f.read(1 << 20)
    except OSError as e:
        raise ProvisionError("cannot read source file (%s)" % (e.strerror or "error"))
    try:
        obj = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        raise ProvisionError("source file is not valid JSON")
    if isinstance(obj, dict):
        obj.pop("_comment", None)  # the placeholder example carries one
    return validate_identity(obj)


def safe_text(s):
    """Make text safe to print (no control/escape characters from a hostile card)."""
    return "".join(c if c.isprintable() else "?" for c in s)


# ---------------------------------------------------------------- payload

def _field(s, n):
    b = s.encode("utf-8")
    return b + b"\x00" * (n - len(b))


def build_payload(ident):
    body = (MAGIC + bytes([VERSION, 0]) + b"\x00\x00" + _field(ident["uid"], UID_LEN)
            + _field(ident["playKey"], KEY_LEN) + _field(ident["connectCode"], CODE_LEN)
            + _field(ident["displayName"], NAME_LEN))
    assert len(body) == PAYLOAD_BODY
    return body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def _unfield(b):
    i = b.find(b"\x00")
    if i >= 0:
        if any(b[i:]):
            raise ProvisionError("payload field is not NUL padded")
        b = b[:i]
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        raise ProvisionError("payload field is not valid UTF-8")


def parse_payload(buf):
    """Return the identity dict, or raise ProvisionError."""
    if len(buf) < PAYLOAD_LEN:
        raise ProvisionError("payload truncated")
    buf = bytes(buf[:PAYLOAD_LEN])
    if buf[:4] != MAGIC:
        raise ProvisionError("not a Multishine identity (bad magic)")
    if buf[4] != VERSION:
        raise ProvisionError("unsupported identity version")
    (crc,) = struct.unpack(">I", buf[PAYLOAD_BODY:])
    if crc != zlib.crc32(buf[:PAYLOAD_BODY]) & 0xFFFFFFFF:
        raise ProvisionError("identity CRC32 mismatch (card data corrupted)")
    o = 8
    out = {}
    for k, n in (("uid", UID_LEN), ("playKey", KEY_LEN), ("connectCode", CODE_LEN), ("displayName", NAME_LEN)):
        out[k] = _unfield(buf[o:o + n])
        o += n
    return out


# ---------------------------------------------------------------- GCI

def _gc_time(t):
    return max(0, int(t) - EPOCH_2000) & 0xFFFFFFFF


_ART = None


def _art():
    global _ART
    if _ART is None:
        import importlib.util
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "brand", "make_save_art.py")
        spec = importlib.util.spec_from_file_location("make_save_art", path)
        _ART = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_ART)
    return _ART


def icon_for(name_or_kind):
    """The ICON_LEN icon bytes for a card file name ('MultishineCode', 'MultishineCodes', ...) or an icon kind ('code', 'identity', ...).
    A name that is not ours gets the plain 'code' logo."""
    art = _art()
    kind = name_or_kind if name_or_kind in art.KINDS else (art.kind_of(name_or_kind) or "code")
    return art.icon_ci8(kind)


def put_icon(block, name_or_kind):
    """Write the icon of a file into its block 0 (bytearray) at ICON_OFFSET."""
    block[ICON_OFFSET:ICON_OFFSET + ICON_LEN] = icon_for(name_or_kind)


def build_gci_data(ident, comment1=None, comment2=None):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import card_text
    comment1 = card_text.pair("Identity")[0] if comment1 is None else comment1
    comment2 = card_text.pair("Identity")[1] if comment2 is None else comment2
    data = bytearray(BLOCK)
    data[0:32] = comment1.encode("ascii")[:31].ljust(32, b"\x00")
    data[32:64] = comment2.encode("ascii")[:31].ljust(32, b"\x00")
    put_icon(data, "identity")
    data[PAYLOAD_OFFSET:PAYLOAD_OFFSET + PAYLOAD_LEN] = build_payload(ident)
    return bytes(data)


def make_dir_entry(name, first_block=0, block_count=1, permissions=PERM_PUBLIC, mtime=None):
    nb = name.encode("ascii")
    if not nb or len(nb) > 32:
        raise ProvisionError("file name must be 1-32 ASCII characters")
    if mtime is None:
        mtime = time.time()
    e = bytearray(GCI_ENTRY)
    e[0:4] = GAME_CODE
    e[4:6] = MAKER
    e[6] = 0xFF
    e[7] = 0x00  # no banner, icon animation loops
    e[8:40] = nb.ljust(32, b"\x00")
    struct.pack_into(">I", e, 0x28, _gc_time(mtime))
    struct.pack_into(">I", e, 0x2C, ICON_OFFSET)  # image offset (icon data; no banner)
    struct.pack_into(">H", e, 0x30, ICON_FORMAT)   # icon 0 = CI8, shared palette after it
    struct.pack_into(">H", e, 0x32, 0x0001)        # icon 0 speed 1
    e[0x34] = permissions
    e[0x35] = 0
    struct.pack_into(">H", e, 0x36, first_block)
    struct.pack_into(">H", e, 0x38, block_count)
    struct.pack_into(">H", e, 0x3A, 0xFFFF)
    struct.pack_into(">I", e, 0x3C, 0x00000000)    # comment at file offset 0
    return bytes(e)


def build_gci(ident, name=DEFAULT_NAME, permissions=PERM_PUBLIC, mtime=None):
    return make_dir_entry(name, 0, 1, permissions, mtime) + build_gci_data(ident)


def parse_entry(e):
    if len(e) < GCI_ENTRY:
        raise ProvisionError("directory entry truncated")
    return {
        "gamecode": bytes(e[0:4]), "maker": bytes(e[4:6]),
        "name": bytes(e[8:40]).split(b"\x00")[0].decode("ascii", "replace"),
        "first": struct.unpack_from(">H", e, 0x36)[0],
        "blocks": struct.unpack_from(">H", e, 0x38)[0],
        "perm": e[0x34],
        "pad": struct.unpack_from(">H", e, 0x3A)[0],
    }


def read_gci(buf):
    if len(buf) < GCI_ENTRY + BLOCK or (len(buf) - GCI_ENTRY) % BLOCK:
        raise ProvisionError("not a GCI file (bad size)")
    ent = parse_entry(buf[:GCI_ENTRY])
    if ent["gamecode"] != GAME_CODE or ent["maker"] != MAKER:
        raise ProvisionError("GCI is not a GALE/01 save")
    if ent["pad"] != 0xFFFF:
        raise ProvisionError("GCI directory entry padding invalid")
    if ent["blocks"] * BLOCK != len(buf) - GCI_ENTRY:
        raise ProvisionError("GCI block count does not match file size")
    return parse_payload(buf[GCI_ENTRY + PAYLOAD_OFFSET:])


# ---------------------------------------------------------------- raw card

def checksum(data):
    """Dolphin additive 16-bit checksum pair over big-endian words of data."""
    c1 = c2 = 0
    for (w,) in struct.iter_unpack(">H", data):
        c1 = (c1 + w) & 0xFFFF
        c2 = (c2 + (w ^ 0xFFFF)) & 0xFFFF
    return (0 if c1 == 0xFFFF else c1), (0 if c2 == 0xFFFF else c2)


def build_header(size_mb, fmt_time=None):
    h = bytearray(b"\xFF" * BLOCK)
    h[0:12] = bytes(12)                      # serial (unique per card on real hardware)
    struct.pack_into(">Q", h, 0x0C, 0 if fmt_time is None else fmt_time)
    struct.pack_into(">I", h, 0x14, 0)       # SRAM bias
    struct.pack_into(">I", h, 0x18, 0)       # SRAM language
    struct.pack_into(">I", h, 0x1C, 0)
    struct.pack_into(">H", h, 0x20, 0)       # device id
    struct.pack_into(">H", h, 0x22, size_mb)  # size in megabits
    struct.pack_into(">H", h, 0x24, 0)       # encoding: 0 = ANSI
    c1, c2 = checksum(h[:0x1FC])
    struct.pack_into(">HH", h, 0x1FC, c1, c2)
    return bytes(h)


def build_dir(entries, update_counter=0):
    d = bytearray(b"\xFF" * BLOCK)
    for i, e in enumerate(entries):
        d[i * GCI_ENTRY:(i + 1) * GCI_ENTRY] = e
    struct.pack_into(">H", d, 0x1FFA, update_counter)
    c1, c2 = checksum(d[:0x1FFC])
    struct.pack_into(">HH", d, 0x1FFC, c1, c2)
    return bytes(d)


def build_bat(total_blocks, chains, update_counter=0):
    """chains: list of block-number lists (each in file order). Block n -> map[n-5]."""
    b = bytearray(BLOCK)
    amap = [0] * 0xFFB
    last = 4
    used = 0
    for chain in chains:
        for i, blk in enumerate(chain):
            amap[blk - 5] = chain[i + 1] if i + 1 < len(chain) else 0xFFFF
            last = blk
            used += 1
    struct.pack_into(">HHH", b, 4, update_counter, total_blocks - 5 - used, last)
    struct.pack_into(">%dH" % len(amap), b, 10, *amap)
    c1, c2 = checksum(b[4:])
    struct.pack_into(">HH", b, 0, c1, c2)
    return bytes(b)


def build_card(ident, name=DEFAULT_NAME, permissions=PERM_PUBLIC, mtime=None, size_bytes=MIN_CARD_BYTES):
    if size_bytes < MIN_CARD_BYTES or size_bytes & (size_bytes - 1) or size_bytes > 0x1000000:
        raise ProvisionError("card size must be a power of two from 0.5 to 16 MiB")
    total = size_bytes // BLOCK
    entry = make_dir_entry(name, 5, 1, permissions, mtime)
    hdr = build_header(size_bytes * 8 // (1 << 20))
    d = build_dir([entry])
    bat = build_bat(total, [[5]])
    blank = b"\xFF" * BLOCK
    parts = [hdr, d, d, bat, bat, build_gci_data(ident)]
    parts += [blank] * (total - len(parts))
    return b"".join(parts)


def _best_copy(buf, off_a, check_fn):
    """Pick the valid copy with the higher update counter, as the console does."""
    good = []
    for off in (off_a, off_a + BLOCK):
        r = check_fn(buf[off:off + BLOCK])
        if r is not None:
            good.append((r, off))
    if not good:
        return None
    return max(good)[1]


def _dir_counter(blk):
    c1, c2 = checksum(blk[:0x1FFC])
    if struct.unpack_from(">HH", blk, 0x1FFC) != (c1, c2):
        return None
    return struct.unpack_from(">H", blk, 0x1FFA)[0]


def _bat_counter(blk):
    c1, c2 = checksum(blk[4:])
    if struct.unpack_from(">HH", blk, 0) != (c1, c2):
        return None
    return struct.unpack_from(">H", blk, 4)[0]


def read_card(buf, name=None):
    """Parse a raw card, validate header/dir/BAT checksums, return the identity."""
    n = len(buf)
    if n < MIN_CARD_BYTES or n % BLOCK or n & (n - 1):
        raise ProvisionError("not a raw memory card image (bad size)")
    hdr = buf[:BLOCK]
    if struct.unpack_from(">HH", hdr, 0x1FC) != checksum(hdr[:0x1FC]):
        raise ProvisionError("card header checksum mismatch")
    if struct.unpack_from(">H", hdr, 0x22)[0] * (1 << 20) // 8 != n:
        raise ProvisionError("card header size does not match image size")
    doff = _best_copy(buf, BLOCK, _dir_counter)
    if doff is None:
        raise ProvisionError("card directory checksum mismatch (both copies)")
    boff = _best_copy(buf, 3 * BLOCK, _bat_counter)
    if boff is None:
        raise ProvisionError("card block allocation map checksum mismatch (both copies)")
    d, bat = buf[doff:doff + BLOCK], buf[boff:boff + BLOCK]
    total = n // BLOCK
    for i in range(DIR_ENTRIES):
        e = parse_entry(d[i * GCI_ENTRY:(i + 1) * GCI_ENTRY])
        if e["gamecode"] != GAME_CODE or e["maker"] != MAKER:
            continue
        if name is not None and e["name"] != name:
            continue
        if not (5 <= e["first"] < total) or e["blocks"] < 1 or e["blocks"] > total - 5:
            continue
        chain, blk = [], e["first"]
        while blk != 0xFFFF and len(chain) <= e["blocks"]:
            if not 5 <= blk < total:
                raise ProvisionError("card block chain out of range")
            chain.append(blk)
            blk = struct.unpack_from(">H", bat, 10 + 2 * (blk - 5))[0]
        if len(chain) != e["blocks"]:
            raise ProvisionError("card block chain length does not match directory")
        data = b"".join(buf[b * BLOCK:(b + 1) * BLOCK] for b in chain)
        if data[PAYLOAD_OFFSET:PAYLOAD_OFFSET + 4] != MAGIC:
            continue
        return parse_payload(data[PAYLOAD_OFFSET:])
    raise ProvisionError("no Multishine identity found on card")


def read_any(buf):
    n = len(buf)
    if n >= MIN_CARD_BYTES and n % BLOCK == 0:
        return read_card(buf)
    return read_gci(buf)


# ---------------------------------------------------------------- safe output

def _git(args, cwd):
    return subprocess.run(["git"] + args, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)


def _find_dot_git(path):
    p = path
    while True:
        if os.path.exists(os.path.join(p, ".git")):
            return True
        q = os.path.dirname(p)
        if q == p:
            return False
        p = q


def check_output_path(out):
    """Refuse paths inside a git work tree unless ignored and untracked."""
    ap = os.path.abspath(out)
    parent = os.path.dirname(ap)
    if not os.path.isdir(parent):
        raise ProvisionError("output directory does not exist")
    real = os.path.join(os.path.realpath(parent), os.path.basename(ap))
    try:
        inside = _git(["rev-parse", "--is-inside-work-tree"], parent)
    except OSError:  # git not installed
        if _find_dot_git(os.path.realpath(parent)):
            raise ProvisionError("output is inside a git work tree and git is unavailable to check ignores")
        return real
    if inside.returncode != 0 or inside.stdout.strip() != b"true":
        # Not in a work tree (or inside a .git dir); a bare .git dir ancestor is also refused.
        if _find_dot_git(os.path.realpath(parent)):
            raise ProvisionError("output is inside a git directory; refusing")
        return real
    if _git(["ls-files", "--error-unmatch", "--", real], parent).returncode == 0:
        raise ProvisionError("output path is tracked by git; refusing")
    if _git(["check-ignore", "-q", "--no-index", "--", real], parent).returncode != 0:
        raise ProvisionError("output path is inside a git work tree and not gitignored; refusing "
                             "(use a path under build/ or out/, or outside the repository)")
    return real


def write_private(path, data, force=False):
    """Write with mode 0600; refuse overwrite without force; never leave a partial file."""
    path = check_output_path(path)
    if os.path.lexists(path) and not force:
        raise ProvisionError("output exists; use --force to overwrite")
    d = os.path.dirname(path)
    if not force:
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            raise ProvisionError("output exists; use --force to overwrite")
        except OSError as e:
            raise ProvisionError("cannot write output (%s)" % (e.strerror or "error"))
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "wb") as f:
                f.write(data)
        except BaseException:
            try:
                os.unlink(path)
            except OSError:
                pass
            raise
        return path
    fd, tmp = tempfile.mkstemp(prefix=".sdprov-", dir=d)  # created 0600
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return path


def is_raw_path(path):
    return os.path.splitext(path)[1].lower() == ".raw"


def build_for_path(path, ident, name, perms):
    if is_raw_path(path):
        return build_card(ident, name, perms), "raw card image"
    return build_gci(ident, name, perms), "GCI"


REVOKE_NOTE = ("Treat this file like a password. Do not share it, upload it, or put it in cloud sync.\n"
               "If it leaks or the card is lost: log in again via the Slippi Launcher and re-provision\n"
               "(whether a new login rotates the playKey is unverified; test with a throwaway account).")


def _emit(out, ident, kind, path):
    print("wrote %s (%s), mode 0600" % (path, kind), file=out)
    print("connectCode: %s" % safe_text(ident["connectCode"]), file=out)
    print("displayName: %s" % safe_text(ident["displayName"]), file=out)
    print(REVOKE_NOTE, file=out)


# ---------------------------------------------------------------- CLI

def _repo_placeholder():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", PLACEHOLDER_REL))


def _perms(args):
    return PERM_PUBLIC | (PERM_NOCOPY | PERM_NOMOVE if args.protect else 0)


def cmd_import(args, out):
    ident = load_user_json(args.src)
    data, kind = build_for_path(args.out, ident, args.name, _perms(args))
    path = write_private(args.out, data, args.force)
    _emit(out, ident, kind, path)
    return 0


def cmd_export_placeholder(args, out):
    ident = load_user_json(args.placeholder or _repo_placeholder())
    data, kind = build_for_path(args.out, ident, args.name, _perms(args))
    path = write_private(args.out, data, args.force)
    _emit(out, ident, kind, path)
    return 0


def cmd_show(args, out):
    try:
        with open(args.card, "rb") as f:
            buf = f.read(0x1000000 + 0x100)
    except OSError as e:
        raise ProvisionError("cannot read card file (%s)" % (e.strerror or "error"))
    ident = read_any(buf)
    print("connectCode: %s" % safe_text(ident["connectCode"]), file=out)
    print("displayName: %s" % safe_text(ident["displayName"]), file=out)
    return 0


def build_parser():
    p = argparse.ArgumentParser(prog="multishine_provision",
                                description="Put a Slippi identity on a GameCube memory card image.")
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--out", required=True, help="output path (.raw = full card image, otherwise GCI)")
        sp.add_argument("--force", action="store_true", help="overwrite an existing output file")
        sp.add_argument("--name", default=DEFAULT_NAME, help="card file name (default %(default)s)")
        sp.add_argument("--protect", action="store_true",
                        help="set the no-copy/no-move permission bits (not honoured by all tools)")

    s = sub.add_parser("import", help="validate a user.json and write a card file")
    s.add_argument("--from", dest="src", required=True, help="path to YOUR user.json (explicit; never scanned for)")
    common(s)
    s.set_defaults(fn=cmd_import)
    s = sub.add_parser("show", help="print connectCode and displayName from a card/GCI")
    s.add_argument("--card", required=True)
    s.set_defaults(fn=cmd_show)
    s = sub.add_parser("export-placeholder", help="write a card from the fake placeholder identity")
    common(s)
    s.add_argument("--placeholder", help=argparse.SUPPRESS)
    s.set_defaults(fn=cmd_export_placeholder)
    return p


def main(argv=None, out=None, err=None):
    out = out or sys.stdout
    err = err or sys.stderr
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args, out)
    except ProvisionError as e:
        print("error: %s" % e, file=err)
        return 2
    except Exception:  # never let a traceback (which could carry locals) reach the terminal
        print("error: internal error (details suppressed to avoid leaking secrets)", file=err)
        return 3


if __name__ == "__main__":
    sys.exit(main())
