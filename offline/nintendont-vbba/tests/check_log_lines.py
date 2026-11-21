#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Worst-case length of every dbgprintf line of kernel/vbba_glue.c (from patches/0001-multishine-vbba.patch).
   Nintendont's dbgprintf (kernel/vsprintf.c) formats into a 256-byte stack buffer with no bound: a longer line overruns the
   stack of the calling thread.  Every conversion is counted at its widest: %u 10, %d 11, %x/%X 8 (or the field width if larger),
   %c 1, %% 1, %s the longest string the call can pass (string literals among the call's arguments, stepname[]; FAIL if unknown).
   %s of a char array declared in the glue counts as that array's size - 1.  Comment text and a forwarding logging macro are ignored.
   FAIL when any line can reach 256 characters (255 plus the NUL fill the buffer).
   Usage: check_log_lines.py [--verbose] [glue.c]"""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
LIMIT = 256   # the hard bound: 255 characters plus the NUL fit the 256-byte buffer
args = [a for a in sys.argv[1:] if not a.startswith("--")]
verbose = "--verbose" in sys.argv
if args: src = open(args[0], encoding="latin-1").read()
else:
    s = open(os.path.join(here, "..", "patches", "0001-multishine-vbba.patch"), encoding="latin-1", newline="").read()
    a = s.index("diff --git a/kernel/vbba_glue.c"); b = s.index("diff --git a/kernel/vbba_glue.h", a)
    src = "\n".join(l[1:] for l in s[a:b].split("\n") if l.startswith("+"))
def strip_comments(t):      # comment text (it names dbgprintf in prose) and the line structure kept; string literals respected
    out = []; i = 0; q = False
    while i < len(t):
        c = t[i]
        if q:
            out.append(c)
            if c == "\\": out.append(t[i + 1]); i += 2; continue
            if c == '"': q = False
            i += 1; continue
        if c == '"': q = True; out.append(c); i += 1; continue
        if t.startswith("/*", i):
            j = t.find("*/", i + 2); j = len(t) if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in t[i:j])); i = j; continue
        if t.startswith("//", i):
            j = t.find("\n", i); j = len(t) if j < 0 else j
            i = j; continue
        out.append(c); i += 1
    return "".join(out)


src = strip_comments(src)
src = re.sub(r"(?m)^\s*#\s*define\s+\w+\([^)]*\)\s+dbgprintf\s*\(__VA_ARGS__\).*$", "", src)   # a logging macro that forwards to dbgprintf: its callers are checked instead
names = re.search(r"stepname\[\]\s*=\s*\{(.*?)\};", src, re.S)
# char arrays declared in the glue: a %s argument that names one is at most its last dimension - 1 characters (it is NUL terminated there)
CHARBUF = {m.group(1): int(m.group(2)) - 1 for m in re.finditer(r"\bchar\s+(\w+)\s*(?:\[\s*\w+\s*\])*\[\s*(\d+)\s*\]", src)}
STEPMAX = max(len(x) for x in re.findall(r'"((?:[^"\\]|\\.)*)"', names.group(1))) if names else 0

def unesc(t):
    return t.replace('\\r', '\r').replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"').replace('\\\\', '\\')

def call_text(i):      # from 'dbgprintf(' to its closing parenthesis (strings and nesting respected)
    j = src.index("(", i); depth = 0; k = j; q = False
    while True:
        c = src[k]
        if q:
            if c == "\\": k += 1
            elif c == '"': q = False
        elif c == '"': q = True
        elif c == "(": depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0: return src[j + 1:k]
        k += 1

rc = 0; worst = 0; n = 0
for m in re.finditer(r"\bdbgprintf\s*\(", src):
    body = call_text(m.start())
    fm = re.match(r'\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', body)
    if not fm: print("FAIL: dbgprintf without a literal format: %s" % body[:80]); rc = 1; continue
    fmt = unesc("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fm.group(1))))
    rest = body[fm.end():]
    lits = [len(unesc(x)) for x in re.findall(r'"((?:[^"\\]|\\.)*)"', rest)]
    bufs = [CHARBUF[x] for x in re.findall(r"\b(\w+)\s*\[", rest) + re.findall(r"\b(\w+)\b", rest) if x in CHARBUF]
    smax = max(lits + bufs + ([STEPMAX] if "stepname" in rest else []) + [0])
    total = 0; i = 0; nconv_s = 0
    while i < len(fmt):
        if fmt[i] != "%": total += 1; i += 1; continue
        mm = re.match(r"%([-+ #0]*)(\d*)(?:\.(\d+))?([hlL]?)([diuxXcsp%])", fmt[i:])
        if not mm: print("FAIL: unknown conversion in %r" % fmt); rc = 1; break
        w = int(mm.group(2) or 0); c = mm.group(5)
        if c == "%": total += 1
        elif c == "c": total += max(1, w)
        elif c in "du": total += max(11 if c == "d" else 10, w)
        elif c in "xXp": total += max(8, w)
        elif c == "s":
            nconv_s += 1
            if smax == 0: print("FAIL: %%s with unknown string length in %r" % fmt[:60]); rc = 1
            total += max(smax, w)
        i += len(mm.group(0))
    n += 1; worst = max(worst, total)
    if verbose or total >= LIMIT: print("%s %3d  %s" % ("FAIL" if total >= LIMIT else "    ", total, fmt.strip()[:90]))
    if total >= LIMIT: rc = 1
print("dbgprintf lines of the glue: %d, worst case %d characters (limit < %d: buffer 256 with the NUL): %s" % (n, worst, LIMIT, "PASS" if rc == 0 else "FAIL"))
sys.exit(rc)
