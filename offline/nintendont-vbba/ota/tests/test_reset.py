#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of Reset Multishine (ota/ms_reset.h, tests/reset_test.c) and its wiring in the loader: every SD:/multishine file the sources name is classified (removed or kept),
the plan never touches a keep-listed name or anything outside multishine/, the hold needs 2 s, one backup generation, partial failures.  Run: python3.14 test_reset.py (needs cc)"""
import os, re, subprocess, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_reset_test_bin")


def rd(p):
    with open(p) as f:
        return f.read()


class Reset(unittest.TestCase):
    def test_logic(self):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-I" + OTA, os.path.join(HERE, "reset_test.c"), "-o", BIN])
        r = subprocess.run([BIN], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-300:])

    def test_every_multishine_file_is_classified(self):
        h = rd(os.path.join(OTA, "ms_reset.h"))
        rem = set(re.findall(r'"([^"]+)"', h.split("MSR_REMOVE[] = {")[1].split("};")[0]))
        keep = set(re.findall(r'"([^"]+)"', h.split("MSR_KEEP[] = {")[1].split("};")[0]))
        self.assertFalse(rem & keep)
        names = set()
        for fn in ("ms_ota_loader.c", "ms_ui.c", "ms_disc_gate.c", "ms_inject.c", "ms_slotb.h"):
            src = rd(os.path.join(OTA, fn))
            names |= set(re.findall(r'"sd:/multishine/([A-Za-z0-9_.]+)"', src))
            names |= set(re.findall(r'(?:path_of|has_file)\([^"]*"([A-Za-z0-9_.]+)"', src))
        names |= {"saves", "shell.dol"}
        names -= {"dev"}  # classified below anyway
        missing = sorted(n for n in names if n not in rem and n not in keep and n not in ("saves/GALE.raw",))
        self.assertEqual(missing, [], "files named in the sources that the reset lists neither remove nor keep")

    def test_wiring(self):
        ld = rd(os.path.join(OTA, "ms_ota_loader.c"))
        self.assertIn('#include "ms_reset.h"', ld)
        self.assertIn("ms_reset_screen()", ld.split("void ms_about(void)")[1])
        self.assertIn('"Start", "Reset"', ld)
        self.assertIn("FPAD_Start(0)", ld.split("void ms_about(void)")[1])
        self.assertIn("msr_run(", ld)
        self.assertIn("msr_ui_frame(", ld)
        self.assertEqual(ld.count("NIN_CFG_MC_SLOTB"), 0)


if __name__ == "__main__":
    unittest.main()
