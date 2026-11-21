#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0023: text checks of the ARM-side part (the stub itself is run as machine code by exidma_stub_test.py)."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0023-multishine-exidma-keep-cb.patch"), "rb").read().decode("latin1").replace("\r", "")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = "\n".join(l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---"))
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
chk(set(re.findall(r"(?m)^diff --git a/(\S+)", p)) == {"kernel/EXI.c", "kernel/EXI.h", "kernel/asm/EXIDMA.S", "kernel/vbba_glue.c"}, "touches exactly EXI.c, EXI.h, EXIDMA.S, vbba_glue.c")
chk("exiTcCmdWait++" in add and "(w >> 24) == 0x12" in add and "EXIInterrupt();" in p, "TC interrupts sent with a command waiting are counted (counting only, EXIInterrupt() unchanged)")
chk(rem.count("EXIInterrupt") == 0 and "stw" not in rem.replace("stw\t\tr4", ""), "EXIInterrupt() and the existing stores are not removed")
chk("static u32 vgCfgAuto = 4u," in rem and "static u32 vgCfgAuto = 0u," in add, "autopoke default is 0 (off): the lost-callback fix, not a re-thrown interrupt, is the cure")
chk("cmpwi\t\tr7,\t0" in add and "beq\t\tCallbackKeep" in add and "mfmsr\t\tr9" in add and "mtmsr\t\tr10" in add and "mtmsr\t\tr9" in add, "stub: conditional callback store, interrupts off around the mask update, MSR restored")
ln = [l for l in add.split("\n") if "DIAG: exi overlap:" in l and "dbgprintf" in l]
chk(ln, "overlap line present")
if ln:
    fmt = ln[0].split('dbgprintf("', 1)[1].split('"', 1)[0].replace("\\r\\n", "")
    n = len(re.findall(r"%[0-9]*[uxXd]", fmt)); fixed = len(re.sub(r"%[0-9]*[uxXd]", "", fmt))
    chk(fixed + n * 10 < 0xF0, "overlap line fits the dbgprintf buffer: %d" % (fixed + n * 10))
print("exifix patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
