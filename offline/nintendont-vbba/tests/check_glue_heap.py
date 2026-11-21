#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Host check of the heap/queue layout of kernel/vbba_glue.c (from patches/0001-multishine-vbba.patch), the class of bug behind the Wii freeze hunt:
   (1) FAIL: the IOS message queue memory must hold its entries (4 bytes each) and the entries must cover the slots;
   (1b) FAIL: root cause of the Wii freeze hunt: every thread_create must get the stack TOP (block + size, like kernel.ld's __di_stack_addr), not the block base:
       with the base the thread's stack grows down into the heap block allocated before it (the queue block) and corrupts it;
   (2) FAIL: no heap block that backs a thread stack or the queue may be smaller than what is asked of it;
   (3) INFO only: heap offsets of the thread stacks mod 64 (the 64-alignment hypothesis was refuted: h1fix2 ran clean at 32 mod 64, h1fix3 tripped at 0).  The offsets are computed from the allocation order with sizeof(struct vg) = SZ_VG and
       sizeof(struct vg_slot) = SZ_SLOT measured with arm-none-eabi-gcc (-DSZ_VG= / -DSZ_SLOT= via environment, defaults of the b1e tree).
   Usage: check_glue_heap.py [--strict] [glue.c]   (default: the glue of the current patch)"""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
if args: src = open(args[0], encoding="latin-1").read()
else:
    s = open(os.path.join(here, "..", "patches", "0001-multishine-vbba.patch"), encoding="latin-1", newline="").read()
    a = s.index("diff --git a/kernel/vbba_glue.c"); b = s.index("diff --git a/kernel/vbba_glue.h", a)
    src = "\n".join(l[1:] for l in s[a:b].split("\n") if l.startswith("+"))
SZ_VG, SZ_SLOT = int(os.environ.get("SZ_VG", 62304)), int(os.environ.get("SZ_SLOT", 192))   # sizeof(struct vg) at VBBA_STEP 9 (37248 at step 1), measured with arm-none-eabi-gcc
def const(name):
    return int(re.search(r"#define %s\s+(\w+)" % name, src).group(1), 0)
slots, stack = const("VBBA_SLOTS"), const("VBBA_STACK")
QSLACK = const("VBBA_QSLACK") if "define VBBA_QSLACK" in src else 0
MIN_SLACK_BYTES = 64
def ev(e):
    e = e.replace("VBBA_SLOTS", str(slots)).replace("VBBA_QSLACK", str(QSLACK)).replace("VBBA_STACK", str(stack)).replace("sizeof(struct vg_slot)", str(SZ_SLOT)).replace("sizeof(struct vg)", str(SZ_VG)).replace("sizeof(ioctlv)", "8")   # ioctlv = { void *data; u32 len } on the ARM
    for n in sorted(set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", e)), key=len, reverse=True):   # any other #define'd numeric constant (e.g. VBBA_NET_STACK)
        m = re.search(r"#define %s\s+\(?\s*(0x[0-9A-Fa-f]+|\d+)u?\s*\)?\s*(/\*|//|$)" % n, src, re.M)
        if m: e = re.sub(r"\b%s\b" % n, str(int(m.group(1), 0)), e)
    return eval(e, {"__builtins__": {}})
allocs = [(m.group(1), ev(m.group(2)), int(m.group(3))) for m in re.finditer(r"(\w+) = \([^)]*\)\s*heap_alloc_aligned\(vgHeap, ([^;]*?), (\d+)\);", src.split("static void vg_start")[1])]
q = re.search(r"mqueue_create\(vgQueueHeap, ([^)]*)\)", src); entries = ev(q.group(1))
rc = 0; off = 0; log = []
HEAP = const("VBBA_HEAP_SIZE") if re.search(r"#define VBBA_HEAP_SIZE\s+\w+", src) else 0x1DC00
for name, size, al in allocs:
    off = (off + al - 1) // al * al
    log.append((name, off, size, al)); off += (size + 31) // 32 * 32
if off > HEAP: print("FAIL the heap allocations need %#x bytes, the heap region (up to the 0x131BE000 status block) has %#x" % (off, HEAP)); rc = 1
qsize = dict((n, s) for n, o, s, a in log)["vgQueueHeap"]
if entries < slots: print("FAIL queue holds %d entries for %d slots" % (entries, slots)); rc = 1
if qsize < 4 * entries: print("FAIL queue memory %d B < 4 x %d entries" % (qsize, entries)); rc = 1
for m in re.finditer(r"thread_create\((\w+), NULL, ([^,]+), ([^,]+), ", src):
    stk, size = m.group(2).strip(), m.group(3).strip()
    if not re.match(r"^\w+ \+ ", stk) or stk.split("+", 1)[1].strip() != size: print("FAIL thread_create(%s): stack argument '%s' is not <block> + <size> (%s): the kernel takes the stack top" % (m.group(1), stk, size)); rc = 1
print("heap use %#x of %#x" % (off, HEAP))
print("slots %d, queue %d entries in %d B, stacks at %s" % (slots, entries, qsize, ", ".join("%s %#x" % (n, o) for n, o, s, a in log if "Stack" in n)))
sys.exit(rc)
