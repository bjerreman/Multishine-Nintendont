# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""NIN_MS_WIFIFIX: wifi_fix_patch.py applies to the kernel glue once (every anchor unique), keeps the thread's Wi-Fi-only guard, and the header test passes.
Run: python3.14 tests/wifi_fix_patch_test.py   (the apply test needs a built tree: VBBA_GLUE=/path/to/kernel/vbba_glue.c, else the scratch tree of a build under $VBBA_SCRATCH)"""
import glob, os, subprocess, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import wifi_fix_patch as W


def glue_source():
    p = os.environ.get("VBBA_GLUE")
    if p and os.path.exists(p):
        return open(p).read()
    for c in sorted(glob.glob(os.path.join(os.environ.get("VBBA_SCRATCH", "/tmp"), "vbba", "kernel", "vbba_glue.c"))):
        s = open(c).read()
        if "VBBAWifiThread" not in s:
            return s
    return None


class WifiFix(unittest.TestCase):
    def test_header_host_test(self):
        out = os.path.join(os.environ.get("TMPDIR", "/tmp"), "wl_build_test")
        subprocess.check_call(["cc", "-std=c99", "-pedantic", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(HERE, "..", "platform", "nintendont"), "-I",
                               os.path.join(HERE, "..", "core"), os.path.join(HERE, "wl_build_test.c"), "-o", out])
        self.assertIn(b"ok", subprocess.check_output([out]))

    def test_guard_and_shutdown_text(self):
        # the thread must refuse anything but a Wi-Fi interface, and the shutdown must resume what it paused
        self.assertIn("vgCfgKind != 1u", W.FUNCS)
        self.assertLess(W.FUNCS.index("IOS_Ioctl(kdFd, 1"), W.FUNCS.index("vgCfgKind != 1u"))   # the WiiConnect24 pause comes before the Wi-Fi-only guard
        self.assertIn("IOS_Ioctl(kdFd, 3", W.SHUT)
        self.assertIn("thread_cancel(vgWfThread", W.SHUT)
        self.assertNotIn("IOS_IoctlAsync", W.FUNCS)

    def test_applies_once_to_the_glue(self):
        s = glue_source()
        if s is None:
            self.skipTest("no built kernel glue source")
        t = W.patch(s)
        for n in ("VBBAWifiThread", "vg_wf_report();", '#include "vbba_wl.h"'):
            self.assertGreaterEqual(t.count(n), 1, n)
        with self.assertRaises(AssertionError):
            W.patch(t)            # a second application must refuse (the anchors are no longer unique or present)


if __name__ == "__main__":
    unittest.main()
