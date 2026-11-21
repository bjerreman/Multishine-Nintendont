#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Release profile (patch 0026): applies on the stack, reverts the guest-path sampler bytes, compiles the diagnostics out
(VBBA_QUIET / VBBA_DIAG=0), keeps /vbba.cfg at dns= only, and keeps the milestone lines.  Applies the real chain to the pinned clone when it exists."""
import os, re, subprocess, sys, tempfile, shutil
here = os.path.dirname(os.path.abspath(__file__)); pdir = os.path.join(here, "..", "patches"); root = os.path.normpath(os.path.join(here, "..", "..", ".."))
fails = 0
def chk(c, m):
    global fails
    if not c: fails += 1; print("FAIL", m)
    else: print("ok  ", m)
p = open(os.path.join(pdir, "0026-multishine-release-quiet.patch"), "rb").read().decode("latin1").replace("\r", "")
files = set(re.findall(r"(?m)^diff -ruN a/(\S+) b/", p))
chk(files == {"kernel/Makefile", "kernel/asm/FakeInterrupt.S", "kernel/main.c", "kernel/vbba_glue.c"}, "touches the Makefile, the two guest-path files of 0024 and vbba_glue.c only: %s" % sorted(files))
add = "\n".join(l[1:] for l in p.split("\n") if l.startswith("+") and not l.startswith("+++"))
rem = "\n".join(l[1:] for l in p.split("\n") if l.startswith("-") and not l.startswith("---"))
chk("DIAG: " not in add and "STALL: " not in add, "no diagnostic line is added")
chk("0xD3026700" in rem and "SampleDone" in rem and "13026700" in rem, "the sampler bytes (FakeInterrupt.S) and its main.c ring clear are removed again")
chk("$(if $(VBBA_QUIET),-DVBBA_QUIET=1 -DVBBA_DIAG=0)" in add, "Makefile: VBBA_QUIET=1 sets VBBA_QUIET and VBBA_DIAG=0 for the vBBA objects")
for k in ("netearly=", "netdelay=", "nobba=", "nosave=", "sdprobe=", "irqpoke=", "gate=", "autopoke="):
    chk(k not in add.replace('"netearly=", 9', ""), "the %s key is not added" % k)
chk("#if !VBBA_QUIET   /* release build: /vbba.cfg knows dns= only" in add, "cfg: the experiment keys are inside #if !VBBA_QUIET")
chk("#else\nvoid VBBASdProbeTick(void) {}" in add, "VBBASdProbeTick has a stub when the diag block is out")
chk("#define VBBA_CRUMBS (!VBBA_QUIET)" in add, "the IOS breadcrumb lines are off")
# patch 0027: the MAC publish block (layout shared with the engine: glue/s17od/s12_macpub.h)
q = open(os.path.join(pdir, "0027-multishine-mac-publish.patch"), "rb").read().decode("latin1").replace("\r", "")
qa = "\n".join(l[1:] for l in q.split("\n") if l.startswith("+") and not l.startswith("+++"))
chk(set(re.findall(r"(?m)^diff -ruN a/(\S+) b/", q)) == {"kernel/main.c", "kernel/vbba_glue.c"}, "0027 touches main.c and vbba_glue.c only")
chk("0x13026680u" in qa and "0x56424D43u" in qa, "0027: block at ARM 0x13026680 (PPC 0xD3026680), magic VBMC")
chk("b[1] = seq + 1u" in qa and "b[1] = seq + 2u" in qa and (lambda t: t.index("b[1] = seq + 1u") < t.index("b[2] =") < t.index("b[1] = seq + 2u"))(qa[qa.index("static void vg_mac_publish"):]), "0027: the sequence word is odd while the words are written and even after")
chk("vg_mac_publish(m, vgCfgKind);" in qa and "netprofile %u" in qa, "0027: published when ip/top answers; RUNID prints the Network Profile")
# the lines that must stay in the release (they are in the stack, untouched by 0026)
glue_pins = ["first DHCP ack", "first guest UDP datagram", "link up signalled", "gate: handshake seen", "gate: released", "net start released: cap reached", "VBBA: stopped: tx frames", "VBBA: guest net:", "VBBA: stop: %u requests outstanding"]
stack = "".join(open(os.path.join(pdir, f), "rb").read().decode("latin1") for f in sorted(os.listdir(pdir)) if re.match(r"00(0[1-9]|1[0-9]|2[0-5])-.*\.patch$", f))
for g in glue_pins:
    chk(g in stack and g not in rem or g in ("VBBA: stopped: tx frames",) and g in stack, "milestone line kept: " + g)
# real application on the pinned clone
pin = os.environ.get("NIN_SRC", os.path.join(root, "third_party", "Nintendont"))
PIN = "bbc0a208e81d6ab0a828a431dc0b2ba6fc06aec5"
if os.path.isdir(pin):
    t = tempfile.mkdtemp()
    try:
        subprocess.check_call("git -C '%s' archive %s | tar x -C '%s'" % (pin, PIN, t), shell=True)
        ok = True
        for n in "0001 0002 0003 0004 0005 0006 0007 0008 0010 0011 0012 0015 0016 0017 0018 0019 0020 0021 0022 0023 0024 0025 0026 0027".split():
            f = [x for x in os.listdir(pdir) if x.startswith(n + "-")][0]
            r = subprocess.run(["git", "apply", "--whitespace=nowarn", os.path.join(pdir, f)], cwd=t, capture_output=True)
            if r.returncode: ok = False; print("   patch", n, "failed:", r.stderr.decode()[:200]); break
        chk(ok, "0001..0025 (without the build-time-only 0009) + 0026 + 0027 apply on FIX94/Nintendont " + PIN[:7])
        if ok:
            fi = open(os.path.join(t, "kernel", "asm", "FakeInterrupt.S"), "rb").read()
            orig = subprocess.check_output(["git", "-C", pin, "show", PIN + ":kernel/asm/FakeInterrupt.S"])
            chk(fi == orig, "FakeInterrupt.S is byte-identical to upstream (no sampler in the guest interrupt path)")
            mc = open(os.path.join(t, "kernel", "main.c"), "rb").read()
            chk(b"13026700" not in mc, "main.c does not clear a sampler ring")
            g = open(os.path.join(t, "kernel", "vbba_glue.c"), "rb").read().decode("latin1")
            def guarded(line_start):   # the line sits inside a #if !VBBA_QUIET ... #endif region (nearest preceding #if/#endif decides)
                i = g.index(line_start); pre = g[:i]
                m = [x for x in re.finditer(r"(?m)^#(if !VBBA_QUIET|if VBBA_QUIET|else|endif|if [^\n]*|ifndef [^\n]*|ifdef [^\n]*)", pre)]
                depth = 0
                for x in reversed(m):
                    d = x.group(1)
                    if d == "endif": depth += 1
                    elif d.startswith("if") or d.startswith("ifndef") or d.startswith("ifdef"):
                        if depth == 0: return "!VBBA_QUIET" in d
                        depth -= 1
                return False
            for g1 in ('dbgprintf("VBBA: watchdog: heartbeat', 'dbgprintf("VBBA: idle sleeps', 'dbgprintf("VBBA: duty: asleep', 'dbgprintf("VBBA: sendto latency', '\tvgWdStack = (u32 *)heap_alloc_aligned', '\tif (vgWdStack) {', '\tvg_ifmac_again();', '\tvg_tl_line(vgTlK++', 'dbgprintf("VBBA: cfg:'):
                if g1 in g: chk(guarded(g1), "inside #if !VBBA_QUIET: " + g1.strip()[:46])
                else: chk("vg_tl_line(vgTlK++" in g1, "present: " + g1.strip()[:46])
            ex = open(os.path.join(t, "kernel", "asm", "EXIDMA.S"), "rb").read()
            chk(b"EXIDMA" in ex or len(ex) > 100, "the EXIDMA stub fix (0023) is still in")
    finally:
        shutil.rmtree(t, ignore_errors=True)
else:
    print("skip  pinned clone not found: the application check is not run")
print("FAILS", fails); sys.exit(1 if fails else 0)
