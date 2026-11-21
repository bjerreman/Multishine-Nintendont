#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""The teardown of the in-place hand-over (ms_chain.c / ms_chain_steps.h): the default set holds only calls that cannot block, every step is logged before and after in the order of the table, and
the blocking suspects (GX_DrawDone, the STM / ES closes on stale file descriptors) are not in the default set.  Run: python3.14 test_chain_steps.py  (needs cc)"""
import os, re, subprocess, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
BIN = os.path.join(tempfile.gettempdir(), "ms_chain_steps_bin")


class Steps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I" + OTA, os.path.join(HERE, "chain_steps_test.c"), "-o", BIN])

    def plan(self, mode):
        return subprocess.check_output([BIN, mode]).decode().split()

    def test_default_set_cannot_block(self):
        self.assertEqual(self.plan("default"), ["AUDIO_StopDMA", "VIDEO_Flush", "GX_Flush"])

    def test_full_set_in_order(self):
        self.assertEqual(self.plan("full"), ["AUDIO_StopDMA", "VIDEO_Flush", "GX_Flush", "GX_DrawDone", "__STM_Close", "__ES_Close", "__exception_closeall", "__lwp_thread_closeall"])

    def test_every_step_is_crumbed_before_and_after_and_the_source_runs_them_from_the_table(self):
        src = open(os.path.join(OTA, "ms_chain.c")).read()
        a = src.index("for (i = 0; i < MSCH_NSTEPS; i++)")
        body = src[a:a + 1500]
        self.assertLess(body.index('CRUMB("teardown: %s ..."'), body.index("switch (i)"))
        self.assertLess(body.index("switch (i)"), body.index('CRUMB("teardown: %s done"'))
        for name, case in (("AUDIO_StopDMA", "MSCH_AUDIO"), ("VIDEO_Flush", "MSCH_VIDEO"), ("GX_Flush", "MSCH_GXFLUSH"), ("GX_DrawDone", "MSCH_GXDONE"), ("__STM_Close", "MSCH_STM"),
                           ("__ES_Close", "MSCH_ES"), ("__exception_closeall", "MSCH_EXC"), ("__lwp_thread_closeall", "MSCH_THR")):
            self.assertRegex(body, r"case %s: %s\(\)" % (case, re.escape(name)))
        # the crumb order of the whole hand-over: plan, image words, args, the teardown, step 4 (teardown done), step 5 (tramp copied, SD shut down)
        order = [src.index(k) for k in ('"chain step 1:', '"chain step 2:', '"chain step 3:', 'msch_steps[i].name', '"chain step 4:', '"chain step 5:')]
        self.assertEqual(order, sorted(order))
        self.assertLess(src.index('"chain step 5:'), src.index("hk->shutdown()"))     # the SD is closed after the last crumb and before the tramp


if __name__ == "__main__":
    unittest.main(warnings="ignore")
