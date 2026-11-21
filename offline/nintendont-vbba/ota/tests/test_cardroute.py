#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the set-up's memory card route and the block figure (ota/ms_cardroute.h): the C decisions (tests/cardroute_test.c), every part of MSCR_FULL_BLOCKS recomputed from its source so the
one figure cannot drift silently, no other hard-coded install size and no 'MiB' in player text of the loader, and the wiring (probe, set-up, Memory card screen, start).
Run: python3.14 test_cardroute.py   (needs cc)"""
import glob, os, re, struct, subprocess, sys, tempfile, unittest, warnings

warnings.simplefilter("ignore")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(OTA, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "card"))


def src(n):
    return open(os.path.join(OTA, n)).read()


def define(text, name):
    m = re.search(r"#define\s+%s\s+([0-9]+)u?\b" % name, text)
    return int(m.group(1)) if m else None


H = src("ms_cardroute.h")
FILES, PACK, DATA = define(H, "MSCR_FILES_BLOCKS"), define(H, "MSCR_PACK_BLOCKS"), define(H, "MSCR_DATA_BLOCKS")


def main_repo():
    """the main checkout (dist/ is not in git: a worktree reads the built containers from there)"""
    try:
        common = subprocess.check_output(["git", "-C", REPO, "rev-parse", "--path-format=absolute", "--git-common-dir"], text=True).strip()
        return os.path.dirname(common)
    except Exception:
        return REPO


class Logic(unittest.TestCase):
    def test_decisions(self):
        b = os.path.join(tempfile.gettempdir(), "ms_cardroute_test_bin")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-I" + OTA, os.path.join(HERE, "cardroute_test.c"), "-o", b])
        r = subprocess.run([b], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-400:])


class Figure(unittest.TestCase):
    def test_sum(self):
        self.assertEqual((FILES, PACK, DATA), (151, 30, 6))
        self.assertIn("(MSCR_FILES_BLOCKS + MSCR_PACK_BLOCKS + MSCR_DATA_BLOCKS)", H)

    def test_five_files_from_the_newest_card_container(self):
        import crd_data as C
        root = main_repo()
        found = sorted(glob.glob(os.path.join(root, "dist", ".crd_*", "crd-*.mscf")) + glob.glob(os.path.join(REPO, "dist", ".crd_*", "crd-*.mscf")),
                       key=lambda p: int(re.search(r"crd-(\d+)\.mscf$", p).group(1)))
        if not found:
            self.skipTest("no built crd-N.mscf in dist/ (built by the release)")
        b = open(found[-1], "rb").read()
        k = b.index(C.MAGIC)
        total = struct.unpack_from(">I", b, k + 8)[0]
        gcis = C.parse_payload(b[k:k + total])
        names = [g[8:40].split(b"\0")[0].decode() for g in gcis]
        self.assertEqual(tuple(names), C.NAMES)
        blocks = sum((len(g) - C.GCI_HDR) // 0x2000 for g in gcis)
        self.assertEqual(blocks, FILES, "%s holds %d blocks: change MSCR_FILES_BLOCKS in ms_cardroute.h" % (os.path.basename(found[-1]), blocks))

    def test_five_names_agree(self):
        import crd_data as C
        names = re.findall(r'"([^"]+)"', H.split("mscr_our_names(void)")[1].split("};")[0])
        self.assertEqual(len(names), define(H, "MSCR_NOURS"))
        self.assertEqual(tuple(names[:5]), C.NAMES)
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', src("ms_card_core.c").split("NAMES[MSCRD_NFILES] = {")[1].split("};")[0])), C.NAMES)
        self.assertEqual(names[5], "MultishinePack")
        self.assertIn('#define MSPK_NAME       "MultishinePack"', src("ms_pack_core.h"))
        data = names[6:]
        self.assertEqual(len(data), DATA)
        game = ""
        for d in [x for x in os.environ.get("MS_GAME_DIRS", "").split(":") if x]:     # the game-side sources, when a checkout of them is given
            for p, _, fs in os.walk(os.path.join(os.environ.get("MS_REPO", REPO), d)):
                if "/tests" in p:
                    continue
                for f in fs:
                    if f.endswith((".c", ".h", ".py")):
                        game += open(os.path.join(p, f), errors="replace").read()
        if not game:
            self.skipTest("cross-check with the game's card files: set MS_REPO to a checkout of the multishine repository")
        for n in data:
            self.assertIn('"%s"' % n, game, "%s is no longer a card file of the game: change MSCR_DATA_BLOCKS and the list" % n)
        self.assertNotIn("MultishineBridge", names)                      # lab only, never shipped

    def test_pack_size_follows_the_release(self):
        rel = ""
        for p in glob.glob(os.path.join(REPO, "tools", "release", "*")):
            if os.path.isfile(p):
                rel += open(p, errors="replace").read()
        slmap = "--with-slmap" in rel or "with_slmap=True" in rel
        self.assertEqual(PACK, 34 if slmap else 30, "the console pack is 30 blocks, 34 with the stage select entry")
        doc = os.environ.get("MS_MAP_NOTE", "")     # an optional design note
        if os.path.exists(doc):
            self.assertIn("lean (console) pack 30 -> 34 blocks", open(doc).read())
        self.assertLessEqual(PACK, define(src("ms_pack_core.h"), "MSPK_MAX_PACK_BLOCKS"))
        self.assertLessEqual(FILES + PACK + DATA, define(H, "MSCR_VCARD_BLOCKS"))


class PlayerText(unittest.TestCase):
    STR = re.compile(r'(?:sayf\([^,]+,\s*[A-Z_x0-9]+,\s*|PrintFormat\([^"]*|put_c\([^"]*|ShowMessageScreen\(|setup_fail\(|snprintf\(su\.[a-z]+, sizeof su\.[a-z]+, )"([^"]*)"')

    def strings(self):
        out = []
        for f in ("ms_ota_loader.c", "ms_ui.c"):
            out += self.STR.findall(src(f))
        for f in ("ms_start.h", "ms_cardroute.h", "ms_slotb.h"):
            out += re.findall(r'return "([^"]*)"|snprintf\(o, n, "([^"]*)"', src(f))
        flat = []
        for s in out:
            flat += [x for x in (s if isinstance(s, tuple) else (s,)) if x]
        return flat

    def test_no_mib_for_the_card(self):
        bad = [s for s in self.strings() if re.search(r"\bMiB\b", s)]
        self.assertEqual(bad, [], "player text says MiB")

    def test_never_59(self):
        bad = [s for s in self.strings() if re.search(r"\b59\b", s)]
        self.assertEqual(bad, [])

    def test_one_place_for_the_figure(self):
        for f in ("ms_ota_loader.c", "ms_start.h", "ms_ui.c"):
            t = src(f)
            for n in ("187", "191", "151 free", "151 blocks"):
                self.assertNotIn(n, " ".join(self.STR.findall(t)) if f.endswith(".c") else t, "%s: %s hard-coded" % (f, n))


class Wiring(unittest.TestCase):
    def test_loader(self):
        ld, st = src("ms_ota_loader.c"), src("stage_loader.py")
        self.assertIn('"ms_cardroute.h"', st)
        self.assertIn('#include "ms_cardroute.h"', ld)
        probe = ld[ld.index("static void card_probe(s32 chn"):]
        probe = probe[:probe.index("\n}\n")]
        self.assertIn("MSCR_PROBE_MS", probe)                             # time-bounded
        self.assertIn("FPAD_Cancel(0)", probe)                            # B stops it
        for w in ("CARD_Create", "CARD_Write", "CARD_Delete", "CARD_Format", "CARD_SetStatus"):
            self.assertNotIn(w, probe)                                    # the probe never writes
        self.assertIn("CARD_Unmount(chn)", probe)
        su = ld[ld.index("static void setup_route("):]
        su = su[:su.index("\n}\n")]
        self.assertIn("mscr_probe_allowed(con) && !f->card", su)          # sensed only with ports and no virtual card
        self.assertIn("card_probe(0, &a); card_probe(1, &b);", su)
        self.assertIn("ota_log(\"setup: memory card sense:", su)           # the result and the route in ota.log
        self.assertIn("d.route == MSCR_R_ASK ? setup_ask(&d, &a, &b) : d.route", su)
        self.assertIn("if (msst_plan(&f0) & MSST_CARD) setup_route(&f0);", ld)
        self.assertIn("su.plan = su.phys ? msst_plan_phys(&f) : msst_plan(&f);", ld)
        step = ld[ld.index("static int setup_step("):]
        step = step[:step.index("\n}\n")]
        self.assertIn("r = card_physical(0);", step)                      # slot A only
        self.assertNotIn("card_physical(1)", ld)                           # slot B is never written
        self.assertNotIn('"Y", "Slot B"', ld)
        self.assertIn("write_file(MSPHYS_MARK", step)
        phys = ld[ld.index("static int card_physical(s32 chn)"):]
        phys = phys[:phys.index("\n}\n")]
        self.assertLess(phys.index("mscr_short("), phys.index("CARD_Delete"))  # the space check before any erase
        self.assertLess(phys.index("ask_a_or_b()"), phys.index("CARD_Delete"))  # and the A-confirm naming our files
        self.assertIn("mscr_need_text(", phys)
        mc = ld[ld.index("static void MemoryCardMenu(void)"):]
        mc = mc[:mc.index("\n}\n")]
        self.assertIn("card_probe(0, &sa); card_probe(1, &sb);", mc)
        self.assertIn("mscr_slot_text(b, sizeof b, 'B', &sb)", mc)
        self.assertIn("ports && FPAD_X(0)", mc)
        self.assertIn("mscr_need_text(", mc)
        self.assertIn("ncfg->Config &= ~NIN_CFG_MEMCARDEMU;", ld.split("static void start_commit(")[1].split("\n}\n")[0])
        self.assertIn("ncfg->Config &= ~NIN_CFG_MEMCARDEMU;", ld.split("void ms_autoboot_fix(void)")[1].split("\n}\n")[0])
        self.assertIn("f->phys = file_exists(MSPHYS_MARK);", ld)
        self.assertIn('"physical_card"', src("ms_reset.h"))


if __name__ == "__main__":
    unittest.main(verbosity=1)
