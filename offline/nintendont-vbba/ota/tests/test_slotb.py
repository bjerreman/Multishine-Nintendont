#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the slot B backup card (ota/ms_slotb.h, tests/slotb_test.c): when it is made or skipped, the create procedure with failures and resume,
the empty card the loader writes judged by tools/card/gc_card_strict.py, and the wiring in the loader sources.  Run: python3.14 test_slotb.py (needs cc)"""
import os, subprocess, sys, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(OTA, "..", "..", ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_slotb_test_bin")


def build():
    subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA,
                           "-I" + os.path.join(REPO, "offline", "client_net"), "-I" + os.path.join(REPO, "offline", "cardloader", "cl"),
                           os.path.join(HERE, "slotb_test.c"), os.path.join(OTA, "ms_card_core.c"), os.path.join(OTA, "ms_ota_core.c"),
                           os.path.join(REPO, "offline", "client_net", "mw2_crypto.c"), os.path.join(REPO, "offline", "payload_pack", "ms_sha256.c"), "-o", BIN])


class Slotb(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build()

    def test_decisions_and_create(self):
        r = subprocess.run([BIN, "decide"], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-400:])

    def test_virtual_card_paths_follow_the_device(self):
        r = subprocess.run([BIN, "vroot"], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "VROOT OK", r.stdout + r.stderr[-400:])

    def test_loader_wiring_for_usb_and_the_shell_check(self):
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn('#define MSCARD_RAW   ms_vpath("saves/GALE.raw")', src)        # no hard-coded SD path for the virtual card
        early = src.split("static void card_root_early(void)")[1].split("static int card_boot_wanted")[0]   # a2fc4087: the early look names BOTH devices on purpose (before UseSD is known)
        self.assertIn('"sd:/multishine/saves/GALE.raw"', early); self.assertIn('"usb:/multishine/saves/GALE.raw"', early)
        self.assertNotIn('"sd:/multishine/saves', src.replace(early, ""))           # everywhere else: no hard-coded SD path for the virtual card
        self.assertIn("ms_vcard_refresh();", src.split("static int pick_game")[1].split("static void put_c")[0])   # the device changes: look again
        body = src.split("static void ota_boot_check_body")[2].split("extern void MsAboutLine")[0]
        self.assertIn("Could not check for updates", body)                          # a failed or late shell check is never "Up to date"
        self.assertIn("ota_latest(&KIND_SHL, shl_have, 5000", body)

    def test_blank_cards_pass_the_strict_check(self):
        for mib, blocks in ((2, 251), (4, 507), (8, 1019)):
            p = os.path.join(tempfile.mkdtemp(), "GALE_B.raw")
            r = subprocess.run([BIN, "blank", str(mib * 1024 * 1024), p], capture_output=True, text=True)
            self.assertEqual(r.stdout.strip(), "BLANK %d free %d" % (mib * 1024 * 1024, blocks), r.stdout + r.stderr[-300:])
            self.assertEqual(os.path.getsize(p), mib * 1024 * 1024)
            s = subprocess.run([sys.executable, "-I", os.path.join(REPO, "tools", "card", "gc_card_strict.py"), p], capture_output=True, text=True)
            self.assertEqual(s.returncode, 0, s.stdout + s.stderr)

    def test_blank_b_is_the_same_format_as_blank_a(self):
        a, b = os.path.join(tempfile.mkdtemp(), "a.raw"), os.path.join(tempfile.mkdtemp(), "b.raw")
        subprocess.check_call([BIN, "blank", str(2 * 1024 * 1024), a], stdout=subprocess.DEVNULL)
        subprocess.check_call([BIN, "blank", str(2 * 1024 * 1024), b], stdout=subprocess.DEVNULL)
        self.assertEqual(open(a, "rb").read(), open(b, "rb").read())

    def test_wiring(self):
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn('#include "ms_slotb.h"', ld)
        self.assertIn("slotb_ensure(1);", ld.split("card_virtual(2);")[1][:300])        # the set-up, right after slot A
        self.assertIn("slotb_ensure(0);", ld.split("int ms_start_screen")[1][:1500])       # the start screen
        self.assertIn("slotb_ensure(0);", ld.split("void ms_autoboot_fix")[1][:300])       # the autoboot path
        self.assertNotIn("NIN_CFG_MC_SLOTB", ld)                                             # the kernel owns that bit
        self.assertNotIn("MC_SLOTB", open(os.path.join(OTA, "ms_cfg_force.h")).read().split("static inline void ms_cfg_force")[1])
        self.assertEqual(ld.count("msb_line("), 3)                                           # start screen, About, Memory card screen


if __name__ == "__main__":
    unittest.main()
