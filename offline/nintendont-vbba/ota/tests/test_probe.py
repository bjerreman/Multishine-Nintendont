#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the Connection check core (ota/ms_probe_core.[ch], tests/probe_test.c): ASan + UBSan, and a strict C89 compile of the core alone.  Run: python3.14 test_probe.py (needs cc)"""
import os, subprocess, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_probe_test_bin")


class Probe(unittest.TestCase):
    def test_strict_c89(self):
        subprocess.check_call(["cc", "-std=c89", "-pedantic", "-Wall", "-Wextra", "-Werror", "-Wno-long-long", "-c", os.path.join(OTA, "ms_probe_core.c"), "-o", os.devnull if False else os.path.join(tempfile.gettempdir(), "ms_probe_c89.o")])

    def test_core(self):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA,
                               os.path.join(HERE, "probe_test.c"), os.path.join(OTA, "ms_probe_core.c"), "-o", BIN])
        r = subprocess.run([BIN], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-1500:] + r.stderr[-400:])
        self.assertIn(" 0 failures", r.stdout)


if __name__ == "__main__":
    unittest.main()
