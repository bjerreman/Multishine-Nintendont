#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Repository lint (stdlib only; uses shellcheck / pyflakes when they are installed, else built-in checks).

  python3 tools/lint_repo.py [--licence SPDX[,SPDX...]] [--strict]

Checks every tracked file (git ls-files):
  * SPDX line in the first 20 lines of every source file (.c .h .S .s .py .sh .inc .ld), value in the accepted list
  * shell scripts: `bash -n` (and shellcheck -S error when installed); executable bit on files with a #! line
  * Python: compiles (py_compile), no unused imports (small AST check; pyflakes when installed)
  * no CR/LF line endings, no absolute home paths (/Users/<name>, /home/<name>), no private-key blocks
  * binaries: listed with their size (the README names the allowed ones)
Exit 1 when an error is found (warnings are printed and do not fail unless --strict)."""
import argparse
import ast
import os
import py_compile
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC = (".c", ".h", ".S", ".s", ".py", ".sh", ".inc", ".ld")
SPDX = re.compile(r"SPDX-License-Identifier:\s*([A-Za-z0-9.+\- ()]+?)\s*(?:\(|\*/|$|-->|\s{2,}|\s-\s)")
HOME = re.compile(r"/(Users|home)/(?!USER\b|runner\b|name\b|user\b)[A-Za-z][A-Za-z0-9._-]+")
KEY = re.compile(r"-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----")
GEN = re.compile(r"(?i)generated")


def files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-s", "-z"], capture_output=True, check=True).stdout.decode()
    for ent in out.split("\0"):
        if ent:
            meta, path = ent.split("\t", 1)
            mode = meta.split()[0]
            if mode != "160000":
                yield path, mode


def unused_imports(src, path):
    try:
        tree = ast.parse(src, path)
    except SyntaxError:
        return []
    imported = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                imported[(a.asname or a.name).split(".")[0]] = node.lineno
        elif isinstance(node, ast.ImportFrom) and node.module != "__future__":
            for a in node.names:
                if a.name != "*":
                    imported[a.asname or a.name] = node.lineno
    used = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Name):
            used.add(node.id)
        elif isinstance(node, ast.Attribute):
            n = node
            while isinstance(n, ast.Attribute):
                n = n.value
            if isinstance(n, ast.Name):
                used.add(n.id)
    text_all = re.findall(r"__all__\s*=\s*[\[(]([^\])]*)", src)
    for t in text_all:
        used.update(re.findall(r"['\"]([A-Za-z_][A-Za-z0-9_]*)['\"]", t))
    lines = src.splitlines()
    out = []
    for name, ln in imported.items():
        if name in used:
            continue
        if ln - 1 < len(lines) and ("noqa" in lines[ln - 1]):
            continue
        out.append((ln, name))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--licence", default="GPL-3.0-or-later,GPL-3.0-or-later OR GPL-2.0-only,MIT,CC0-1.0")
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args()
    ok_lic = [x.strip() for x in a.licence.split(",")]
    err, warn = [], []
    shells, pys = [], []
    nsrc = nlic = 0
    for path, mode in files():
        p = os.path.join(ROOT, path)
        with open(p, "rb") as fh:
            data = fh.read()
        binary = b"\0" in data[:8192]
        if binary:
            warn.append("binary %s (%d B)" % (path, len(data)))
            continue
        text = data.decode("utf-8", "replace")
        head = text.splitlines()[:20]
        if b"\r\n" in data:
            (warn if path.endswith(".patch") else err).append("%s: CRLF line endings%s" % (path, " (patch context of an upstream CRLF file: must stay byte exact)" if path.endswith(".patch") else ""))
        for mm in HOME.finditer(text):
            (err if mm.group(1) == "Users" else warn).append("%s: absolute home path (/%s/...)" % (path, mm.group(1)))
            break
        if KEY.search(text) and not path.endswith("lint_repo.py"):
            err.append("%s: private key block" % path)
        first = head[0] if head else ""
        if first.startswith("#!") and mode != "100755":
            warn.append("%s: has a #! line but is not executable" % path)
        if path.endswith(SRC):
            nsrc += 1
            m = next((SPDX.search(ln) for ln in head if SPDX.search(ln)), None)
            if m:
                lic = m.group(1).strip()
                if lic in ok_lic:
                    nlic += 1
                else:
                    err.append("%s: SPDX %r not accepted here" % (path, lic))
            elif any(GEN.search(ln) for ln in head[:3]):
                nlic += 1          # generated output: carries its generator's licence (the generator is in this repository)
            else:
                err.append("%s: no SPDX line" % path)
        if path.endswith(".sh") or (first.startswith("#!") and "sh" in first.split("/")[-1] and "python" not in first):
            shells.append(p)
        if path.endswith(".py"):
            pys.append((p, path, text))
    for p in shells:
        r = subprocess.run(["bash", "-n", p], capture_output=True, text=True)
        if r.returncode:
            err.append("%s: bash -n: %s" % (os.path.relpath(p, ROOT), r.stderr.strip()[:200]))
    if shutil.which("shellcheck"):
        r = subprocess.run(["shellcheck", "-S", "error"] + shells, capture_output=True, text=True)
        if r.returncode:
            err.append("shellcheck:\n" + r.stdout[-3000:])
    else:
        warn.append("shellcheck not installed: bash -n only")
    tmp = tempfile.mkdtemp()
    try:
        for p, path, text in pys:
            try:
                py_compile.compile(p, cfile=os.path.join(tmp, "x.pyc"), doraise=True)
            except py_compile.PyCompileError as e:
                err.append("%s: does not compile: %s" % (path, str(e).splitlines()[-1][:200]))
            for ln, name in unused_imports(text, path):
                warn.append("%s:%d: unused import %s" % (path, ln, name))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if shutil.which("pyflakes"):
        r = subprocess.run(["pyflakes"] + [p for p, _a, _b in pys], capture_output=True, text=True)
        if r.returncode:
            warn.append("pyflakes:\n" + r.stdout[-3000:])
    for w in warn:
        print("warn  " + w)
    for e in err:
        print("ERROR " + e)
    print("lint: %d source files, %d with an accepted SPDX line (%.1f %%), %d shell scripts, %d Python files, %d errors, %d warnings"
          % (nsrc, nlic, 100.0 * nlic / max(nsrc, 1), len(shells), len(pys), len(err), len(warn)))
    return 1 if err or (a.strict and warn) else 0


if __name__ == "__main__":
    sys.exit(main())
