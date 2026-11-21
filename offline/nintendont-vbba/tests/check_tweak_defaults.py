#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Build-time proof that the numeric defaults a VBBA_TWEAK list promises are the ones IN FORCE at run time.
   Why: b17c (tweak main1000) was built with -DVBBA_MAIN_US=1000 and the console log said 'main-loop sleep 20 us': the value was compiled in but the code that applies it
   was disabled at VBBA_STEP 1 (the diagnostic builds' step), so the test never ran.  A -D that reaches the compiler is not proof; this script
     1. takes the built tree's kernel/Makefile VBBA_FLAGS and kernel/vbba_glue.c (as patched and tweaked by build_wii.sh),
     2. dumps the macros of that translation unit (cc -E -dM) and checks the ones the tweaks set (yield, idle, priority, main, in-flight, buffers),
     3. compiles the host glue test with exactly those flags and the build's VBBA_STEP, runs it with GLUE_EXPECT and checks the values the glue actually applied
        (status words cfg_idle_us, cfg_prio, cfg_main_us, in-flight cap, buffers, the thread priority, and VBBAMainDelay(20) as the main loop calls it).
   Usage: check_tweak_defaults.py --tree TREE --tweak "plainprobe idle1ms ..." --step N [--out DIR]     exit 0 = every default as promised"""
import argparse, os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__)); d = os.path.dirname(here); root = os.path.dirname(os.path.dirname(d))
ap = argparse.ArgumentParser(); ap.add_argument("--tree", required=True); ap.add_argument("--tweak", default=""); ap.add_argument("--step", type=int, default=9)
ap.add_argument("--out", default=os.path.join(root, "build", "nintendont-vbba", "tweakcheck"))
a = ap.parse_args(); tw = a.tweak.replace("b17d", "plainprobe idle1ms yield1ms inflight1 main1000").split(); os.makedirs(a.out, exist_ok=True)
mk = open(os.path.join(a.tree, "kernel", "Makefile"), encoding="latin-1").read()
line = re.search(r"^VBBA_FLAGS := (.*)$", mk, re.M).group(1)
flags = [t for t in line.split() if t.startswith("-D") and "$(" not in t]
step = a.step
# what the tweak list promises
exp = dict(main=next((int(w[4:]) for w in tw if re.fullmatch(r"main\d+", w)), 0), idle=1000 if "idle1ms" in tw else 50, prio=0x48 if "prio48" in tw else 0x70,
           yield_=1000 if "yield1ms" in tw else 100)
if "tx16" in tw: exp["inflight"] = 16; exp["buffers"] = 16
else:
    exp["buffers"] = 24 if step >= 3 else 8
    exp["inflight"] = 1 if "inflight1" in tw else 2 if "cap2" in tw else (16 if step >= 3 else 8)
G = os.path.join(here, "glue")
import shutil
gs = os.path.join(a.out, "glue_src"); os.makedirs(gs, exist_ok=True)       # copied out of the tree: its own kernel headers would shadow the host shims
for f in ("vbba_glue.c", "vbba_glue.h"): shutil.copy(os.path.join(a.tree, "kernel", f), gs)
glue = os.path.join(gs, "vbba_glue.c")
inc = ["-I" + os.path.join(G, "shim"), "-I" + G, "-I" + os.path.join(d, "core"), "-I" + os.path.join(d, "platform", "nintendont")]
base = ["-DVBBA_WITH_CORE", "-DVBBA_TMO_START_MS=1500", "-DVBBA_TMO_OTHER_MS=600", "-DVBBA_STALL_TICKS=380000u", "-DVBBA_TL_PERIOD_MS=200u", "-DVBBA_HEAP_BASE=((uintptr_t)heap_blk)",
        "-DVBBA_HEAP_SIZE=0x1E000", "-DVBBA_EXT_ADDR=((uintptr_t)status_blk)", "-DVBBA_STATUS_ADDR=((uintptr_t)pub_blk)", "-DVBBA_STEP=%d" % step]
# flags of the build override the test defaults of the same macro
names = {f[2:].split("=")[0] for f in flags}
cflags = [f for f in base if f[2:].split("=")[0] not in names] + flags
rc = 0
dm = subprocess.run(["cc", "-E", "-dM", "-std=gnu99"] + cflags + inc + [glue], capture_output=True, text=True)
macros = dict(re.findall(r"^#define (\w+) (.*)$", dm.stdout, re.M))
def mval(n): return int(re.sub(r"[uUlL]+$", "", macros[n]), 0)
for n, want in (("VBBA_YIELD_US", exp["yield_"]), ("VBBA_IDLE_US", exp["idle"]), ("VBBA_THREAD_PRIO", exp["prio"]), ("VBBA_MAIN_US", exp["main"])):
    got = mval(n) if n in macros else None
    ok = got == want; rc |= 0 if ok else 1
    print("   macro %-18s %s (expected %s) %s" % (n, got, want, "ok" if ok else "MISMATCH"))
exe = os.path.join(a.out, "glue_test_check")
srcs = [glue, os.path.join(G, "kmock.c"), os.path.join(G, "glue_test.c"), os.path.join(d, "core", "vbba_chip.c"), os.path.join(d, "core", "vbba_router.c"),
        os.path.join(d, "platform", "nintendont", "vbba_ndexi.c"), os.path.join(d, "platform", "nintendont", "vbba_tp_ios.c"),
        os.path.join(root, "offline", "bba", "bba_mx98728.c"), os.path.join(root, "offline", "net", "sdnet.c")]
cc = subprocess.run(["cc", "-std=gnu99", "-O1", "-g", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] + cflags + inc + srcs + ["-lpthread", "-o", exe], capture_output=True, text=True)
if cc.returncode: print(cc.stderr[-2000:]); sys.exit(1)
env = dict(os.environ, ASAN_OPTIONS="detect_stack_use_after_return=0", GLUE_EXPECT="main=%d idle=%d prio=%x inflight=%d buffers=%d" % (exp["main"], exp["idle"], exp["prio"], exp["inflight"], exp["buffers"]))
r = subprocess.run(["perl", "-e", "alarm 120; exec {$ARGV[0]} @ARGV", exe], capture_output=True, text=True, env=env)
eff = [l for l in r.stdout.split("\n") if l.startswith("EFFECTIVE")]
print("   " + (eff[0] if eff else "no EFFECTIVE line:\n" + r.stdout[-800:] + r.stderr[-800:]))
rc |= 0 if (r.returncode == 0 and eff and "AS EXPECTED" in eff[0]) else 1
print("tweak defaults [%s] at step %d: %s" % (" ".join(tw), step, "PASS" if rc == 0 else "FAIL"))
sys.exit(rc)
