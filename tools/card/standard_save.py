#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""THE standard Melee save 'MultishineMelee': one 11-block file used everywhere (card downloads, the system-files builder), deterministic, with
  * everything unlocked, exactly what the console's ml_unlock_all sets in GmSaveData (file offsets of logical file 1):
      0x00 u16 unlocked characters |= 0x7FF        0x02 u16 unlocked stages |= 0x7FF        0x04 u8 feature mask = 0xFF
      0x2E4 u32[3] 'special message seen' bits 0..0x41 set, 0x2D8 u32[3] pending bits cleared, 0x318 u32[0x42] unlock times (fixed constant here)  (gm_8017297C)
      0x2F0 u32[3..10] notice flags, bits 0..299 set (gm_801741FC)
      0x468 s16 trophy count = 293, 0x46C u16[293] trophy flags: low byte 0 -> 1
  * 999 Trophy Lottery coins (melee_coins.set_coins), the world records,
    Vs items OFF (melee_coins.set_items_off), Rumble OFF for all four ports (melee_coins.set_rumble_default)
  * OUR banner/icon art and the real digest (release/save_art.ours_full: the way the loader save gets them); no Nintendo pixels in the output.
Fixed directory-entry mtime, so two runs give the same bytes.  Stdlib only.

  standard_save.py build  --template T.gci --out MultishineMelee.gci [--records-data F.json]     refuses to overwrite OUT
  standard_save.py verify FILE.gci                                                                every property, short report, exit 1 on a failure
  standard_save.py apply  --in CARD.raw --out NEW.raw --template T.gci [--replace]               put the standard file on a card (replace an existing MultishineMelee only with --replace)
The template is a fresh save made by Melee (gitignored; a local work folder); its body is game state, its art and comment are not used as they are.
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
for d in ("vmc_builder", "release"):
    sys.path.insert(0, os.path.join(HERE, "..", d))
sys.path.insert(0, HERE)
import melee_coins as MC      # noqa: E402
import melee_records as MR    # noqa: E402
import save_art as SA         # noqa: E402
import vmc_card as C          # noqa: E402
import vmc_gci as G           # noqa: E402

NAME = MC.MELEE_NAME
COINS = 999
SETS = MR.ALL_SETS
MTIME = 0x325D7438             # fixed directory-entry modification time (the template's)
UNLOCK_TIME = 0x325D7438       # fixed 'unlocked at' second written for conditions not yet unlocked (retail writes the clock)
N_COND = 0x42
N_NOTICE = 0x12C
N_TROPHY = 293
O_CHARS, O_STAGES, O_FEAT = 0x00, 0x02, 0x04
O_PENDING, O_SEEN, O_NOTICE, O_TIMES = 0x2D8, 0x2E4, 0x2F0, 0x318
O_TCOUNT, O_TFLAGS = 0x468, 0x46C


class StdError(Exception):
    pass


def _bit_set(buf, base, i):
    return bool(struct.unpack_from(">I", buf, base + 4 * (i // 32))[0] & (1 << (i % 32)))


def _bit_put(buf, base, i, on):
    o = base + 4 * (i // 32)
    v = struct.unpack_from(">I", buf, o)[0]
    v = (v | (1 << (i % 32))) if on else (v & ~(1 << (i % 32)) & 0xFFFFFFFF)
    struct.pack_into(">I", buf, o, v)


def unlock_all(g):
    """g = GmSaveData bytes (bytearray, edited in place): what ml_unlock_all does on the console."""
    struct.pack_into(">H", g, O_CHARS, struct.unpack_from(">H", g, O_CHARS)[0] | 0x7FF)
    struct.pack_into(">H", g, O_STAGES, struct.unpack_from(">H", g, O_STAGES)[0] | 0x7FF)
    g[O_FEAT] = 0xFF
    for i in range(N_COND):
        if not _bit_set(g, O_SEEN, i):
            struct.pack_into(">I", g, O_TIMES + 4 * i, UNLOCK_TIME)
            _bit_put(g, O_SEEN, i, True)
            _bit_put(g, O_PENDING, i, False)
    for i in range(N_NOTICE):
        _bit_put(g, 0x2F0, i, True)
    for i in range(N_TROPHY):
        o = O_TFLAGS + 2 * i
        v = struct.unpack_from(">H", g, o)[0]
        if v & 0xFF == 0:
            struct.pack_into(">H", g, o, (v & 0xFF00) | 1)
    struct.pack_into(">h", g, O_TCOUNT, N_TROPHY)


def apply_unlocks(data):
    out = bytearray(data)
    for b in MC.file1_sectors(data):
        sl = slice(b * C.BLOCK, (b + 1) * C.BLOCK)
        _, dec = MC.decode_sector(data[sl])
        view = bytearray(dec[MC.DATA_OFF:MC.DATA_OFF + 0x700])
        unlock_all(view)
        dec[MC.DATA_OFF:MC.DATA_OFF + 0x700] = view
        out[sl] = MC.encode_sector(dec)
    return bytes(out)


def build(template, records_data=None):
    """-> .gci bytes (0x40 entry + 11 blocks)."""
    entry, data = MC.template_gci(template)
    if len(data) != MC.BLOCKS * C.BLOCK:
        raise StdError("template: not an 11-block save")
    body = apply_unlocks(data)
    body = MC.set_coins(body, COINS)
    ds = MR.load_dataset(records_data)
    body, _ = MR.set_records(body, ds, list(SETS))
    body = MC.set_items_off(body)
    body = MC.set_rumble_default(body)
    entry = bytearray(entry)
    struct.pack_into(">H", entry, 0x36, 0)
    struct.pack_into(">I", entry, 0x28, MTIME)
    gci = SA.ours_full(bytes(entry) + body)
    prob = problems(gci, ds)
    if prob:
        raise StdError("internal check failed: " + "; ".join(prob))
    return gci


def problems(gci, ds=None, records_data=None):
    """-> list of failed properties ([] = all hold)."""
    bad = []
    try:
        entry, data, info = G.parse_gci(gci)
    except G.GciError as x:
        return ["not a .gci: %s" % x]
    if info["name"] != NAME or info["blocks"] != 11 or len(gci) != 0x40 + 11 * C.BLOCK:
        bad.append("name/size: %r, %d blocks" % (info["name"], info["blocks"]))
    if not SA._layout_ok(gci):
        bad.append("layout is not the Melee save layout (GALE01, RGB5A3 banner, CI8 icon, 11 blocks)")
    if struct.unpack_from(">I", gci, 0x28)[0] != MTIME:
        bad.append("directory mtime is not the fixed one")
    b, ic, pb = SA._ours()
    if gci[SA.BANNER_OFF:SA.BANNER_OFF + SA.BANNER_LEN] != b or gci[SA.ICON_OFF:SA.ICON_OFF + SA.ICON_LEN] != ic or gci[SA.PAL_OFF:SA.PAL_OFF + SA.PAL_LEN] != pb:
        bad.append("banner/icon/palette are not our art")
    if hashlib.sha256(gci[SA.BANNER_OFF:SA.BANNER_OFF + SA.BANNER_LEN]).hexdigest() == SA.DISC_BANNER_SHA256:
        bad.append("banner is Melee's own")
    if gci[SA.DIGEST_OFF:SA.DIGEST_OFF + SA.DIGEST_LEN] != SA.digest(gci):
        bad.append("digest over banner/icon/comment is wrong")
    try:
        bad += G.melee_problems(data)
        copies = MC.file1_sectors(data)
    except MC.CoinError as x:
        return bad + [str(x)]
    gs = []
    for blk in copies:
        _, dec = MC.decode_sector(data[blk * C.BLOCK:(blk + 1) * C.BLOCK])
        gs.append(bytes(dec[MC.DATA_OFF:MC.DATA_OFF + 0x700]))
    if len(gs) != 2 or gs[0] != gs[1]:
        bad.append("the two game-data copies are missing or differ")
    for k, g in enumerate(gs[:2]):
        t = "copy %d: " % k
        if struct.unpack_from(">H", g, O_CHARS)[0] & 0x7FF != 0x7FF:
            bad.append(t + "characters not all unlocked")
        if struct.unpack_from(">H", g, O_STAGES)[0] & 0x7FF != 0x7FF:
            bad.append(t + "stages not all unlocked")
        if g[O_FEAT] != 0xFF:
            bad.append(t + "feature mask not 0xFF")
        if any(not _bit_set(g, O_SEEN, i) or _bit_set(g, O_PENDING, i) for i in range(N_COND)):
            bad.append(t + "unlock conditions not all recorded")
        if any(struct.unpack_from(">I", g, O_TIMES + 4 * i)[0] == 0 for i in range(N_COND)):
            bad.append(t + "an unlock time is zero")
        if any(not _bit_set(g, O_NOTICE, i) for i in range(N_NOTICE)):
            bad.append(t + "notice flags not all set")
        if struct.unpack_from(">h", g, O_TCOUNT)[0] != N_TROPHY or any(struct.unpack_from(">H", g, O_TFLAGS + 2 * i)[0] & 0xFF == 0 for i in range(N_TROPHY)):
            bad.append(t + "trophies not all owned")
        coin, best = struct.unpack_from(">II", g, MC.COIN_OFF)
        if coin != COINS * MC.UNIT or best < coin:
            bad.append(t + "coins are %d tenths, not %d" % (coin, COINS * MC.UNIT))
        if g[MC.ITEM_FREQ_OFF] != MC.ITEMS_OFF:
            bad.append(t + "Vs items are not off")
        if any(g[MC.RUMBLE_OFF + p] != MC.RUMBLE_DEFAULT for p in range(4)):
            bad.append(t + "Rumble is not off for all four ports")
    try:
        ds = ds or MR.load_dataset(records_data)
        again, log = MR.set_records(data, ds, list(SETS))
        if again != data:
            bad.append("records differ from the dataset (%d changes would be made)" % len(log))
    except (MR.RecError, OSError) as x:
        bad.append("records not checkable: %s" % x)
    return bad


def _write_new(path, data):
    if os.path.exists(path):
        raise StdError("%s exists; not overwriting" % path)
    with open(path, "xb") as f:
        f.write(data)


def apply_card(in_path, out_path, template, replace=False):
    if os.path.realpath(in_path) == os.path.realpath(out_path):
        raise StdError("--out must not be the input card")
    gci = build(template)
    entry, data, _ = G.parse_gci(gci)
    card = MC.load_card(in_path)
    ent = MC.melee_entry(card)
    if ent is not None:
        if not replace:
            raise StdError("the card already has %s; pass --replace to put the standard file over it" % NAME)
        card.remove(ent)
    G.check_region(card, bytes(entry[:4]))
    card.add(bytes(entry), data)
    prob = C.Card(bytes(card.img)).consistency()
    if prob:
        raise StdError("card check failed: " + "; ".join(prob))
    return ("replaced" if ent is not None else "added"), bytes(card.img)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("--template", required=True); b.add_argument("--out", required=True); b.add_argument("--records-data")
    v = sub.add_parser("verify"); v.add_argument("file")
    p = sub.add_parser("apply"); p.add_argument("--in", dest="inp", required=True); p.add_argument("--out", required=True); p.add_argument("--template", required=True)
    p.add_argument("--replace", action="store_true")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            gci = build(a.template, a.records_data)
            _write_new(a.out, gci)
            print("%s: %d bytes, sha256 %s" % (a.out, len(gci), hashlib.sha256(gci).hexdigest()))
        elif a.cmd == "verify":
            gci = open(a.file, "rb").read()
            bad = problems(gci)
            for line in ("unlocks: characters, stages, features, 66 conditions, 300 notices, 293 trophies", "coins: %d" % COINS, "records: %s" % "+".join(SETS),
                         "Vs items: off", "Rumble: off (4 ports)", "art: ours (no Melee banner), digest valid", "mtime fixed 0x%08X" % MTIME):
                if not bad:
                    print("ok    " + line)
            for x in bad:
                print("FAIL  " + x)
            print("sha256 %s" % hashlib.sha256(gci).hexdigest())
            return 1 if bad else 0
        else:
            what, img = apply_card(a.inp, a.out, a.template, a.replace)
            _write_new(a.out, img)
            print("%s: standard %s %s" % (a.out, NAME, what))
    except (StdError, MC.CoinError, MR.RecError, G.GciError, C.CardError, OSError) as x:
        print("standard_save: %s" % x, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
