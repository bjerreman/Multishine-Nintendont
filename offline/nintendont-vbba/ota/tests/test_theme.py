#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the shell look: the launch progress mapping (ms_progress.h, pure C) and the generated background (gen_shell_theme.py): flat navy, the mark has the aspect of the
site's SVG mark, sits in the lower third centred on x 320, deterministic output.  Run: python3.14 test_theme.py   (needs cc)."""
import math, os, re, subprocess, sys, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(OTA, "..", "..", ".."))
sys.path.insert(0, OTA)
sys.path.insert(0, os.path.join(REPO, "tools"))
import gen_shell_theme as T      # noqa: E402


def svg_mark_aspect():
    """width / height of the mark as the site draws it (polygon points with their stroke), from _logo_svg's own markup"""
    import site_brand
    svg = site_brand._logo_svg()
    k = float(re.search(r"scale\(([\d.]+)\)", svg).group(1))
    xs, ys = [], []
    for pts, sw in re.findall(r'<polygon points="([^"]+)"[^>]*stroke-width="([\d.]+)"', svg):
        for p in pts.split():
            x, y = (float(v) for v in p.split(","))
            xs += [100 + (x - 100) * k - float(sw) * k / 2, 100 + (x - 100) * k + float(sw) * k / 2]
            ys += [100 + (y - 100) * k - float(sw) * k / 2, 100 + (y - 100) * k + float(sw) * k / 2]
    return (max(xs) - min(xs)) / (max(ys) - min(ys))


class Progress(unittest.TestCase):
    def test_mapping(self):
        bin_ = os.path.join(tempfile.gettempdir(), "ms_progress_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I" + OTA, os.path.join(HERE, "progress_test.c"), "-lm", "-o", bin_])
        r = subprocess.run([bin_], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout)


class Background(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rgb = T.background_rgb()

    def lit(self):
        W = T.W
        px = [(i // 3 % W, i // 3 // W) for i in range(0, len(self.rgb), 3) if tuple(self.rgb[i:i + 3]) != T.NAVY]
        return px

    def test_flat_navy_and_nothing_else_but_cyan_blends(self):
        self.assertEqual(tuple(self.rgb[0:3]), T.NAVY)
        for i in range(0, len(self.rgb), 3):
            r, g, b = self.rgb[i:i + 3]
            if (r, g, b) == T.NAVY:
                continue
            # every other pixel is a blend of navy and the mark's cyan
            t = (b - T.NAVY[2]) / (T.CYAN[2] - T.NAVY[2])
            self.assertTrue(-0.01 <= t <= 1.01)
            for c in range(3):
                self.assertAlmostEqual(self.rgb[i + c], T.NAVY[c] + t * (T.CYAN[c] - T.NAVY[c]), delta=2)

    @unittest.skipUnless(os.path.exists(os.environ.get("MS_SITE_BRAND", "")), "the website's brand module is not in this tree")
    def test_mark_has_the_svgs_aspect(self):
        px = self.lit()
        xs = [p[0] for p in px]; ys = [p[1] for p in px]
        w, h = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        self.assertAlmostEqual(w / h, svg_mark_aspect(), delta=0.03 * svg_mark_aspect(), msg="texture %dx%d" % (w, h))

    def test_mark_is_centred_in_the_lower_third(self):
        px = self.lit()
        xs = [p[0] for p in px]; ys = [p[1] for p in px]
        self.assertAlmostEqual((min(xs) + max(xs)) / 2, T.LOGO_CX, delta=2)
        self.assertAlmostEqual((min(ys) + max(ys)) / 2, T.LOGO_CY, delta=3)
        self.assertGreaterEqual(min(ys), 2 * T.H // 3)
        self.assertLessEqual(max(ys), T.H - 24)
        self.assertLessEqual(max(xs) - min(xs) + 1, T.LOGO_W)

    def test_deterministic_and_png_is_640x480(self):
        a = T.png(self.rgb)
        self.assertEqual(a, T.png(T.background_rgb()))
        self.assertEqual(a[16:24], (640).to_bytes(4, "big") + (480).to_bytes(4, "big"))


class Font(unittest.TestCase):
    def test_every_ascii_glyph_is_in_aileron(self):
        cps = T.cmap_codepoints()
        self.assertTrue(all(c in cps for c in range(0x20, 0x7F)))

    def test_layout_rules_of_the_loader(self):
        from PIL import ImageFont
        f = ImageFont.truetype(T.TTF, 16)
        cps = T.cmap_codepoints()
        x = dict((i, dx) for i, (dx, ch) in enumerate(T.layout("A   : x", 16, f, cps)[0]))
        self.assertEqual(x[1], 40)                      # ':' sits at column 4 of the old 10 px grid
        self.assertEqual(T.layout("X/1 : x", 16, f, cps)[0][3][0], 40)      # one space before ':' counts as a run
        w1 = T.layout("Rev:      01", 16, f, cps)[0][-2][0]
        self.assertEqual(w1, 100)                       # the value column of the game info screen
        self.assertEqual(T.layout("あ", 16, f, cps)[0][0][1], "?")      # a glyph Aileron lacks
        self.assertGreater(T.layout("Multishine", 16, f, cps)[1], 50)


class ButtonFlow(unittest.TestCase):
    """the footers and keys of every screen (stage_loader.py, ms_ui.c, ms_ota_loader.c) and the mandatory boot check"""
    def src(self, n):
        return open(os.path.join(OTA, n)).read()

    def test_footers(self):
        st, ld = self.src("stage_loader.py"), self.src("ms_ota_loader.c")
        st_plain = st.replace("\\", "")
        for s in ('"A", "Select", "Start", "About", "X", "Memory card", "Hold B", "Exit"',        # the fallback device screen
                  '"B", "Back", "A", "Select", "Y", "Settings"'):                                                   # the game list: no Game Version key any more
            self.assertIn(s, st_plain, s)
        self.assertNotIn("Game Version", st_plain)
        self.assertIn('ms_foot(0, "B", "Back", "A", "Check for updates", "X", "Memory card", "Y", "Problem report", "Start", "Reset", NULL)', ld)
        self.assertIn('ms_foot(0, "B", "Back", "A", "Virtual card", "X", "Slot A", NULL)', ld)       # the Memory card screen: slot A only (slot B is shown, never written)
        self.assertNotIn('"Y", "Slot B"', ld)
        self.assertNotIn("ms_footer", st + ld)
        self.assertEqual(ld.split("ms_about(void)")[1].split("the start screen and the quick set-up")[0].count("FPAD_Start(0)"), 1)   # Start = Reset on the About page

    def test_boot_check_is_mandatory_and_no_pad_text_on_screen(self):
        ld = self.src("ms_ota_loader.c")
        self.assertNotIn("auto_off", ld)
        self.assertNotIn("Auto update", ld)
        self.assertNotIn(":pad", self.src("ms_ui.c"))


class SayWindow(unittest.TestCase):
    """ms_say.h: the scrolling status window of the multi-line screens (pure C, tests/say_test.c)"""
    def test_ring_and_window(self):
        bin_ = os.path.join(tempfile.gettempdir(), "ms_say_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I" + OTA, os.path.join(HERE, "say_test.c"), "-o", bin_])
        r = subprocess.run([bin_], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)


class OldCardBackup(unittest.TestCase):
    """ms_bak.h: rename the old card file, else keep it by a verified copy, else an error with nothing changed (pure C, tests/bak_test.c)"""
    def test_decision_logic(self):
        bin_ = os.path.join(tempfile.gettempdir(), "ms_bak_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I" + OTA, os.path.join(HERE, "bak_test.c"), "-o", bin_])
        r = subprocess.run([bin_], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)


if __name__ == "__main__":
    unittest.main(warnings="ignore")
