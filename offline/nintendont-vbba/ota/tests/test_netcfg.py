#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the Wii network config core (ota/ms_netcfg_core.[ch], tests/netcfg_test.c): ASan + UBSan, and a strict C89 compile of the core alone.  Run: python3.14 test_netcfg.py (needs cc)"""
import os, subprocess, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_netcfg_test_bin")


class Netcfg(unittest.TestCase):
    def test_strict_c89(self):
        subprocess.check_call(["cc", "-std=c89", "-pedantic", "-Wall", "-Wextra", "-Werror", "-Wno-long-long", "-c", os.path.join(OTA, "ms_netcfg_core.c"), "-o", os.path.join(tempfile.gettempdir(), "ms_netcfg_c89.o")])

    def test_core(self):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA,
                               os.path.join(HERE, "netcfg_test.c"), os.path.join(OTA, "ms_netcfg_core.c"), os.path.join(OTA, "ms_probe_core.c"), "-o", BIN])
        r = subprocess.run([BIN], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-1500:] + r.stderr[-400:])
        self.assertIn(" 0 failures", r.stdout)

    def test_putback_uses_entry_snapshot(self):
        """ms_netedit.h: every put-back restores the snapshot taken when the screen opened (rl / rs), a failed save can only be kept by a second, different press, and the warning is on screen"""
        t = open(os.path.join(OTA, "ms_netedit.h")).read()
        self.assertIn("ne_snap_take();                                                        /* the settings as they are now", t)
        self.assertEqual(t.count("ne_restore(ne_prof"), 4)
        self.assertEqual(t.count("ne_restore(ne_prof, oldl"), 0)
        self.assertEqual(t.count("ne_restore(ne_prof, NULL, olds"), 0)
        self.assertIn("Your old password will be lost if you keep this.", t)
        self.assertIn("Press Y again to keep.", t)
        self.assertIn("ne_snap_free();\n\tClearScreen();\n\treturn changed;", t)


    def test_173_a_kept_save_sets_the_network_profile_used_at_start(self):
        """: connection 2 (cable) saved here, nincfg.bin profile 0 = Auto, every start selected connection 1 (Wi-Fi) again"""
        t = open(os.path.join(OTA, "ms_netedit.h")).read()
        i = t.index("static int ne_profile_file(unsigned prof)\n{"); body = t[i:t.index("\n}\n", i)]
        self.assertIn("read_file(bin, &b, &n, 4096) != 0", body)                      # never created: a missing file stops it
        self.assertIn("msn_nincfg_set_profile(b, n, prof)", body)                      # the magic / length check and the one byte
        for piece in ('"sd:/multishine/nincfg.upd"', "write_file(nw, b, n) != 0 || verify_head(nw, b, n, n) != 0", "msota_swap_in(&fs, bin, nw, old)", 'ota_log("netedit: network profile %u written to nincfg.bin", prof)'):
            self.assertIn(piece, body)
        self.assertNotIn("write_file(bin", body)                                       # never written in place
        self.assertIn("if (sr >= 0 && ne_prof >= 1 && ne_prof <= 3) { ne_profile_set(ne_prof, &changed);", t)   # the SAVED connection, never ne_pend (Auto) or a guess
        self.assertIn("else if (sr == -1 && ne_start_prof != ne_snap_prof) ne_profile_set(ne_snap_prof, &changed);", t)   # a put-back returns the profile too
        self.assertIn("if (tr == 0 && ne_prof >= 1 && ne_prof <= 3 && ne_prof != ne_start_prof) { ne_profile_set(ne_prof, &changed);", t)
        self.assertIn("if (ne_prof_written && ne_start_prof >= 1 && ne_start_prof <= 3) conn_apply_profile(ne_start_prof);", t)
        self.assertIn("msn_start_line(su, sizeof su, ne_start_prof, ne_start_wired)", t)
        self.assertIn("ne_start_prof = file_net_profile();", t)
        self.assertNotIn("ncfg->NetworkProfile = (u8)ne_pend", t)


if __name__ == "__main__":
    unittest.main()
