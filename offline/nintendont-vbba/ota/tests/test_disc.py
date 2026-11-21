#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the Nintendont shell's disc lock (ota/ms_disc_lock.c via tests/disc_test.c): the cache format always; the verdict on the REAL stock v1.02 image (read only, nothing
is stored in the repo), plus virtual alterations.  The image tests skip with a message when the retail image is absent.  Run: python3.14 test_disc.py   (needs cc)."""
import hashlib, os, struct, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
OTA = os.path.join(REPO, "offline", "nintendont-vbba", "ota")
BIN = os.path.join(tempfile.gettempdir(), "ms_disc_test_bin")
STOCK_SHA1 = "d4e70c064cc714ba8400a849cf299dbd1aa326fc"
CANDIDATES = ["/Applications/Super Smash Bros. Melee (USA) (En,Ja) (v1.02).iso", os.path.expanduser("~/Downloads/Gamecube/Super Smash Bros. Melee.iso")]


def build_bin():
    subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                           "-I" + OTA, "-I" + os.path.join(REPO, "offline", "payload_pack"), os.path.join(OTA, "tests", "disc_test.c"), os.path.join(OTA, "ms_disc_lock.c"),
                           os.path.join(REPO, "offline", "payload_pack", "ms_sha256.c"), "-o", BIN])


def sha1(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def verdict(path, *mods):
    out = subprocess.run([BIN, "verdict", path, *mods], capture_output=True, text=True).stdout
    v = [l for l in out.splitlines() if l.startswith("VERDICT")][0].split()
    r = [l for l in out.splitlines() if l.startswith("READS")][0].split()
    return int(v[1]), int(r[1]), int(r[3])


def detail(path, *mods):
    out = subprocess.run([BIN, "verdict", path, *mods], capture_output=True, text=True).stdout
    l = [x for x in out.splitlines() if x.startswith("FIRST")][0].split(None, 8)
    return {"first": int(l[1]), "mask": int(l[3], 16), "id": l[5], "rev": int(l[7]), "detail": l[8].partition("DETAIL ")[2]}


class Cache(unittest.TestCase):
    def test_cache(self):
        out = subprocess.run([BIN, "cache"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("disc cache ok", out.stdout)


class Images(unittest.TestCase):
    stock = None

    @classmethod
    def setUpClass(cls):
        for c in CANDIDATES + [os.environ.get("MULTISHINE_RETAIL_ISO", "")]:
            if c and os.path.exists(c) and sha1(c) == STOCK_SHA1:
                cls.stock = c
                break

    def setUp(self):
        if not self.stock:
            self.skipTest("SKIPPED: the retail v1.02 image (SHA-1 %s) is not available here" % STOCK_SHA1)

    def test_accepts_stock_and_reads_little(self):
        v, n, nb = verdict(self.stock)
        self.assertEqual(v, 1)
        self.assertLessEqual(n, 40)
        self.assertLessEqual(nb, 80000)            # about 53 KB of sampled windows plus the header: a few ms of reads, not a hash of 1.4 GB

    def test_rejects_alterations(self):
        # a byte flipped inside every kind of sampled window: header, bi2, apploader, Start.dol, data windows, the variant windows
        for off in (0x0, 0x440, 0x2440, 0x12c960, 0x23aae0, 0x348c60, 0x68680e0, 0x1141e720, 0x51a64cc0):
            v, _, _ = verdict(self.stock, "flip=0x%x" % off)
            self.assertIn(v, (-2, -3, -1), "a flipped byte at 0x%x was accepted" % off)
        # the title / patch-area windows must be stock too: a changed title or a modified image is refused
        self.assertEqual(verdict(self.stock, "flip=0x37e000")[0], -3)
        self.assertEqual(verdict(self.stock, "ascii=0x20:Multishine")[0], -3)
        self.assertEqual(detail(self.stock, "flip=0x37e000")["first"], 15)
        self.assertEqual(verdict(self.stock, "byte=0x7:0x01")[0], -2)       # revision 1 (v1.01)
        self.assertEqual(verdict(self.stock, "byte=0x7:0x00")[0], -2)       # revision 0 (v1.00)
        self.assertEqual(verdict(self.stock, "ascii=0x0:GALP01")[0], -1)    # another game ID (the PAL id)
        self.assertEqual(verdict(self.stock, "ascii=0x0:GALJ01")[0], -1)
        self.assertIn(verdict(self.stock, "byte=0x1c:0")[0], (-1, -2))     # no DVD magic
        self.assertEqual(verdict(self.stock, "size=1000000000")[0], -5)     # truncated
        self.assertEqual(verdict(self.stock, "size=0")[0], -1)

    def test_every_common_window_is_checked_and_named(self):
        offs = [0x0, 0x440, 0x2440, 0x12c960, 0x23aae0, 0x348c60, 0x68680e0, 0x1141e720, 0x1bfd4d80, 0x26b8b3c0, 0x31741a00, 0x3c2f8040, 0x46eae680, 0x51a64cc0]
        for i, off in enumerate(offs[1:], 1):
            v, _, _ = verdict(self.stock, "flip=0x%x" % (off + 0x10))
            self.assertEqual(v, -3, "window %d" % i)
            d = detail(self.stock, "flip=0x%x" % (off + 0x10))
            self.assertEqual((d["first"], d["mask"], d["detail"]), (i + 1, 1 << i, "data window %d differs" % (i + 1)), d)
        d = detail(self.stock, "flip=0x440", "flip=0x68680e0")
        self.assertEqual((d["first"], d["mask"]), (3 - 1, (1 << 1) | (1 << 6)))
        d = detail(self.stock, "ascii=0x0:GALP01")
        self.assertEqual(d["id"], "GALP01")

    def test_scrubbed_copy_still_passes(self):
        # zero every gap between the files (unused areas of a scrubbed image): the verdict must stay
        img = open(self.stock, "rb")
        img.seek(0x420)
        dol, fst, fsz = struct.unpack(">III", img.read(12))
        img.seek(fst)
        table = img.read(fsz)
        n = struct.unpack(">I", table[8:12])[0]
        img.seek(dol)
        hdr = img.read(0x100)
        dol_end = max(struct.unpack(">I", hdr[4 * i:4 * i + 4])[0] + struct.unpack(">I", hdr[0x90 + 4 * i:0x94 + 4 * i])[0] for i in range(18) if struct.unpack(">I", hdr[4 * i:4 * i + 4])[0])
        img.seek(0x2440 + 0x14)
        apl = 0x20 + struct.unpack(">I", img.read(4))[0] + 4
        used = [(0, 0x2440 + apl), (dol, dol_end - dol), (fst, fsz)]
        for i in range(1, n):
            e = table[12 * i:12 * i + 12]
            if e[0] == 0:
                off, size = struct.unpack(">II", e[4:12])
                used.append((off, size))
        used.sort()
        gaps, end = [], 0
        for off, size in used:
            if off > end + 0x10000:
                gaps.append((end, off - end))
            end = max(end, off + size)
        self.assertGreater(len(gaps), 0)
        mods = ["zero=0x%x:%d" % (a + 0x200, b - 0x400) for a, b in gaps[:12]]
        self.assertEqual(verdict(self.stock, *mods)[0], 1)


if __name__ == "__main__":
    build_bin()
    unittest.main(verbosity=2)
