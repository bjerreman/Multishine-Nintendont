#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0020: the dbgprintf Hollywood 0x0D800070 bit-0 test no longer ends the log for good, SD sector writes are retried
(result unchanged: RES_OK, as the old bool < 0 test), SD command errors are counted and shown in the SDERR line.
Host check of the patch text and, when git is there, that the patch applies on a copy of the previous patched tree is done by build_wii.sh."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
p = open(os.path.join(here, "..", "patches", "0020-multishine-sd-log-diag.patch"), "rb").read().decode("latin1")
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = [l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---")]
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
chk("0xd800070" in add and "dbgBitN++" in add, "the register test is counted")
chk("return -1" not in add, "no early return added")
chk(len([r for r in rem if "return -1" in r]) == 1 and len([r for r in rem if "0xd800070" in r]) == 1, "the old test (and only it) is removed")
chk("while( !sdio_WriteSectors( sector, count, buff ) )" in add and "wtry = 5" in add, "write retried at most 5 times")
chk("return RES_ERROR" not in add, "a failed write still ends as RES_OK (old behaviour, the old test never fired)")
chk("vbbaSdErrN++" in add and "vbbaSdErrRet" in add, "SD command errors counted, first one kept")
chk("LOGBIT:" in add and "SDERR:" in add, "log lines")
sderr = [l for l in add.split("\n") if "SDERR:" in l]
chk(sderr and len(sderr[0]) < 480, "SDERR format")
# SDERR line length at run time: fixed text + 12 numbers of at most 10 digits stays below the 0x100 dbgprintf buffer
fixed = len(re.sub(r"%[0-9]*[uxXd]", "", sderr[0].split('"')[1].replace("\\r\\n", "")))
chk(fixed + 12 * 10 < 0xF0, "SDERR line fits the 256 byte dbgprintf buffer: %d" % (fixed + 120))
note = [l for l in add.split("\n") if "LOGBIT:" in l]
fixed = len(re.sub(r"%[0-9]*[uxXd]", "", note[0].split('"')[1].replace("\\r\\n", "")))
chk(fixed + 4 * 10 < 0xF0, "LOGBIT line fits: %d" % (fixed + 40))
print("sdlog patch test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
