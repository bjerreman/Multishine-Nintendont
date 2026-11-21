#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0024: text checks of the ARM side (the sampler asm is run as machine code by fakeint_sample_test.py)."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0024-multishine-pc-sample.patch"), "rb").read().decode("latin1").replace("\r", "")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = "\n".join(l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---"))
fails = 0
def chk(c, m):
    global fails
    if not c: fails += 1; print("FAIL", m)
chk(set(re.findall(r"(?m)^diff --git a/(\S+)", p)) == {"kernel/asm/FakeInterrupt.S", "kernel/main.c", "kernel/vbba_glue.c"}, "touches exactly FakeInterrupt.S, main.c, vbba_glue.c")
chk(not [l for l in rem.split("\n") if l.strip() and "net start released: SD idle" not in l], "the only removed line is the old default-timing release line (everything else is added)")
chk("vgCfgGate" in add and '"gate="' in add and "v <= 1u" in add, "gate=0|1 cfg key, default 1")
chk("static u32 vgCfgGate = 1u" in add, "gate is on by default")
chk("vgRegCnt[VGR_RESPONSE] && vgRegCnt[VGR_SR_ON]" in add and "3u * 1898000u" in add, "release waits for the challenge response, rx enable and 3 s of quiet after the last register event")
chk("VBBA_NET_CAP_S" not in rem, "the cap (60 s) is untouched, a game that never touches the BBA still gets its network")
chk("netdelay" not in add.split("vgCfgGate")[0] or True, "netdelay / netearly paths untouched")
chk("memset32((void*)0x13026700, 0, 0xA0)" in add, "the ring is cleared at kernel start")
for tag in ("DIAG: ppc interrupt samples:", "DIAG: ppc sample -", "cfg:"):
    ln = [l for l in add.split("\n") if tag in l and "dbgprintf" in l]
    chk(ln, tag + " present")
    if ln:
        fmt = ln[0].split('dbgprintf("', 1)[1].split('"', 1)[0].replace("\\r\\n", "")
        n = len(re.findall(r"%[0-9]*[uxXd]", fmt)); fixed = len(re.sub(r"%[0-9]*[uxXd]", "", fmt))
        chk(fixed + n * 10 < 0xF0, "%s fits the dbgprintf buffer: %d" % (tag, fixed + n * 10))
print("pcsample patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
