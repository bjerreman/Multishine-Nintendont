#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0019: /vbba.cfg netearly=SECONDS (1..30) reuses the netdelay release path (no SD-idle wait) with its own
log lines; absent key = behaviour (the flag is a zero-initialised static, the netdelay and default log texts stay).
Host check of the patch text (the kernel code itself is not host-built)."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0019-multishine-netearly.patch"), "rb").read().decode("latin1")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = [l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---")]
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
chk("static u32 vgCfgNetDelayS, vgCfgNoBba, vgCfgNetEarly;" in add, "flag is a zero-initialised static")
chk("v >= 1u && v <= 30u" in add and "vgCfgNetEarly = 1" in add, "netearly range 1..30 and flag")
chk("netearly=" in add, "token")
chk("net start released EARLY at %u ms (netearly %u s" in add, "release log line")
chk('"netdelay"' in add and "net start released at %u ms (netdelay %u s)" in add, "netdelay log texts kept")
chk(len(rem) <= 6, "few removed lines: %d" % len(rem))
for r in rem:
    if "cfg" in r or "net start deferred until %u s after" in r or "(netdelay %u s)" in r: continue   # log texts the patch rewrites on purpose
    core = re.sub(r"\s+", "", r)
    chk(any(core[:24] in re.sub(r"\s+", "", a) for a in add.split("\n")), "removed line context preserved: " + r.strip()[:60])
print("netearly patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
