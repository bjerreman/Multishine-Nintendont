#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the disc-drive probe's early exit (ota/ms_driveprobe.h, tests/driveprobe_test.c) and its wiring in the loader.  Run: python3.14 test_driveprobe.py (needs cc)"""
import os, subprocess, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_driveprobe_test_bin")


class DriveProbe(unittest.TestCase):
    def test_decision(self):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-I" + OTA, os.path.join(HERE, "driveprobe_test.c"), "-o", BIN])
        r = subprocess.run([BIN], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-300:])

    def test_wiring(self):
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn('#include "ms_driveprobe.h"', ld)
        body = ld.split("static int drive_probe(void)")[1][:2500]
        self.assertIn("msdp_step(", body)
        self.assertIn("drive: no disc after", body)
        self.assertIn("ds_stop(0)", body)
        self.assertIn("no_drive_probe", body)


if __name__ == "__main__":
    unittest.main()
