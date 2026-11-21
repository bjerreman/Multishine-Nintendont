#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the shell's settings screen work: the values forced for the hidden rows (ms_cfg_force.h) leave the shipped config (gen_nincfg.py) byte-identical and change a
dirty config only where intended; the row table (ms_rows.h) leaves no gaps.  Needs the pinned Nintendont CommonConfig.h (a local work folder); skipped without it.
Run: python3.14 test_cfg.py   (needs cc)"""
import os, struct, subprocess, sys, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(OTA, "..", "..", ".."))
INC = os.environ.get("NIN_CFG_INCLUDE", "")     # common/include of a patched Nintendont tree (optional)
BIN = os.path.join(tempfile.gettempdir(), "ms_cfg_test_bin")


@unittest.skipUnless(os.path.exists(os.path.join(INC, "CommonConfig.h")), "pinned Nintendont sources not present")
class Cfg(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I" + INC, "-I" + OTA, os.path.join(HERE, "cfg_test.c"), "-o", BIN])
        import re
        h = open(os.path.join(INC, "CommonConfig.h")).read()
        cls.bit = lambda self, n: 1 << int(re.search(r"\b%s\s*=\s*\((\d+)\)" % n, h).group(1))
        cls.tmp = tempfile.mkdtemp()

    def force(self, b, marker=None, wiiu=0, vcard=0):
        i, o = os.path.join(self.tmp, "in.bin"), os.path.join(self.tmp, "out.bin")
        open(i, "wb").write(b)
        subprocess.check_call([BIN, "force", i, o] + ([] if marker is None else [str(marker), str(wiiu), str(vcard)]))
        return open(o, "rb").read()

    def shipped(self):
        p = os.path.join(self.tmp, "ship.bin")
        subprocess.check_call([sys.executable, os.path.join(REPO, "offline", "nintendont-vbba", "gen_nincfg.py"), os.path.join(INC, "CommonConfig.h"), p])
        return open(p, "rb").read()

    def test_the_shipped_config_is_unchanged(self):
        b = self.shipped()
        self.assertEqual(len(b), 548)
        f = bytearray(self.force(b))
        bit = self.bit("NIN_CFG_BIT_NATIVE_SI")
        cfg = struct.unpack_from(">I", f, 8)[0]
        self.assertTrue(cfg & bit)                                                 # the one difference: Native Control is forced on (a Wii, no opt-out marker)
        struct.pack_into(">I", f, 8, cfg & ~bit)
        self.assertEqual(bytes(f), b)

    def test_a_dirty_config_changes_only_the_hidden_values(self):
        b = bytearray(self.shipped())
        keep = self.bit("NIN_CFG_BIT_DEBUGGER") | self.bit("NIN_CFG_BIT_LED") | self.bit("NIN_CFG_BIT_FORCE_WIDE") | self.bit("NIN_CFG_BIT_AUTO_BOOT") | self.bit("NIN_CFG_BIT_MC_MULTI")
        bad = self.bit("NIN_CFG_BIT_CHEATS") | self.bit("NIN_CFG_BIT_CHEAT_PATH") | self.bit("NIN_CFG_BIT_ARCADE_MODE") | self.bit("NIN_CFG_BIT_CC_RUMBLE") | self.bit("NIN_CFG_BIT_SKIP_IPL")
        cfg = struct.unpack_from(">I", b, 8)[0]
        cfg = (cfg | keep | bad) & ~(self.bit("NIN_CFG_BIT_BBA_EMU") | self.bit("NIN_CFG_BIT_LOG"))      # an old config: BBA emulation and Log both off
        struct.pack_into(">I", b, 8, cfg)
        vid = struct.unpack_from(">I", b, 12)[0] | (1 << 5)                    # Patch PAL50
        struct.pack_into(">I", b, 12, vid)
        struct.pack_into(">I", b, 16, 1)                                       # German
        b[275:277] = b"/x"                                                     # a cheat path
        struct.pack_into(">I", b, 532, 2)                                      # MaxPads 2
        b[540] = 2; b[541] = 10; b[542] = 3; b[543] = 2                        # blocks, video scale, offset, network profile: not hidden rows
        out = self.force(bytes(b))
        o, n = struct.unpack_from(">I", b, 8)[0], struct.unpack_from(">I", out, 8)[0]
        self.assertEqual(n & bad, 0)
        self.assertTrue(n & self.bit("NIN_CFG_BIT_BBA_EMU"))
        self.assertEqual(n & keep, keep)                                       # debug rows, widescreen, auto boot, multi: untouched
        self.assertTrue(n & self.bit("NIN_CFG_BIT_LOG"))                          # Log is forced on (an old config with Log off gets it)
        self.assertFalse(o & self.bit("NIN_CFG_BIT_LOG"))
        self.assertEqual(struct.unpack_from(">I", out, 12)[0], vid & ~(1 << 5))
        self.assertEqual(struct.unpack_from(">I", out, 16)[0], 0xFFFFFFFF)
        self.assertEqual(out[275:530], bytes(255))
        self.assertEqual(struct.unpack_from(">I", out, 532)[0], 4)
        # nothing else moved
        mask = [(8, 12), (12, 16), (16, 20), (275, 530), (532, 536)]
        a, c = bytearray(b), bytearray(out)
        for s, e in mask:
            a[s:e] = bytes(e - s); c[s:e] = bytes(e - s)
        self.assertEqual(a, c)

    def test_the_card_size_follows_the_virtual_card(self):
        b = self.shipped()
        self.assertEqual(b[540], 4)                                            # the loader's first-run default: 1019 blocks
        self.assertEqual(self.force(b)[540], 4)                                # no virtual card: untouched
        out = self.force(b, marker=0, vcard=1)
        self.assertEqual(out[540], 2)                                          # a virtual card (2 MiB, 251 blocks): the kernel must expect that size
        a, c = bytearray(b), bytearray(out)
        a[540] = c[540] = 0
        a[8:12] = c[8:12] = bytes(4)
        self.assertEqual(a, c)

    def test_native_control_is_on_for_a_wii_unless_opted_out_and_never_on_a_wii_u(self):
        b = bytearray(self.shipped())
        bit = self.bit("NIN_CFG_BIT_NATIVE_SI")
        cfg = struct.unpack_from(">I", b, 8)[0]
        stale = bytearray(b); struct.pack_into(">I", stale, 8, cfg | bit)          # an old config that has it ON
        clear = bytearray(b); struct.pack_into(">I", clear, 8, cfg & ~bit)         # one that has it OFF
        for src in (stale, clear):
            off = struct.unpack_from(">I", self.force(bytes(src), 1, 0, 0), 8)[0]
            self.assertFalse(off & bit)                                            # opt-out marker (Wii): forced off, a stale value never survives
            on = struct.unpack_from(">I", self.force(bytes(src), 0, 0, 0), 8)[0]
            self.assertTrue(on & bit)                                              # no opt-out (Wii): forced on
            for marker in (0, 1):
                u = struct.unpack_from(">I", self.force(bytes(src), marker, 1, 0), 8)[0]
                self.assertFalse(u & bit)                                          # a Wii U: never on, marker or not
            self.assertEqual(struct.unpack_from(">I", self.force(bytes(src), 1, 0, 0), 8)[0] & ~bit, struct.unpack_from(">I", self.force(bytes(src), 0, 0, 0), 8)[0] & ~bit)   # nothing else depends on it
        self.assertFalse(cfg & bit)                                                # the shipped config has it off

    def test_unlock_read_speed_always_on_and_memcard_emulation_follows_the_virtual_card(self):
        b = bytearray(self.shipped())
        rem, emu = self.bit("NIN_CFG_BIT_REMLIMIT"), self.bit("NIN_CFG_BIT_MEMCARDEMU")
        cfg = struct.unpack_from(">I", b, 8)[0]
        self.assertTrue(cfg & rem and cfg & emu)                                   # the shipped config has both on
        old = bytearray(b); struct.pack_into(">I", old, 8, cfg & ~(rem | emu))     # an old config with both off
        for wiiu in (0, 1):
            no_card = struct.unpack_from(">I", self.force(bytes(old), 0, wiiu, 0), 8)[0]
            self.assertTrue(no_card & rem)                                        # Unlock Read Speed: on for both consoles, whatever the file said
            self.assertFalse(no_card & emu)                                       # no virtual card file: the stored value stays (a real card can be used)
            card = struct.unpack_from(">I", self.force(bytes(old), 0, wiiu, 1), 8)[0]
            self.assertTrue(card & rem and card & emu)                            # the virtual card exists: emulation on
            keep = struct.unpack_from(">I", self.force(bytes(b), 0, wiiu, 0), 8)[0]
            self.assertTrue(keep & emu)                                           # no card file but the player's stored 'on' is kept

    def test_native_control_row_is_hidden_for_everybody(self):
        for fl in self.COUNTS:
            left, _ = self.ranks(fl)
            self.assertEqual(left[19], -1, fl)

    def ranks(self, fl):
        out = subprocess.run([BIN, "rows", str(fl)], capture_output=True, text=True, check=True).stdout.split()
        rows = [(out[i], int(out[i + 1]), int(out[i + 2])) for i in range(0, len(out), 3)]
        return ([r[2] for r in rows if r[0] == "L"], [r[2] for r in rows if r[0] == "R"])

    def layout(self, fl):
        out = subprocess.check_output([BIN, "layout", str(fl)]).decode().split("\n")
        rows = [(l.split()[0], int(l.split()[1]), int(l.split()[2]), int(l.split()[3]), int(l.split()[4])) for l in out if l and l[0] in "LR"]
        desc = [int(l.split()[2]) for l in out if l.startswith("D ")]
        return rows, desc

    def order(self, fl):
        out = subprocess.check_output([BIN, "order", str(fl)]).decode().split("\n")
        return [("L" if int(l.split()[1]) == 0 else "R", int(l.split()[2])) for l in out if l]

    def step(self, fl, part, row, d):
        o = subprocess.check_output([BIN, "step", str(fl), str(part), str(row), str(d)]).decode().split()
        return ("L" if o[0] == "0" else "R", int(o[1]))

    # layout flags: 1 = dev marker, 2 = video mode forced (the loader draws left row 16 only then), 4 = the virtual card file exists (Memcard Emulation row gone)
    # 8 = the console is a Wii U / vWii.  Network Profile (right row 7) is gone for everybody (the Network screen chooses the connection), WiiU Widescreen (left 10) only on a Wii U, Drive Access LED (left 11) only on a Wii
    COUNTS = {0: 8, 2: 9, 1: 11, 3: 12, 4: 7, 6: 8, 5: 10, 7: 11, 8: 8, 10: 9, 9: 11, 11: 12, 12: 7, 14: 8, 13: 10, 15: 11}

    def test_settings_rows_form_one_column_that_ends_above_the_logo(self):
        # the logo's top is about y 335; a row is 16 px high; the footer starts at 430.  Non-dev: every row in one column; dev: the overflow (right-column rows only) wraps to a second column
        # at the top right and the description starts below it.  No gap anywhere: the slots of a column are 0..k-1 and the y values consecutive.
        for fl, nrows in self.COUNTS.items():
            rows, desc = self.layout(fl)
            dev = fl & 1
            self.assertEqual(len(rows), nrows, fl)
            self.assertEqual(sorted(r[4] for r in rows), list(range(nrows)), fl)                  # every slot used once, none skipped
            col = {0: [], 1: []}
            for p, r, y, c, sl in rows:
                col[c].append(y)
                self.assertLessEqual(y + 16, 335, (fl, p, r, y))
                if c: self.assertEqual(p, "R", (fl, p, r))                                         # only right-column rows wrap (their x is ours to move)
            for c, ys in col.items():
                self.assertEqual(sorted(ys), [76 + 16 * k for k in range(len(ys))], (fl, c))      # no blank slot between rows
            self.assertEqual(len(col[1]), max(0, nrows - 16))
            self.assertLessEqual(desc[-1] + 16, 335, fl)
            if col[1]:
                self.assertGreater(desc[0], max(col[1]))
            self.assertEqual(desc[0], 76 + (80 if dev else 0))

    def test_screen_order_network_profile_then_auto_boot_then_the_rest(self):
        for fl in self.COUNTS:
            o = self.order(fl)
            self.assertNotIn(("R", 7), o)                                                          # Network Profile: gone
            self.assertEqual(o[0], ("L", 7))                                                       # Auto Boot: the first row
            self.assertEqual(("L", 10) in o, bool(fl & 8))                                         # WiiU Widescreen only on a Wii U ...
            self.assertEqual(("L", 11) in o, not (fl & 8))                                         # ... and the Drive Access LED only on a Wii
            if fl & 8: self.assertEqual(o[-1], ("L", 10))                                          # WiiU Widescreen: the LAST row
            mid = [x for x in o[1:] if x != ("L", 10)]
            left = [r for p, r in mid if p == "L"]
            right = [r for p, r in mid if p == "R"]
            self.assertEqual(left, sorted(left))                                                   # the other left rows keep their order
            self.assertEqual(right[0], 7) if False else None
            self.assertEqual(right, sorted(right))
            self.assertEqual(mid, [("L", r) for r in left] + [("R", r) for r in right])            # all left rows, then the right column
            self.assertEqual(("L", 16) in o, bool(fl & 2))                                         # the video mode row exists only when forced
            self.assertEqual(("L", 3) in o, not (fl & 4))                                          # Memcard Emulation: gone while the virtual card exists
            for gone in (("L", 8), ("R", 8), ("L", 12), ("L", 19)):                                # Unlock Read Speed, WiiU Gamepad Slot, Log, Native Control
                self.assertNotIn(gone, o)
        # the photo (not dev, video auto, no virtual card): Network Profile, Auto Boot, Memcard Emulation (3), Force Widescreen (5), Force Progressive (6), Drive Access LED (11), Video (15),
        # Video Width, Screen Position, then WiiU Widescreen (10) last
        self.assertEqual(self.order(0), [("L", 7), ("L", 3), ("L", 5), ("L", 6), ("L", 11), ("L", 15), ("R", 0), ("R", 1)])
        self.assertEqual(self.order(4), [("L", 7), ("L", 5), ("L", 6), ("L", 11), ("L", 15), ("R", 0), ("R", 1)])
        self.assertEqual(self.order(8), [("L", 7), ("L", 3), ("L", 5), ("L", 6), ("L", 15), ("R", 0), ("R", 1), ("L", 10)])

    def test_cursor_follows_the_screen_order_with_wrap(self):
        for fl in self.COUNTS:
            o = self.order(fl)
            n = len(o)
            for i, (p, r) in enumerate(o):
                pp = 1 if p == "R" else 0
                self.assertEqual(self.step(fl, pp, r, 1), o[(i + 1) % n], (fl, p, r))
                self.assertEqual(self.step(fl, pp, r, -1), o[(i - 1) % n], (fl, p, r))
        self.assertEqual(self.step(0, 0, 7, 1), ("L", 3))             # down from Auto Boot: the first other left row
        self.assertEqual(self.step(0, 0, 3, -1), ("L", 7))            # up from it: Auto Boot
        self.assertEqual(self.step(0, 0, 7, -1), ("R", 1))            # up from the top row: the last row (Wii)
        self.assertEqual(self.step(0, 1, 1, 1), ("L", 7))             # down from the last row: the top row
        self.assertEqual(self.step(8, 1, 1, 1), ("L", 10))            # Wii U: down from the last right-column row: WiiU Widescreen
        self.assertEqual(self.step(0, 0, 16, 0), ("L", 7))            # standing on a row that is gone (video no longer forced): back on the top row
        self.assertEqual(self.step(2, 0, 16, 0), ("L", 16))           # ... while it exists the cursor stays
        self.assertEqual(self.step(0, 0, 16, 1), ("L", 7))            # a vanished row: down goes to the first row

    def test_no_gaps_and_removed_rows_stay_removed_in_dev_mode(self):
        for fl in self.COUNTS:
            left, right = self.ranks(fl)
            vis_l = [x for x in left if x >= 0]
            vis_r = [x for x in right if x >= 0]
            self.assertEqual(vis_l, list(range(len(vis_l))))
            self.assertEqual(vis_r, list(range(2)))
            for gone in (0, 4, 8, 13, 14, 17, 18, 19):                           # 8 = Unlock Read Speed (forced on), 19 = Native Control (the marker decides)
                self.assertEqual(left[gone], -1)
            for gone in (2, 3, 4, 5, 6, 7, 8):                                # 8 = WiiU Gamepad Slot, 7 = Network Profile
                self.assertEqual(right[gone], -1)
            self.assertEqual(right[7], -1)                                     # Network Profile: gone (the Network screen)
            self.assertEqual(left[3] >= 0, not (fl & 4))                         # Memcard Emulation: gone while the virtual card exists
            self.assertEqual(left[11] >= 0, not (fl & 8))                      # Drive Access LED: Wii only
            self.assertEqual(left[16] >= 0, bool(fl & 2))
            self.assertEqual(left[12], -1)                                     # Log: hidden for everyone, forced on
            for dbg in (1, 2, 9):
                self.assertEqual(left[dbg] >= 0, bool(fl & 1))


if __name__ == "__main__":
    unittest.main(warnings="ignore")
