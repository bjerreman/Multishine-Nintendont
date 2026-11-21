#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Strict GameCube memory card check: the verdict the game's own CARD library gives a raw card image.

Mirrors vendor/melee/libs/dolphin/src/dolphin/card/CARDCheck.c rule by rule (the Nintendo SDK Melee links):
  mount  = __CARDVerify (CARDMount's last step): VerifyID (header: deviceID 0, size, checksum, encoding; the SRAM flash-ID compare
           is console state, not card content, and is reported separately), VerifyDir (both 8 KiB directory copies: u16 sum / inverse
           sum over 0..0x1FFB), VerifyFAT (both BAT copies: checksum over 4..0x1FFF, free count == real free entries).
           0 errors = READY, 1 or 2 errors = BROKEN (a single bad copy already fails the mount).
  check  = CARDCheckEx (Melee runs it after a mount that said READY or BROKEN, lbcardnew.c mask 0x201): more than one bad copy =
           BROKEN; otherwise every directory entry's chain is walked in the newer FAT (valid block numbers, no block in two files,
           chain length == entry length, ends with 0xFFFF), a used FAT entry outside every chain must point to a valid block or 0xFFFF.
           Orphans and a wrong free count are repaired (not an error).  BROKEN here is what Melee shows as a damaged card.

  gc_card_strict.py CARD.raw [...]      prints the verdicts and the first broken rule; exit 1 when any card would be reported damaged
"""
import struct
import sys

BLOCK = 0x2000
SYS_BLOCKS = 5
READY, BROKEN, ENCODING = 0, -6, -13


def checksum(data):
    """__CARDCheckSum: u16 big-endian sum and sum of inverted words; 0xFFFF becomes 0."""
    c = i = 0
    for (w,) in struct.iter_unpack(">H", data):
        c = (c + w) & 0xFFFF
        i = (i + (w ^ 0xFFFF)) & 0xFFFF
    return (0 if c == 0xFFFF else c), (0 if i == 0xFFFF else i)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Verdict:
    def __init__(self):
        self.mount = READY
        self.check = READY
        self.rules = []          # broken rules, in the order the SDK meets them
        self.notes = []          # repairs CARDCheck would make (not errors)
        self.cur_dir = self.cur_fat = None

    def damaged(self):
        return self.check != READY

    def first(self):
        return self.rules[0] if self.rules else None


def _dir_ok(d, n):
    blk = d[n * BLOCK:(n + 1) * BLOCK]
    return checksum(blk[:0x1FFC]) == struct.unpack_from(">HH", blk, 0x1FFC), struct.unpack_from(">H", blk, 0x1FFA)[0]


def _fat_ok(d, n, nblk):
    blk = d[n * BLOCK:(n + 1) * BLOCK]
    if checksum(blk[4:]) != struct.unpack_from(">HH", blk, 0):
        return False, "checksum", 0
    fat = struct.unpack_from(">4096H", blk)
    free = sum(1 for b in range(SYS_BLOCKS, nblk) if fat[b] == 0)
    if free != fat[3]:
        return False, "free count %d, real %d" % (fat[3], free), fat[2]
    return True, "", fat[2]


def verify(data, encoding=0):
    """-> Verdict for the raw image bytes `data` (any size of whole 8 KiB blocks).  encoding: OSGetFontEncode() of the console (0 ANSI)."""
    v = Verdict()
    d = bytes(data)
    nblk = len(d) // BLOCK
    if len(d) % BLOCK or nblk < 64:
        v.mount = v.check = BROKEN
        v.rules.append("image size %d is not a card" % len(d))
        return v
    # VerifyID
    hdr = d[:0x200]
    dev_id, size, enc = struct.unpack_from(">HHH", hdr, 0x20)          # CARDID: deviceID, size (Mbit), encode
    if dev_id != 0 or size * 16 != nblk:
        v.rules.append("header: deviceID %d / size field %d Mbit for %d blocks" % (dev_id, size, nblk))
        v.mount = v.check = BROKEN
        return v
    if checksum(hdr[:0x1FC]) != struct.unpack_from(">HH", hdr, 0x1FC):
        v.rules.append("header checksum")
        v.mount = v.check = BROKEN
        return v
    if enc != encoding:
        v.rules.append("header encoding %d, console %d" % (enc, encoding))
        v.mount = v.check = ENCODING
        return v
    errors = 0
    dirs = [_dir_ok(d, 1), _dir_ok(d, 2)]
    for k, (ok, _) in enumerate(dirs):
        if not ok:
            errors += 1
            v.rules.append("directory copy %d (block %d) checksum" % (k, 1 + k))
    fats = [_fat_ok(d, 3, nblk), _fat_ok(d, 4, nblk)]
    for k, (ok, why, _) in enumerate(fats):
        if not ok:
            errors += 1
            v.rules.append("BAT copy %d (block %d) %s" % (k, 3 + k, why))
    v.mount = READY if errors == 0 else BROKEN
    if errors > 1:
        v.check = BROKEN
        return v
    # the copy in force: the valid one, or (both valid) the newer by signed counter difference (VerifyDir / VerifyFAT pick the older index and overwrite it with the newer)
    if dirs[0][0] and dirs[1][0]:
        v.cur_dir = 1 if s16(dirs[0][1]) - s16(dirs[1][1]) < 0 else 0
    else:
        v.cur_dir = 0 if dirs[0][0] else 1
    if fats[0][0] and fats[1][0]:
        v.cur_fat = 1 if s16(fats[0][2]) - s16(fats[1][2]) < 0 else 0
    else:
        v.cur_fat = 0 if fats[0][0] else 1
    if errors:
        v.notes.append("CARDCheck rewrites the bad copy from the good one")
    dblk = d[(1 + v.cur_dir) * BLOCK:(2 + v.cur_dir) * BLOCK]
    fat = struct.unpack_from(">4096H", d, (3 + v.cur_fat) * BLOCK)
    used = {}
    for n in range(127):
        e = dblk[n * 0x40:(n + 1) * 0x40]
        if e[0] == 0xFF:
            continue
        name = e[8:40].split(b"\0")[0].decode("latin1", "replace")
        start, length = struct.unpack_from(">HH", e, 0x36)
        b, cnt = start, 0
        while b != 0xFFFF and cnt < length:
            if not (SYS_BLOCKS <= b < nblk):
                v.rules.append("file %r: block number %d in its chain is not a data block" % (name, b))
                v.check = BROKEN
                return v
            if b in used:
                v.rules.append("file %r: block %d is also in %r" % (name, b, used[b]))
                v.check = BROKEN
                return v
            used[b] = name
            cnt += 1
            b = fat[b]
        if cnt != length or b != 0xFFFF:
            v.rules.append("file %r: chain has %d blocks%s, the entry says %d" % (name, cnt, "" if b == 0xFFFF else " and does not end", length))
            v.check = BROKEN
            return v
    free = 0
    for b in range(SYS_BLOCKS, nblk):
        if b not in used:
            if fat[b] != 0:
                v.notes.append("orphan block %d freed" % b)
            free += 1
        elif not (SYS_BLOCKS <= fat[b] < nblk) and fat[b] != 0xFFFF:
            v.rules.append("BAT entry of block %d points to %d" % (b, fat[b]))
            v.check = BROKEN
            return v
    if free != fat[3]:
        v.notes.append("free count %d set to %d" % (fat[3], free))
    return v


def name(code):
    return {READY: "READY", BROKEN: "BROKEN", ENCODING: "ENCODING"}.get(code, str(code))


def main(argv):
    bad = 0
    for p in argv:
        v = verify(open(p, "rb").read())
        print("%s: mount %s, CARDCheck %s%s" % (p, name(v.mount), name(v.check), " = Melee: damaged card" if v.damaged() else ""))
        for r in v.rules:
            print("  rule broken: " + r)
        for n in v.notes:
            print("  repair: " + n)
        bad |= v.damaged()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
