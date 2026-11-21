#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the Name Entry inject: the table (ota/ms_inject_tab.h) holds the expected hooks and cave, the apply routine the launcher runs writes exactly those words and nothing else on a matching game, and writes NOTHING on another disc / an already patched
disc / a bad table.  Run: python3.14 test_inject.py   (needs cc)"""
import os, re, subprocess, sys, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(OTA, "..", "..", ".."))
TAB = os.path.join(OTA, "ms_inject_tab.h")


def table():
    """-> dict(hooks=[(addr, stock, new)], cave_addr, cave_words, cave_old_first) parsed from ota/ms_inject_tab.h"""
    h = open(TAB).read()
    hooks = [tuple(int(x, 16) for x in m) for m in re.findall(r"\{ 0x([0-9A-F]{8})U, 0x([0-9A-F]{8})U, 0x([0-9A-F]{8})U \}", h)]
    cave = h.split("ms_inject_cave[MSIJ_CAVE_NWORDS] = {")[1].split("};")[0]
    return {"hooks": hooks, "cave_addr": int(re.search(r"#define MSIJ_CAVE_ADDR 0x([0-9A-F]+)U", h).group(1), 16),
            "cave_words": [int(x, 16) for x in re.findall(r"0x([0-9A-F]{8})U", cave)],
            "cave_old_first": int(re.search(r"#define MSIJ_CAVE_OLD_FIRST 0x([0-9A-F]+)U", h).group(1), 16), "text": h}

BIN = os.path.join(tempfile.gettempdir(), "ms_inject_test_bin")


def parse(out):
    d = {"W": {}}
    for ln in out.strip().splitlines():
        k, *v = ln.split()
        if k == "W":
            d["W"][int(v[0], 16)] = int(v[1], 16)
        else:
            d[k] = int(v[0])
    return d


class Inject(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = table()
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA,
                               os.path.join(HERE, "inject_test.c"), "-o", BIN])

    def run_mode(self, mode):
        return parse(subprocess.check_output([BIN, mode]).decode())

    def test_the_table_shape(self):
        self.assertEqual([h[0] for h in self.t["hooks"]], [0x801BFA20, 0x801B1360, 0x8022E924, 0x801A4578,                       # P1, P2, P3, the prompt patch P5
                                                           0x801AF3A0, 0x801AF3A8, 0x801AF3AC, 0x801AF3B0, 0x801AF3B8, 0x801AFAD4])     # the six silent-create words
        self.assertEqual(self.t["cave_addr"], 0x80362838)
        self.assertEqual(len(self.t["cave_words"]), 52)
        self.assertEqual(self.t["cave_old_first"], 0x7C0802A6)                # the stock function prologue at the cave

    def test_the_wii_variant_is_the_one_inside(self):
        # the cave reads the bus clock word 0x800000F8 and skips the progressive flag / VI configure on a Wii (Nintendont owns the video mode): lwz r11,0xF8(r12)
        self.assertIn(0x816C00F8, self.t["cave_words"])
        self.assertIn(0x7C0B5040, self.t["cave_words"])                       # cmplw r11,r10

    def test_apply_writes_exactly_the_table_words(self):
        r = self.run_mode("ok")
        self.assertEqual(r["STATUS"], 1)
        want = {a: n for a, _, n in self.t["hooks"]}
        for i, w in enumerate(self.t["cave_words"]):
            want[self.t["cave_addr"] + 4 * i] = w
        want = {a: w for a, w in want.items()}
        got = r["W"]
        # a word that equals the filler or the old word is not "changed": every written word that differs is in want, with the right value
        for a, w in got.items():
            self.assertEqual(want[a], w, hex(a))
        for a, w in want.items():
            if a not in got:
                self.fail("word %08X not written" % a)
        self.assertEqual(r["CHANGED"], len(want))

    def test_nothing_is_written_on_another_disc_or_a_bad_table(self):
        for mode, st in (("other-word", 2), ("other-cave", 2), ("applied", 2), ("toomany", 3), ("outofrange", 3)):
            r = self.run_mode(mode)
            self.assertEqual((r["STATUS"], r["CHANGED"]), (st, 0), mode)
        r = self.run_mode("badmagic")
        self.assertEqual((r["STATUS"], r["CHANGED"]), (0, 0))               # not armed: the launcher does nothing

    def test_silent_create_and_prompt_words(self):
        by = {a: (o, n) for a, o, n in self.t["hooks"]}
        self.assertEqual(by[0x801A4578], (0x41820028, 0x48000028))          # beq boot -> b boot: Melee's progressive-scan question is never started (component video or not)
        self.assertEqual(by[0x801AF3AC], (0x38600001, 0x38600000))          # li r3,1 -> li r3,0: no Yes/No buttons
        self.assertEqual(by[0x801AF3B8], (0x38000005, 0x38000006))          # next state = create, as if Yes was chosen
        self.assertEqual(by[0x801AFAD4], (0x41820014, 0x48000020))          # the 'created' acknowledgement needs no A

    def test_default_on_decision(self):
        def d(*a): return int(subprocess.check_output([BIN, "decide"] + [str(x) for x in a]).decode().split()[1])
        self.assertEqual(d(1, 0, 1, 1), 0)         # on: lock passed, no marker, memory card emulation on, card file there
        self.assertEqual(d(0, 0, 1, 1), 1)         # not a game that passed the lock: never
        self.assertEqual(d(1, 1, 1, 1), 2)         # SD:/multishine/no_inject
        self.assertEqual(d(1, 0, 0, 1), 3)         # memory card emulation off
        self.assertEqual(d(1, 0, 1, 0), 4)         # no card file: the card route is not in use

    def test_the_table_holds_only_our_words_and_ten_retail_words(self):
        h = self.t["text"]
        self.assertEqual(len(re.findall(r"\{ 0x", h)), len(self.t["hooks"]))
        # the stock words in the table are single instructions for the safety check, never a block
        self.assertEqual(sorted(o for _, o, _ in self.t["hooks"]), sorted([0x887F0004, 0x38000000, 0x48161431, 0x41820028, 0x80BE007C, 0x80BE0020, 0x38600001, 0x3880000A, 0x38000005, 0x41820014]))
        self.assertLess(len(h), 9000)


if __name__ == "__main__":
    unittest.main(warnings="ignore")
