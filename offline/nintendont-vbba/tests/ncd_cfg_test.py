#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""host test: the real vg_ncd_cfg() text from patches/0017 compiled against a mock ncd (ioctlv 3 returns a given config; the function never writes)."""
import os, re, subprocess, sys, tempfile
d = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(d)
patch = open(os.path.join(root, "patches", "0017-multishine-ncd-config-wired.patch")).read().splitlines()
add = [l[1:] for l in patch if l.startswith("+") and not l.startswith("+++")]
a = next(i for i, l in enumerate(add) if l.startswith("/* the Wii's stored network config"))
b = next(i for i, l in enumerate(add) if i > a and l == "}")
func = "\n".join(add[a:b + 1])
assert "static void vg_ncd_cfg(void)" in func and "NetForce" not in func and "Ioctlv(ncdFd, 4" not in func
t = tempfile.mkdtemp()
open(os.path.join(t, "func.inc"), "w").write(func + "\n")
open(os.path.join(t, "t.c"), "w").write(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32;
typedef struct { void *data; u32 len; } ioctlv;
static int vgHeap = 1, ncdFd = 3; static u32 vgCfgKind;
static char logbuf[8192];
static void dbgprintf(const char *f, ...) { va_list v; va_start(v, f); vsnprintf(logbuf + strlen(logbuf), sizeof logbuf - strlen(logbuf), f, v); va_end(v); }
static void *heap_alloc_aligned(int h, u32 n, u32 al) { (void)h; void *p = NULL; if (posix_memalign(&p, al, n)) return NULL; return p; }
static void heap_free(int h, void *p) { (void)h; free(p); }
#define sync_after_write(p, n) ((void)0)
#define sync_before_read(p, n) ((void)0)
static u8 mock_cfg[0x1B5C]; static u32 mock_status, writes; static u8 written[0x1B5C]; static int read_rc;
static s32 IOS_Ioctlv(int fd, int cmd, int nin, int nio, void *vec) {
	ioctlv *v = vec; (void)fd; (void)nin; (void)nio;
	if (cmd == 3) { if (read_rc < 0) return read_rc; memcpy(v[0].data, mock_cfg, 0x1B5C); *(u32 *)v[1].data = mock_status; return 0; }
	if (cmd == 4) { writes++; memcpy(written, v[0].data, 0x1B5C); *(u32 *)v[1].data = 0; return 0; }
	return -1;
}
#include "func.inc"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void setup(u8 kind, u8 f1, u8 f2, u8 f3) {
	memset(mock_cfg, 0, sizeof mock_cfg); mock_cfg[4] = kind; mock_cfg[6] = 7;
	mock_cfg[8] = f1; mock_cfg[8 + 0x91C] = f2; mock_cfg[8 + 2 * 0x91C] = f3;
	mock_status = 0; writes = 0; logbuf[0] = 0; vgCfgKind = 99; read_rc = 0;
}
int main(void) {
	/* 1: Wi-Fi selected on connection 1, a wired entry on 2 (not selected): logs kind 1, never writes */
	setup(1, 0xA6, 0x26, 0); vg_ncd_cfg();
	CHECK(writes == 0); CHECK(vgCfgKind == 1); CHECK(strstr(logbuf, "connection 1 flags A6: SELECTED, wireless"));
	/* 2: wired selected, kind 2: logs kind 2, never writes */
	setup(2, 0x26, 0xA7, 0); vg_ncd_cfg();
	CHECK(writes == 0); CHECK(vgCfgKind == 2); CHECK(strstr(logbuf, "connection 2 flags A7: SELECTED, wired"));
	/* 6: read failure: nothing written, no crash, kind stays */
	setup(1, 0xA6, 0, 0); read_rc = -5; vg_ncd_cfg();
	CHECK(writes == 0); CHECK(strstr(logbuf, "read failed")); CHECK(vgCfgKind == 99);
	/* 7: status word nonzero: read failed path */
	setup(1, 0xA6, 0, 0); mock_status = 3; vg_ncd_cfg();
	CHECK(writes == 0); CHECK(strstr(logbuf, "read failed"));
	/* 8: log lines never carry the SSID or key area (we never print them) and stay short */
	{ char *p = logbuf, *e; setup(1, 0xA6, 0x27, 0); memset(mock_cfg + 0x300, 'K', 64); vg_ncd_cfg();
	  CHECK(strchr(logbuf, 'K') == NULL || strstr(logbuf, "KKKK") == NULL);
	  while ((e = strchr(p, '\n'))) { CHECK((e - p) < 200); p = e + 1; } }
	printf(fails ? "ncd config test: %d FAILED\n" : "ncd config test: all checks passed\n", fails);
	return fails != 0;
}
''')
r = subprocess.run(["cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", t, os.path.join(t, "t.c"), "-o", os.path.join(t, "t")], capture_output=True, text=True)
if r.returncode: print(r.stdout, r.stderr); sys.exit(1)
r = subprocess.run([os.path.join(t, "t")], capture_output=True, text=True); print(r.stdout, r.stderr)
sys.exit(r.returncode)
