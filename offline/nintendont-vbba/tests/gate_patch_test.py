#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0025: the gate releases the net start 3 s after the guest's BBA init handshake.  Text checks plus a model of the decision with the failure timeline (ndebug_50)."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0025-multishine-gate-handshake.patch"), "rb").read().decode("latin1").replace("\r", "")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
fails = 0
def chk(c, m):
    global fails
    if not c: fails += 1; print("FAIL", m)
    else: print("ok  ", m)
chk(set(re.findall(r"(?m)^diff --git a/(\S+)", p)) == {"kernel/vbba_glue.c"}, "touches only vbba_glue.c")
chk("vgHsTick = read32(HW_TIMER) | 1u" in add and "vgRegCnt[VGR_RESPONSE] && vgRegCnt[VGR_SR_ON]" in add, "the handshake tick is taken once, when the challenge response and rx enable have both been seen")
rel = [l for l in add.split("\n") if "gate: released" in l]
chk(rel and "vgRegLastTick" not in rel[0] and "last_chg" not in rel[0] and "SD idle" not in rel[0], "the release line does not look at register quiet time or SD activity")
chk(rel and "3u * 1898000u" in rel[0] and "VBBA_NET_MIN_S" in rel[0], "release = handshake + 3 s (and not before the minimum)")
chk('gate: handshake seen at' in add and 'gate: released at' in add and "gate: no handshake, cap" in add, "the gate transitions are logged once each (handshake seen, released, cap)")
chk(add.index("VBBA_NET_CAP_S") < add.index("else if (vgCfgGate)") if "VBBA_NET_CAP_S" in add else True, "the cap line stays before the gate")
for tag in ("gate: handshake seen", "gate: released", "gate: no handshake"):
    ln = [l for l in add.split("\n") if tag in l and "dbgprintf" in l]
    chk(ln, tag + " line present")
    if ln:
        fmt = ln[0].split('dbgprintf("', 1)[1].split('"', 1)[0].replace("\\r\\n", "")
        n = len(re.findall(r"%[0-9]*[uxXd]", fmt)); fixed = len(re.sub(r"%[0-9]*[uxXd]", "", fmt))
        chk(fixed + n * 10 < 0xF0, "%s fits the buffer: %d" % (tag, fixed + n * 10))
# model
def release(ms, hs_ms, cap=60000, minms=5000):
    if ms >= cap: return "cap"
    if hs_ms is not None and ms >= minms and ms - hs_ms >= 3000: return "gate"
    return None
def first(hs_ms):
    ms = 0
    while ms < 120000:
        r = release(ms, hs_ms)
        if r: return ms, r
        ms += 10
chk(first(32700) == (35700, "gate"), "ndebug_50: handshake complete at 32.7 s -> released at 35.7 s (waited for the cap at 60.1 s)")
chk(first(16500) == (19500, "gate"), "normal timing: handshake at 16.5 s -> released at 19.5 s")
chk(first(None) == (60000, "cap"), "no handshake: the cap still releases at 60 s")
chk(first(58000) == (60000, "cap"), "a very late handshake: the cap wins")
print("gate patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
