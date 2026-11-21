#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the start screen and the quick set-up (ota/ms_start.h, pure C: step order and resume, main key, autoboot rules, footers, texts) and the wiring in the loader sources.
Run: python3.14 test_start.py   (needs cc)"""
import os, subprocess, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))


def src(n):
    return open(os.path.join(OTA, n)).read()


class Logic(unittest.TestCase):
    def test_start_header(self):
        b = os.path.join(tempfile.gettempdir(), "ms_start_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I" + OTA, os.path.join(HERE, "start_test.c"), "-o", b])
        r = subprocess.run([b], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout)


class DiscSearch(unittest.TestCase):
    def test_search_on_fake_trees(self):
        b = os.path.join(tempfile.gettempdir(), "ms_discsearch_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA, os.path.join(HERE, "discsearch_test.c"), "-o", b])
        r = subprocess.run([b], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-400:])

    def test_one_search_for_start_setup_and_autoboot(self):
        ld = src("ms_ota_loader.c")
        self.assertEqual(ld.count("mds_search(&DS_FS"), 1)                                  # the only place that walks a device
        self.assertIn("find_discs_on(usb, 0, picks, 1,", ld)                                # start screen and autoboot (scan_dev)
        self.assertIn("n = find_discs(discs, MSPK_MAX_PICK, &seen);", ld)                   # the set-up's disc step
        self.assertIn("ndisc = find_discs(discs, MSPK_MAX_PICK, &pk_seen)", ld)             # the card's system files
        self.assertNotIn("gd_collect", ld)
        self.assertIn("disc_image_ok(path)", ld.split("static int ds_check")[1][:300])      # the header test before the lock, never a name alone


class Browse(unittest.TestCase):
    def test_browser_on_fake_trees(self):
        b = os.path.join(tempfile.gettempdir(), "ms_browse_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA, os.path.join(HERE, "browse_test.c"), "-o", b])
        r = subprocess.run([b], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-400:])

    def test_browser_and_drive_wiring(self):
        ld, gate, st = src("ms_ota_loader.c"), src("ms_disc_gate.c"), src("stage_loader.py")
        self.assertIn('"ms_browse.h"', st)                                                    # staged into the loader tree
        self.assertIn('#include "ms_browse.h"', ld)
        self.assertEqual(ld.count("mds_search(&DS_FS"), 1)                                    # the browser lists through the same FatFs callbacks, not a second walker
        self.assertIn("msb_open(s, &DS_FS,", ld)
        self.assertIn("msst_key_decision(key, action, g_drive, g_dev[dev].nstock, msst_setup_needed(&f), g_drive_ok)", ld)
        for s in ("dec == MSST_D_DRIVE", "dec == MSST_D_BROWSER", "dec == MSST_D_LIST", "use_drive_flow()", "file_browser(dev, save)"):
            self.assertIn(s, ld, s)
        k = ld.index("static int br_check")
        body = ld[k:k + 2600]
        self.assertLess(body.index("MDS_STOCK_SIZE"), body.index("msdl_header_ok"))            # size, then header, then the lock
        self.assertLess(body.index("msdl_header_ok"), body.index("gd_check(0, path, 0)"))     # the very check of a game start, never a name
        self.assertIn("ms_disc_poll = br_poll", body)                                         # B stops the check
        self.assertIn("Checking the disc...", body)
        self.assertIn("pick_game(s->usb, s->pick)", ld)                                       # the locked-in image becomes GamePath
        self.assertIn("g_chosen", ld.split("static int find_discs_on")[1][:1200])             # every search tries the browser's pick first
        self.assertIn("ms_disc_why", gate)                                                    # the lock's own reason, shown as 'Not stock Melee v1.02: ...'
        self.assertIn("msdl_detail(v, &info, ms_disc_why", gate)
        self.assertIn("ms_disc_poll && ms_disc_poll()", gate)
        self.assertIn("g_drive_ok = msst_drive_probe_allowed(IsWiiU(), isWiiVC", ld)          # hidden on a Wii U / Wii VC


class Wiring(unittest.TestCase):
    def test_stage_loader_hooks(self):
        st = src("stage_loader.py").replace("\\", "")
        for s in ("ms_start_screen(SelectGame", "ms_autoboot_setup_needed", "ms_autoboot_fix", "ms_autoboot_saved", "ms_sel_mode", '"ms_start.h"'):
            self.assertIn(s, st, s)

    def test_no_game_info_box(self):
        st = src("stage_loader.py")
        self.assertNotIn("Game Version", st)
        self.assertNotIn("Verify MD5", st)
        self.assertIn('"md5.c"', st)           # the files that only the info box used are removed from the tree

    def test_about_keys(self):
        ld = src("ms_ota_loader.c")
        self.assertIn('"B", "Back", "A", "Check for updates", "X", "Memory card", "Y", "Problem report"', ld)

    def test_status_marks_are_drawn_the_same_everywhere(self):
        ui, ld = src("ms_ui.c"), src("ms_ota_loader.c")
        self.assertIn("#define MARK_RIGHT 590", ui)
        self.assertEqual(ld.count("ms_mark(rows[i].good ? MSST_MARK_GOOD : MSST_MARK_BAD, y)"), 1)     # start screen: the rows of ms_start.h
        self.assertIn("msst_rows(g_drive ? 1 : g_dev[dev].nstock, f, rows)", ld)
        self.assertIn("ms_mark(msst_step_mark(s), 100 + 28 * i)", ld)                                 # set-up checklist
        self.assertIn("ms_mark((devices[MS_VUSB() ? DEV_USB : DEV_SD] && file_exists(MSCARD_RAW)) ? 1 : 2", ld)             # About
        self.assertIn("ms_mark((!in.sd_mounted || in.log_fail) ? 2 : 1", ld)                          # Diagnostics

    def test_drive_autoboot_card_wiring(self):
        ld, ui = src("ms_ota_loader.c"), src("ms_ui.c")
        k = ld.index("int ms_start_screen")
        body = ld[k:k + 1800]
        self.assertLess(body.index("drive_probe()"), body.index("scan_dev(0)"))                    # the drive before any file search
        self.assertIn("msst_drive_probe_allowed(IsWiiU(), isWiiVC", ld)                           # never on a Wii U / Wii VC
        self.assertIn('strcpy(ncfg->GamePath, "di")', ld)                                         # the drive launches as the loader's own 'di' entry
        self.assertIn("msst_setup_iso_drive_text()", ld)                                          # set-up with only a disc in the drive says it plainly
        self.assertIn("msst_autoboot_set_now(msst_setup_needed(f), src, file_exists(MSAUTOBOOT_MARK), ms_sel_saved)", ld)
        self.assertIn("ms_vcard_refresh(); ms_cfg_apply();", ld)                                  # card_virtual success: emulation on
        self.assertIn("ms_cfg_force(ncfg, native_marker > 0, IsWiiU(), ms_vcard_exists())", ui)
        self.assertIn("MSR_FL_VCARD", ui)

    def test_native_control_lines(self):
        ld = src("ms_ota_loader.c")
        self.assertEqual(ld.count("ms_native_text(ms_native_state())"), 2)                            # Controllers and Diagnostics

    def test_setup_reuses_the_card_steps(self):
        ld = src("ms_ota_loader.c")
        k = ld.index("static int setup_step")
        body = ld[k:k + 3000]
        for s in ("card_download(", "card_virtual(", "ota_net_up_retry(", "find_discs"):
            self.assertIn(s, body, s)

    def test_connection_check_runs_by_default(self):
        """ota_conn_check is no longer gated on conn_check_on; only no_conn_check switches it off; the 30 s Wi-Fi network window is unconditional"""
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        body = ld[ld.index("static void ota_conn_check(void)"):]
        body = body[:body.index("conn_screen(l, 1)")]
        self.assertIn("file_exists(MSCONN_OFF)", body)
        self.assertNotIn("file_exists(MSCONN_ON)", body)
        self.assertEqual(ld.count("file_exists(MSCONN_ON)"), 0)       # the Wi-Fi network window (30 s) no longer depends on the marker either
        win = ld[ld.index("if (ota_early) {                                                       /* a Wi-Fi association"):]
        win = win[:win.index("nr = ota_net_up(g_boot_ms, 1);")]
        self.assertIn("if (kd != MSP_NET_WIRED) g_boot_ms = 30000;", win)
        self.assertIn("boot check: connection kind %s", win)

    def test_connection_wording_171(self):
        """Wi-Fi is DECENT, never alarming; a cable is best; wired is the top tier (the boot check never runs on wired, so no 'Wi-Fi' text reaches a wired player)"""
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        body = ld[ld.index("static void ota_conn_check(void)"):]
        body = body[:body.index("conn_screen(l, n);")]
        self.assertIn('return kind == MSP_NET_WIRED ? 0 : 1;', open(os.path.join(OTA, "ms_probe_core.c")).read())   # the Wi-Fi texts below rely on this
        for s in ('"Your Wi-Fi is decent."', '"A cable is best for online play."', '"Your Wi-Fi is unsteady right now."', '"Move closer or use a cable for online play."',
                  '"Wi-Fi: decent"', '"Wi-Fi: decent. A cable is best."', '"Wi-Fi: unsteady. A cable is best."', '"Network: not checked"', '"Could not check your connection."'):
            self.assertIn(s, body, s)
        ne = src("ms_netedit.h")
        for s in ('"Connected. The connection is fine."', '"Connected. The connection is decent. A cable is best."', '"Connected, but unsteady right now. A cable is best."'):
            self.assertIn(s, ne, s)
        for f in ("ms_ota_loader.c", "ms_netedit.h", "ms_ui.c", "ms_start.h", "ms_soak.h"):
            t = src(f)
            for old in ("connection is good", "connection is okay", "connection is poor", "Network: good", "Network: okay", "Network: poor", "Wired is smoother", "Expect stutter", "Games will stutter", "go wired", "Lag spikes"):
                self.assertNotIn(old, t, f + ": " + old)
        for s in ("Wi-Fi: unsteady. A cable is best.", "Wi-Fi: decent. A cable is best."):
            self.assertLess(len(s), 56)                                           # ms_net_banner[56]
        for s in ("Move closer or use a cable for online play.",):
            self.assertLessEqual(len(s), len("Games will stutter. Get closer or go wired."))   # no wider than the 170 line on the centred boot screen


if __name__ == "__main__":
    unittest.main(warnings="ignore")
