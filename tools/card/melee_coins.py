#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Trophy Lottery coins in Melee v1.02 NTSC-U's own save (the card file 'MultishineMelee'), COPY ONLY.

  melee_coins.py show   FILE                       coins of the Melee save in a raw card image or a .gci (nothing is written)
  melee_coins.py apply  --in CARD --out NEW.raw [--coins 999] [--template T] [--name MultishineMelee]
                                                   write NEW.raw = CARD with the coins set; CARD is never modified, --out must be another path
  melee_coins.py capture --from CARD --out T.gci   take Melee's own fresh save out of a card Melee has made (Dolphin or console) as a .gci template

Where the number lives (vendor/melee decompilation + a Dolphin-made save):
  gmMainLib_8015CCF0() = &GmSaveData.x1A48 = file offset 0x1E0 of Melee's logical file 1 (GmSaveData, 0x1790 B) is the coin count, 0x1E4 the best-ever count.
  The stored value is TENTHS OF COINS: the Lottery shows stored/10 (tyFigupon_GetCoinCount) and a pull takes 10 (gm_801623FC(count - 0xA)); the game clamps to 9999
  (gm_801623FC: 0x270F), so the largest number the screen can show is 999 and 999 coins is stored as 9990.
File format: the card file has 11 blocks of 8 KiB; block 0 is comment/banner/icons, blocks 1..10 are the HSD card sectors: 16 bytes checksum, then 8,176 bytes
  that are scrambled byte by byte with the PREVIOUS stored byte.  Decoded, a sector starts
  with a 16-byte header (block id u16, sequence u8, three 4-byte file-table entries) and its data begins at +0x20.  Logical file 1 is double-buffered: sectors with
  block id 1 exist twice (here blocks 1 and 10, same sequence); both are patched.  The checksum is the 16-byte HSD sum over the decoded 8,160 bytes (seed 0123..,
  no carry, equal neighbours fixed by xor 0xFF) = melee_save_check.b2.
Needs a template only when the card has no Melee save yet (Melee makes its own at the first boot of a fresh card): the template is a fresh save made by Melee itself
(its banner and icons are Nintendo data, so no template is shipped in the repository; make one with `capture`).  Stdlib only.
"""
import argparse
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "vmc_builder")))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))
import vmc_card as C        # noqa: E402
import vmc_gci as G         # noqa: E402
import melee_save_check as M  # noqa: E402

MELEE_NAME = "MultishineMelee"          # the name the exploit loader gives Melee's save in RAM
BLOCKS = 11
COIN_OFF = 0x1E0                         # GmSaveData.x1A48: current coins, in tenths
BEST_OFF = 0x1E4                         # GmSaveData.x1A4C: best count ever held, in tenths
DATA_OFF = 0x20                          # decoded sector: 0x10 checksum + 0x10 header, then data
FILE1_SIZE = 0x1790                      # sizeof(GmSaveData), the file-table entry of logical file 1
ITEM_FREQ_OFF = 0x448                    # GamePrefs.item_freq (GmSaveData +0x1CB0): 0xFF = items off, 2 = medium (Melee's own fresh save)
ITEMS_OFF = 0xFF
RUMBLE_OFF = 0x458                       # GamePrefs.rumble_enabled[4] (GmSaveData +0x448 + 0x10; the game reads it as lbz port + 0x1CC0(save)): 1 = on per controller port
RUMBLE_DEFAULT = 0                     #: rumble OFF by default (was ON)
MAX_STORED = 0x270F                      # gm_801623FC clamp
UNIT = 10
DEFAULT_COINS = 999
DEFAULT_TEMPLATE = os.path.normpath(os.path.join(HERE, "..", "..", "build", "coins", "MultishineMelee_template.gci"))


class CoinError(Exception):
    pass


# -- sector codec ---------------------------------------------------------------------------------------------------------------------------------------
_INV = {}


def _inverse(prev):
    t = _INV.get(prev)
    if t is None:
        t = bytearray(256)
        for raw in range(256):
            t[M.f302c(prev, raw)] = raw
        _INV[prev] = t
    return t


def decode_sector(blk):
    ok, dec = M.verify(blk)
    return ok, bytearray(dec)


def encode_sector(dec):
    """Decoded 8 KiB sector (checksum field ignored) -> stored sector with a fresh checksum."""
    out = bytearray(dec)
    out[:16] = M.b2(bytes(dec[16:]))
    prev = out[15]
    for i in range(16, len(out)):
        raw = _inverse(prev)[dec[i]]
        out[i] = raw
        prev = raw
    return bytes(out)


def sector_header(dec):
    """-> (block id, sequence, {file_idx: size})"""
    bid = (dec[0x10] << 8) | dec[0x11]
    seq = dec[0x12]
    files = {}
    for k in range(3):
        o = 0x13 + 4 * k
        idx, b1, d1, d2 = dec[o], dec[o + 1], dec[o + 2], dec[o + 3]
        size = (((d1 | ((b1 << 8) & 0x3F00)) << 8) | d2)
        if size:
            files[idx] = size
    return bid, seq, files


# -- the Melee save -------------------------------------------------------------------------------------------------------------------------------------
def file1_sectors(data):
    """Block numbers (1..10) of the 11-block file that hold logical file 1 (block id 1, valid), after structural checks."""
    if len(data) != BLOCKS * C.BLOCK:
        raise CoinError("a Melee save is %d blocks, this one is %d" % (BLOCKS, len(data) // C.BLOCK))
    hits = []
    for b in range(1, BLOCKS):
        ok, dec = decode_sector(data[b * C.BLOCK:(b + 1) * C.BLOCK])
        if not ok:
            continue                         # a stale sector that never got a checksum; the game ignores it, so do we
        bid, seq, files = sector_header(dec)
        if bid == 1:
            if files.get(1) != FILE1_SIZE:
                raise CoinError("block %d: logical file 1 is %r bytes, expected 0x%X: not a Melee 1.02 save" % (b, files.get(1), FILE1_SIZE))
            hits.append(b)
    if not hits:
        raise CoinError("no sector with block id 1 (the game data) found: not a Melee 1.02 save")
    return hits


def read_coins(data):
    """-> [(block, stored coins, stored best)] for every copy of the game data."""
    out = []
    for b in file1_sectors(data):
        _, dec = decode_sector(data[b * C.BLOCK:(b + 1) * C.BLOCK])
        out.append((b, struct.unpack_from(">I", dec, DATA_OFF + COIN_OFF)[0], struct.unpack_from(">I", dec, DATA_OFF + BEST_OFF)[0]))
    return out


def set_coins(data, coins):
    """Whole-file bytes with `coins` (as the Lottery shows them) in every copy of the game data, checksums fixed.  Nothing else changes."""
    if not 0 <= coins <= MAX_STORED // UNIT:
        raise CoinError("coins must be 0..%d (the game clamps the stored value to %d tenths)" % (MAX_STORED // UNIT, MAX_STORED))
    stored = coins * UNIT
    out = bytearray(data)
    for b in file1_sectors(data):
        sl = slice(b * C.BLOCK, (b + 1) * C.BLOCK)
        _, dec = decode_sector(data[sl])
        struct.pack_into(">I", dec, DATA_OFF + COIN_OFF, stored)
        best = struct.unpack_from(">I", dec, DATA_OFF + BEST_OFF)[0]
        if best < stored:
            struct.pack_into(">I", dec, DATA_OFF + BEST_OFF, stored)
        out[sl] = encode_sector(dec)
    bad = G.melee_problems(bytes(out))
    if bad:
        raise CoinError("internal check failed: " + "; ".join(bad))
    for b, cur, _best in read_coins(bytes(out)):
        if cur != stored:
            raise CoinError("internal check failed: block %d reads back %d" % (b, cur))
    return bytes(out)


def set_items_off(data):
    """Whole-file bytes with the Vs item switch OFF in every copy of the game data (: a Multishine-made save starts like a tournament
    setup, items off, as it starts with everything unlocked), checksums fixed.  Only for a save WE add from the template: a player's own save keeps
    the player's choice."""
    out = bytearray(data)
    for b in file1_sectors(data):
        sl = slice(b * C.BLOCK, (b + 1) * C.BLOCK)
        _, dec = decode_sector(data[sl])
        dec[DATA_OFF + ITEM_FREQ_OFF] = ITEMS_OFF
        out[sl] = encode_sector(dec)
    bad = G.melee_problems(bytes(out))
    if bad:
        raise CoinError("internal check failed: " + "; ".join(bad))
    return bytes(out)


def set_rumble_default(data):
    """Whole-file bytes with Rumble at the default (OFF) for all four controller ports in every copy of the game data (: one default everywhere, on; Melee's own fresh
    save already has 1 1 1 1, so for a template made by Melee this changes nothing; it only makes the standard save independent of what the template had)."""
    out = bytearray(data)
    for b in file1_sectors(data):
        sl = slice(b * C.BLOCK, (b + 1) * C.BLOCK)
        _, dec = decode_sector(data[sl])
        for p in range(4):
            dec[DATA_OFF + RUMBLE_OFF + p] = RUMBLE_DEFAULT
        out[sl] = encode_sector(dec)
    bad = G.melee_problems(bytes(out))
    if bad:
        raise CoinError("internal check failed: " + "; ".join(bad))
    return bytes(out)


# -- containers -----------------------------------------------------------------------------------------------------------------------------------------
def _read(path):
    try:
        with open(path, "rb") as f:
            return f.read(C.MAX_BYTES + 1)
    except OSError as x:
        raise CoinError("cannot read %s (%s)" % (path, x.strerror))


def load_card(path):
    buf = _read(path)
    try:
        return C.Card(buf)
    except C.CardError as x:
        raise CoinError("%s: %s" % (path, x))


def melee_entry(card, name=MELEE_NAME):
    return card.find(name)


def template_gci(path, name=MELEE_NAME):
    """A template given as .gci (preferred) or as a card image that holds Melee's save -> (entry bytes, data)."""
    buf = _read(path)
    try:
        entry, data, info = G.parse_gci(buf)
    except G.GciError:
        card = load_card(path)
        e = melee_entry(card, name)
        if e is None:
            raise CoinError("%s holds no %s: make a template with `capture --from <card Melee has made>`" % (path, name))
        entry, data = bytearray(e["raw"]), card.read(e)
        struct.pack_into(">H", entry, 0x36, 0)
        return bytes(entry), data
    if info["name"] != name:
        raise CoinError("%s is %r, expected %r" % (path, info["name"], name))
    return entry, data


def _write_new(path, data, mode):
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".melee_coins.", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)
        if os.path.exists(path):
            raise CoinError("%s already exists: this tool never overwrites (remove it yourself if you mean to replace it)" % path)
        os.rename(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def apply_to_card(in_path, out_path, coins=DEFAULT_COINS, template=None, name=MELEE_NAME, force=False):
    """-> (message, card bytes).  Never writes in_path.  The other files of the card come out byte-identical (checked)."""
    if os.path.realpath(in_path) == os.path.realpath(out_path):
        raise CoinError("--out must not be the input card (copy only, never modify a card in place)")
    card = load_card(in_path)
    before = C.Card(bytes(card.img))
    ent = melee_entry(card, name)
    if ent is not None:
        data = card.read(ent)
        new = set_coins(data, coins)
        for blk_i, b in enumerate(card.chain(ent)):
            card.img[b * C.BLOCK:(b + 1) * C.BLOCK] = new[blk_i * C.BLOCK:(blk_i + 1) * C.BLOCK]
        msg = "%s: coins set to %d in the existing save (%d block copies patched)" % (name, coins, len(file1_sectors(data)))
    else:
        tpath = template or os.environ.get("MS_MELEE_TEMPLATE") or DEFAULT_TEMPLATE
        if not os.path.exists(tpath):
            raise CoinError("the card has no %s yet and no template was found (%s): make one with `capture --from <card Melee has made>` "
                            "or pass --template" % (name, tpath))
        entry, data = template_gci(tpath, name)
        try:
            G.check_region(card, bytes(entry[:4]))
        except G.GciError as x:
            raise CoinError(str(x))
        new = set_items_off(set_coins(data, coins))           # a save we add: items off from the start (the player can turn them on in Vs rules)
        entry = bytearray(entry)
        struct.pack_into(">H", entry, 0x36, 0)
        try:
            card.add(bytes(entry), new)
        except C.CardError as x:
            raise CoinError(str(x))
        msg = "%s: added from the template %s with coins %d, items off (%d blocks)" % (name, os.path.basename(tpath), coins, BLOCKS)
    after = C.Card(bytes(card.img))
    for e in before.entries():
        if e["name"] == name:
            continue
        n = after.find(e["name"], e["gamecode"], e["maker"])
        if n is None or after.read(n) != before.read(e) or n["raw"][:0x36] != e["raw"][:0x36]:
            raise CoinError("internal check failed: file %r would change; nothing written" % e["name"])
    prob = after.consistency()
    if prob:
        raise CoinError("internal check failed: " + "; ".join(prob))
    return msg, bytes(card.img)


# -- CLI ------------------------------------------------------------------------------------------------------------------------------------------------
def cmd_show(a):
    buf = _read(a.file)
    try:
        entry, data, info = G.parse_gci(buf)
        where = ".gci %s" % info["name"]
    except G.GciError:
        card = load_card(a.file)
        e = melee_entry(card, a.name)
        if e is None:
            print("%s: no %s on this card (Melee makes it at its first boot)" % (a.file, a.name))
            return 0
        data, where = card.read(e), "card file %s" % a.name
    rows = read_coins(data)
    problems = G.melee_problems(data)
    print("%s (%s): %s" % (a.file, where, "checksums OK" if not problems else "; ".join(problems)))
    for b, cur, best in rows:
        print("  block %d: coins %d (stored %d), best %d (stored %d)" % (b, cur // UNIT, cur, best // UNIT, best))
    return 0 if not problems else 1


def cmd_apply(a):
    msg, img = apply_to_card(a.inp, a.out, a.coins, a.template, a.name)
    _write_new(a.out, img, os.stat(a.inp).st_mode & 0o777)
    print(msg)
    print("wrote %s (%d bytes); %s is untouched" % (a.out, len(img), a.inp))
    return 0


def cmd_capture(a):
    card = load_card(a.src)
    e = melee_entry(card, a.name)
    if e is None:
        raise CoinError("%s has no %s" % (a.src, a.name))
    data = card.read(e)
    bad = G.melee_problems(data)
    if bad:
        raise CoinError("that save is damaged: " + "; ".join(bad))
    gci = G.export_gci(card, e)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    _write_new(a.out, gci, 0o644)
    print("template %s: %s, coins in it now %s" % (a.out, a.name, ", ".join(str(c // UNIT) for _, c, _b in read_coins(data))))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("show"); s.add_argument("file"); s.add_argument("--name", default=MELEE_NAME); s.set_defaults(fn=cmd_show)
    s = sp.add_parser("apply")
    s.add_argument("--in", dest="inp", required=True); s.add_argument("--out", required=True)
    s.add_argument("--coins", type=int, default=DEFAULT_COINS, help="coins as the Lottery shows them, 0..999 (default 999)")
    s.add_argument("--template", help="a fresh Melee save (.gci or a card) used only when the card has no Melee save (default $MS_MELEE_TEMPLATE, then %s)" % DEFAULT_TEMPLATE)
    s.add_argument("--name", default=MELEE_NAME); s.set_defaults(fn=cmd_apply)
    s = sp.add_parser("capture")
    s.add_argument("--from", dest="src", required=True); s.add_argument("--out", required=True); s.add_argument("--name", default=MELEE_NAME); s.set_defaults(fn=cmd_capture)
    a = p.parse_args(argv)
    try:
        return a.fn(a)
    except (CoinError, C.CardError, G.GciError) as x:
        print("melee_coins: %s" % x, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
