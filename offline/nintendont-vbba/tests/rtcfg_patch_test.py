#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0016 keeps the behaviour when /vbba.cfg has neither netdelay= nor nobba=: every line the patch
removes must come back as an added line that differs only by the new guard (vgCfgNetDelayS / VBBANoBba), and the
new switches must default to 0.  Host check of the patch text (the kernel code itself is not host-built)."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0016-multishine-runtime-switches.patch"), "rb").read().decode("latin1")
rem = [l[1:].rstrip("\r") for l in p.split("\n") if l.startswith("-") and not l.startswith("---")]
add = [l[1:].rstrip("\r") for l in p.split("\n") if l.startswith("+") and not l.startswith("+++")]
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
assert len(rem) <= 8, rem
for r in rem:
    # the removed line must survive inside an added line (the guard is added around or next to it)
    core = re.sub(r"\s+", "", r)
    chk(any(core in re.sub(r"\s+", "", a) or re.sub(r"\s+", "", a).replace("&&!VBBANoBba()", "").replace("||vgCfgNetDelayS", "") == core for a in add), "removed line not preserved: " + r.strip())
txt = "\n".join(add)
chk("static u32 vgCfgNetDelayS, vgCfgNoBba;" in txt, "globals (static = zero initialised)")
chk("!(VBBA_NET_DELAY_S || VBBA_NET_ACT || vgCfgNetDelayS)" in txt, "net start deferral condition")
chk("&& !VBBANoBba())" in txt, "vbbaWanted guard")
chk("if (ok && v <= 600u) vgCfgNetDelayS = v;" in txt, "netdelay range check")
chk("u32 VBBANoBba(void) { return 0; }" in txt, "stub for a build without the vBBA core")
for m in ("netdelay", "nobba"):
    chk(m + "=" in txt, m + " token")
print("rtcfg patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
