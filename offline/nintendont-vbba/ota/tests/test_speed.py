#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the read speed test's arithmetic (ota/ms_speed.h, tests/speed_test.c) and its wiring in the loader.  Run: python3.14 test_speed.py (needs cc)"""
import os, subprocess, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_speed_test_bin")


class Speed(unittest.TestCase):
    def test_math(self):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-I" + OTA, os.path.join(HERE, "speed_test.c"), "-o", BIN])
        r = subprocess.run([BIN], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-300:])

    def test_wiring(self):
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn('#include "ms_speed.h"', ld)
        self.assertIn("ms_speed_screen();", ld.split("void ms_diag_screen(void)\n{")[1])
        self.assertIn('"Start", "Speed test"', ld)
        self.assertIn("ReadRealDisc(b, offset", ld)
        self.assertIn("ms_speed.h", open(os.path.join(OTA, "stage_loader.py")).read())


if __name__ == "__main__":
    unittest.main()
