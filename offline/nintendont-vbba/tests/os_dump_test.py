#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""host test: vg_diag_os() from patches/0018 against a fake PowerPC memory image (two OS threads, one waiting, one running, plus a bad link pointer and a cycle guard)."""
import os, subprocess, sys, tempfile
d = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(d)
patch = open(os.path.join(root, "patches", "0018-multishine-os-thread-dump-irqpoke.patch")).read().splitlines()
add = [l[1:] for l in patch if l.startswith("+") and not l.startswith("+++")]
a = next(i for i, l in enumerate(add) if l.startswith("/* (diagnostic only, reads only): the PowerPC OS thread list"))
b = next(i for i, l in enumerate(add) if i > a and l.startswith("static void vg_diag_os(void)"))
c = next(i for i, l in enumerate(add) if i > b and l == "}")
func = "\n".join(add[a:c + 1])
t = tempfile.mkdtemp()
open(os.path.join(t, "func.inc"), "w").write(func + "\n")
open(os.path.join(t, "t.c"), "w").write(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
static uint32_t *mem;                 /* 32 MiB fake MEM1, big-endian words stored as plain u32 values */
#define VG_PM(a) ((const volatile u32 *)((u8 *)mem + ((a) & 0x01FFFFFFu)))
#define sync_before_read(p, n) ((void)0)
static char logbuf[8192];
static void dbgprintf(const char *f, ...) { va_list v; va_start(v, f); vsnprintf(logbuf + strlen(logbuf), sizeof logbuf - strlen(logbuf), f, v); va_end(v); }
#include "func.inc"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void W(u32 a, u32 v) { mem[(a & 0x01FFFFFFu) / 4] = v; }
int main(void) {
	mem = calloc(0x02000000u / 4, 4);
	W(0x800000D4u, 0x80500000u); W(0x800000DCu, 0x80600000u); W(0x800000E0u, 0x80700000u); W(0x800000E4u, 0x80600000u);
	/* thread 1: waiting (state 4), priority 8, wait queue set, saved pc/lr/sp */
	W(0x80600000u + 0x2C8u, 4u << 16); W(0x80600000u + 0x2D0u, 8u); W(0x80600000u + 0x2DCu, 0x80123456u);
	W(0x80600000u + 0x198u, 0x8001A2B4u); W(0x80600000u + 0x84u, 0x80019000u); W(0x80600000u + 0x04u, 0x80700F00u); W(0x80600000u + 0x2FCu, 0x80700000u);
	/* thread 2: running (2), no queue, next = 0 */
	W(0x80700000u + 0x2C8u, 2u << 16); W(0x80700000u + 0x2D0u, 16u); W(0x80700000u + 0x198u, 0x80003210u); W(0x80700000u + 0x2FCu, 0u);
	logbuf[0] = 0; vg_diag_os();
	CHECK(strstr(logbuf, "cur thread 80600000"));
	CHECK(strstr(logbuf, "os thr 0 @80600000 state 4 prio 8 waitq 80123456 | srr0 8001A2B4 lr 80019000 r1 80700F00"));
	CHECK(strstr(logbuf, "os thr 1 @80700000 state 2 prio 16 waitq 00000000 | srr0 80003210"));
	CHECK(strstr(logbuf, "os thr 2") == NULL);
	/* a list that points back at itself is cut at 8 entries; a wild pointer ends the walk without a read outside MEM1 */
	W(0x80700000u + 0x2FCu, 0x80600000u); logbuf[0] = 0; vg_diag_os();
	{ int n = 0; char *p = logbuf; while ((p = strstr(p, "os thr "))) { n++; p++; } CHECK(n == 8); }
	W(0x80600000u + 0x2FCu, 0xC0000000u); W(0x80700000u + 0x2FCu, 0); logbuf[0] = 0; vg_diag_os();
	{ int n = 0; char *p = logbuf; while ((p = strstr(p, "os thr "))) { n++; p++; } CHECK(n == 1); }
	/* line length stays short */
	{ char *p = logbuf, *e; while ((e = strchr(p, '\n'))) { CHECK((e - p) < 200); p = e + 1; } }
	printf(fails ? "os dump test: %d FAILED\n" : "os dump test: all checks passed\n", fails);
	return fails != 0;
}
''')
r = subprocess.run(["cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", t, os.path.join(t, "t.c"), "-o", os.path.join(t, "t")], capture_output=True, text=True)
if r.returncode: print(r.stdout, r.stderr); sys.exit(1)
r = subprocess.run([os.path.join(t, "t")], capture_output=True, text=True); print(r.stdout, r.stderr)
sys.exit(r.returncode)
