#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0021: /vbba.cfg nosave=1 (GCNCard_Save leaves the SD alone, the save counts as done) and sdprobe=1 (SD sector 0 read once a second
through the SD driver, timed, SDPROBE line, slot LED).  Host check of the patch text; that it applies on the full chain and builds is done by build_wii.sh."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0021-multishine-sd-probe-nosave.patch"), "rb").read().decode("latin1")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = [l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---")]
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
# nosave
chk('"nosave="' in add and "vbbaCfgNoSave = v ? 1u : 0u" in add, "nosave key parsed, default 0")
chk("vbbaNoSaveN++" in add, "skipped saves counted")
chk("if (vbbaCfgNoSave) {}" in add.replace("\r", ""), "no SD access in the nosave branch (f_open only in the else)")
chk(not any("BlockOffLow = 0xFFFFFFFF" in r or "changed_system = false" in r for r in rem), "the reset of the dirty offsets after the save is untouched (the save counts as done)")
chk(all(("f_open_char" in r or "if (ret == FR_OK)" in r or "#define MSS_IS_FAT" in r) for r in rem if r.strip()) and len([r for r in rem if r.strip()]) == 3, "only the f_open line, its if and the MSS_IS_FAT macro are rewritten")
# sdprobe
chk('"sdprobe="' in add and "vbbaCfgSdProbe = v ? 1u : 0u" in add, "sdprobe key parsed, default 0")
chk("sdio_ReadSectors(0, 1, vgProbeBuf)" in add and "aligned(32)" in add, "raw SD read of sector 0 into a 32 byte aligned static buffer")
chk("DI_IRQ" in add and "1898000u" in add, "once a second, not while an asynchronous disc read may use the SD")
chk("if (!vbbaCfgSdProbe" in add, "no probe unless asked")
chk("MSS_PROBE) (" not in add and "(s) == MSS_PROBE" in add, "the probe stage counts as an SD stage (solid LED, no STALL line)")
chk("mode = 2u" in add and "3796000u" in add and "vbbaPrBad && mode == 0u) mode = 1u" in add, "LED: solid over 2 s pending, fast blink when the last probe failed or was slow")
chk("949000u" in add, "slow = over 500 ms")
chk("VBBASdProbeTick();" in add, "called from the main loop")
chk("void VBBASdProbeTick(void) {}" in add, "stub for builds without the vBBA core")
# log line lengths (dbgprintf buffer is 0x100 bytes)
for tag in ("SDPROBE:", "cfg:"):
    ln = [l for l in add.split("\n") if tag in l and "dbgprintf" in l]
    chk(ln, tag + " line present")
    if ln:
        fmt = ln[0].split('dbgprintf("', 1)[1].split('"', 1)[0].replace("\\r\\n", "")
        n = len(re.findall(r"%[0-9]*[uxXd]", fmt)); fixed = len(re.sub(r"%[0-9]*[uxXd]", "", fmt))
        chk(fixed + n * 10 < 0xF0, "%s line fits the dbgprintf buffer: %d" % (tag, fixed + n * 10))
print("sdprobe patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
