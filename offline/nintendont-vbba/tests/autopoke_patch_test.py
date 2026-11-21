#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0022: /vbba.cfg autopoke=SECONDS.  Host check of the patch text; that it applies on the full chain and builds is done by build_wii.sh."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0022-multishine-autopoke.patch"), "rb").read().decode("latin1").replace("\r", "")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = [l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---")]
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
chk(not [r for r in rem if r.strip()], "the patch only adds lines (no behaviour removed)")
chk("ExiChanTabAddr = (value << 16) + (u32)(s32)(s16)valueB;" in add, "the EXI channel table address is taken from the patched EXIDMA operands (sign extended addi)")
chk("u32 ExiChanTabAddr = 0;" in add, "address defaults to 0 (not found)")
chk("(e->w0 >> 24) == 0x12 && (e->w0 & 0x00F00000u) == 0" in add, "only a channel 0 DMA is the trigger")
chk("vgCfgAuto = 4u" in add, "default 4 s")
chk('"autopoke="' in add and "v >= 1u && v <= 60u" in add and "v == 0u" in add, "autopoke key: 0 = off, 1..60 s")
chk("vgApRun < 8u" in add and "vgApRun = 0" in add, "at most 8 per silence, reset when the guest talks")
chk("!EXI_IRQ" in add and "(u32)(t - vgApAt) > vgCfgAuto * 1898000u" in add, "not while an interrupt is pending, one per period")
chk("EXIPoke(8u)" in add, "uses the existing delivery path (EXILockTry inside EXIPoke)")
chk("DIAG: exi guest:" in add and "vg_pm_rd(0x800000C4u)" in add, "freeze dump of the guest EXI words")
for tag in ("autopoke #", "DIAG: exi guest:", "cfg:"):
    ln = [l for l in add.split("\n") if tag in l and "dbgprintf" in l]
    chk(ln, tag + " line present")
    if ln:
        fmt = ln[0].split('dbgprintf("', 1)[1].split('"', 1)[0].replace("\\r\\n", "")
        n = len(re.findall(r"%[0-9]*[uxXd]", fmt)); fixed = len(re.sub(r"%[0-9]*[uxXd]", "", fmt))
        chk(fixed + n * 10 < 0xF0, "%s line fits the dbgprintf buffer: %d" % (tag, fixed + n * 10))
print("autopoke patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
