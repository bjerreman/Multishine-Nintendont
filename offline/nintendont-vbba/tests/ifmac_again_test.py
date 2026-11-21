#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""host test: vg_ifmac_again() from patches/0017 against a mock ip/top: runs only after the timed gaps (5 s, 30 s), at most twice, logs the time and same/differs, never touches the chip."""
import os, subprocess, sys, tempfile
d = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(d)
patch = open(os.path.join(root, "patches", "0017-multishine-ncd-config-wired.patch")).read().splitlines()
add = [l[1:] for l in patch if l.startswith("+") and not l.startswith("+++")]
a = next(i for i, l in enumerate(add) if l.startswith("/* ask ip/top for the active interface's MAC again"))
b = next(i for i, l in enumerate(add) if i > a and l == "}")
func = "\n".join(add[a:b + 1])
assert "static void vg_ifmac_again(void)" in func
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
static int vgHeap = 1, ipFd = 4; static u32 vgIfMacT1, vgIfMacTries; static u8 vgIfMacFirst[6];
static u32 now_ms; static u32 vg_now_ms(void *c) { (void)c; return now_ms; }
static char logbuf[4096]; static int calls; static u8 answer[6]; static s32 ans_rc;
static void dbgprintf(const char *f, ...) { va_list v; va_start(v, f); vsnprintf(logbuf + strlen(logbuf), sizeof logbuf - strlen(logbuf), f, v); va_end(v); }
static void *heap_alloc_aligned(int h, u32 n, u32 al) { (void)h; void *p = NULL; if (posix_memalign(&p, al, n)) return NULL; return p; }
static void heap_free(int h, void *p) { (void)h; free(p); }
#define sync_after_write(p, n) ((void)0)
#define sync_before_read(p, n) ((void)0)
static s32 IOS_Ioctlv(int fd, int cmd, int nin, int nio, void *vec) {
	ioctlv *v = vec; u32 *in = v[0].data; (void)fd; (void)nin; (void)nio; calls++;
	if (cmd != 0x1C || in[0] != 0xFFFE || in[1] != 0x1004 || v[0].len != 8 || v[1].len != 6 || v[2].len != 4 || *(u32 *)v[2].data != 6) return -99;
	if (ans_rc >= 0) memcpy(v[1].data, answer, 6);
	return ans_rc;
}
#include "func.inc"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
	static const u8 w[6] = {1,2,3,4,5,6}, l[6] = {7,8,9,10,11,12};
	memcpy(vgIfMacFirst, w, 6); memcpy(answer, w, 6);
	now_ms = 1000; vg_ifmac_again(); CHECK(calls == 0);                 /* no first answer yet */
	vgIfMacT1 = 20000; now_ms = 24999; vg_ifmac_again(); CHECK(calls == 0);   /* before +5 s */
	now_ms = 25000; vg_ifmac_again(); CHECK(calls == 1 && vgIfMacTries == 1 && strstr(logbuf, "retry 1 at 25000 ms (+5000 ms") && strstr(logbuf, "same as the first"));
	now_ms = 26000; vg_ifmac_again(); CHECK(calls == 1);                  /* second one waits for +30 s */
	memcpy(answer, l, 6); now_ms = 50000; vg_ifmac_again(); CHECK(calls == 2 && strstr(logbuf, "retry 2 at 50000 ms") && strstr(logbuf, "07:08:09:0A:0B:0C") && strstr(logbuf, "differs from the first"));
	now_ms = 90000; vg_ifmac_again(); CHECK(calls == 2);                  /* never a third */
	vgIfMacTries = 0; vgIfMacT1 = 1; ans_rc = -5; logbuf[0] = 0; now_ms = 6000; vg_ifmac_again(); CHECK(strstr(logbuf, "failed (rc -5)"));
	vgIfMacTries = 0; ipFd = -1; calls = 0; now_ms = 99000; vg_ifmac_again(); CHECK(calls == 0);
	printf(fails ? "FAILED\n" : "ifmac_again test ok\n"); return fails != 0;
}
''')
cc = os.environ.get("CC", "cc")
if subprocess.call([cc, "-std=gnu11", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-o", os.path.join(t, "t"), os.path.join(t, "t.c")]) != 0: sys.exit(1)
sys.exit(subprocess.call([os.path.join(t, "t")]))
