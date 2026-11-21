#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""NIN_MS_WIFIFIX (build_wii.sh): edits kernel/vbba_glue.c so a Wii on Wi-Fi gets two things done once its network is up.
  1. Roaming off.  The Wi-Fi driver scans for a better access point about every 10 s and each scan stops the link for ~150 ms (a 5-7 frame stall in a game).
     The driver's 'roam_off' switch is set through /dev/wl0 (platform/nintendont/vbba_wl.h builds the command buffer), read back, and re-checked every 15 s
     because the driver turns roaming on again whenever it re-associates.
  2. WiiConnect24's background scheduler paused (kd/request ioctl 1) and resumed at shutdown (ioctl 3): it can send and download during a game.
Both come from wize-logic/melee-wii's Wii port (wii/src/wifi.c, wii/), re-written here for the ARM kernel.  Everything runs in ONE low-priority thread
that waits for the network (address seen) and a Wi-Fi interface (ncd config kind 1), so a call that hangs can only hold that thread; the main loop only prints the
thread's results.  The WiiConnect24 pause is done on wired consoles too; the roaming part is Wi-Fi only (a wired or unknown interface logs 'skip' and ends the thread).
Usage: wifi_fix_patch.py <kernel/vbba_glue.c>"""
import sys

INCLUDE = '#include "vbba_tp_ios.h"\n'
FUNCS_ANCHOR = '\theap_free(vgHeap, name);\n}\n\n/* ---- start-up trace'
INIT_ANCHOR = '\tdbgprintf("VBBA: init: completion and helper threads created\\r\\n");\n'
MAIN_ANCHOR = '\tvg_reopen();                                        /* missing handles / ncd: blocking opens belong here, not on the fast thread */\n'
SHUT_ANCHOR = '\t\tif (ncdFd >= 0) { IOS_Close(ncdFd); ncdFd = -1; }\n\t\tif (ipFd >= 0) { IOS_Close(ipFd); if (ipFd == sockFd)'

FUNCS = r'''
/* ---- NIN_MS_WIFIFIX: roaming off + WiiConnect24 paused ---- */
#define VG_WF_RING 16u
static u32 *vgWfStack; static u8 *vgWfIo, *vgWfKdOut; static u32 vgWfThread;
static volatile u32 vgWfHead, vgWfState;       /* state: 0 waiting, 1 not Wi-Fi / done, 2 roam_off set, 3 WiiConnect24 paused too */
static u32 vgWfTail; static s32 vgWfWl = -1;
struct vg_wfev { u32 code, a, b, ms; };
static struct vg_wfev vgWfEv[VG_WF_RING];
enum { WF_START = 1, WF_SKIP, WF_KD, WF_OPEN, WF_GET0, WF_SET, WF_GET1, WF_RECHECK, WF_LOST };
static void vg_wf_ev(u32 code, u32 a, u32 b)
{
	struct vg_wfev *e = &vgWfEv[vgWfHead % VG_WF_RING];
	e->code = code; e->a = a; e->b = b; e->ms = vg_now_ms(NULL);
	vgWfHead++;
}
/* one WLC command through the hand-built iobuf; returns IOS's result, the answer stays in vgWfIo */
static s32 vg_wf_wl(u32 cmd, u32 inner, const char *nm, int sv)
{
	s32 rc;
	if (vbba_wl_build(vgWfIo, ((u32)vgWfIo + VBBA_WL_DATA_OFF) & 0x3FFFFFFFu, cmd, inner, nm, sv) < 0) return -300;
	sync_after_write(vgWfIo, VBBA_WL_IOBUF_SIZE);
	rc = IOS_Ioctl(vgWfWl, (s32)VBBA_WL_IOCTL_WLC, vgWfIo, VBBA_WL_IOBUF_SIZE, NULL, 0);
	sync_before_read(vgWfIo, VBBA_WL_IOBUF_SIZE);
	udelay(20000);                                     /* the driver finishes its write-back after the reply; reusing the buffer at once raced in melee-wii's tests */
	return rc;
}
static s32 vg_wf_get_roam(u32 *off)
{
	s32 rc = vg_wf_wl(VBBA_WLC_GET_VAR, VBBA_WL_GET_INNER, "roam_off", -1);
	*off = rc >= 0 ? vbba_wl_value(vgWfIo) : 0;
	return rc;
}
static u32 VBBAWifiThread(void *arg)
{
	u32 t0, off = 0, wait_ms = 0;
	s32 rc;
	char *nm = (char *)vgWfIo + 0x80;                  /* the 32-byte name buffer behind the iobuf (allocated together) */
	(void)arg;
	vg_wf_ev(WF_START, vgCfgKind, 0);
	while (!vgStop && wait_ms < 180000u) {             /* the network start is released late (SD idle) and DHCP takes a while */
		if ((vst->kd_flags & 8) && vst->ip_ms && (u32)(vg_now_ms(NULL) - vst->ip_ms) >= 3000u) break;
		udelay(200000); wait_ms += 200;
	}
	if (vgStop || !(vst->kd_flags & 8)) { vg_wf_ev(WF_SKIP, 1, wait_ms); vgWfState = 1; return 0; }
	if (kdFd >= 0 && vgWfKdOut) {                       /* WiiConnect24 scheduler: pause, wired or Wi-Fi (a download under way delays the answer) */
		memset(vgWfKdOut, 0, 32); sync_after_write(vgWfKdOut, 32);
		rc = IOS_Ioctl(kdFd, 1, NULL, 0, vgWfKdOut, 32);
		sync_before_read(vgWfKdOut, 32);
		vg_wf_ev(WF_KD, (u32)rc, rc >= 0 ? *(u32 *)vgWfKdOut : 0);
		if (rc >= 0 && (s32)*(u32 *)vgWfKdOut >= 0) vgWfState = 3;
	}
	if (vgCfgKind != 1u) { vg_wf_ev(WF_SKIP, 2, vgCfgKind); if (vgWfState < 3) vgWfState = 1; return 0; }    /* roaming: Wi-Fi only (wired = 2, unknown = 0) */
	strcpy(nm, "/dev/wl0");
	vgWfWl = IOS_Open(nm, 0);
	vg_wf_ev(WF_OPEN, (u32)vgWfWl, 0);
	if (vgWfWl < 0) { if (vgWfState < 3) vgWfState = 1; return 0; }
	rc = vg_wf_get_roam(&off); vg_wf_ev(WF_GET0, (u32)rc, off);
	if (!(rc >= 0 && off)) {
		rc = vg_wf_wl(VBBA_WLC_SET_VAR, 0x0Du, "roam_off", 1); vg_wf_ev(WF_SET, (u32)rc, 0);
		rc = vg_wf_get_roam(&off); vg_wf_ev(WF_GET1, (u32)rc, off);
	}
	if (rc >= 0 && off) vgWfState = vgWfState == 3 ? 3 : 2;
	t0 = vg_now_ms(NULL);
	while (!vgStop) {                                   /* the driver turns roaming back on whenever it re-associates: look again every 15 s */
		udelay(500000);
		if ((u32)(vg_now_ms(NULL) - t0) < 15000u) continue;
		t0 = vg_now_ms(NULL);
		rc = vg_wf_get_roam(&off);
		if (rc < 0) { vg_wf_ev(WF_LOST, (u32)rc, 0); break; }
		if (!off) {
			s32 r2 = vg_wf_wl(VBBA_WLC_SET_VAR, 0x0Du, "roam_off", 1);
			s32 r3 = vg_wf_get_roam(&off);
			vg_wf_ev(WF_RECHECK, (u32)r2, (u32)(r3 >= 0 && off));
		}
	}
	return 0;
}
/* main thread: prints what the thread logged since the last call */
static void vg_wf_report(void)
{
	static const char *const nmv[] = { "?", "start", "skip", "kd", "open", "get0", "set", "get1", "recheck", "lost" };
	while (vgWfTail != vgWfHead) {
		u32 i = vgWfTail++ % VG_WF_RING, c = vgWfEv[i].code;
		dbgprintf("VBBA: wififix: %s a %d b %d at %u ms%s\r\n", c < 10u ? nmv[c] : nmv[0], (s32)vgWfEv[i].a, (s32)vgWfEv[i].b, vgWfEv[i].ms,
			c == WF_GET1 || c == WF_GET0 ? " (b: roam_off, 1 = roaming off)" : c == WF_SKIP ? " (a: 1 no address, 2 not Wi-Fi: b = ncd interface kind)" : "");
	}
}
'''

INIT = r'''	vgWfStack = (u32 *)heap_alloc_aligned(vgHeap, 0x800, 32);
	vgWfIo = (u8 *)heap_alloc_aligned(vgHeap, 0xA0, 32);
	vgWfKdOut = (u8 *)heap_alloc_aligned(vgHeap, 32, 32);
	if (vgWfStack && vgWfIo && vgWfKdOut) {
		memset(vgWfIo, 0, 0xA0);
		vgWfThread = thread_create(VBBAWifiThread, NULL, vgWfStack + 0x800 / sizeof(u32), 0x800 / sizeof(u32), 0x78, 1);   /* the stack TOP, as for the other threads */
		thread_continue(vgWfThread);
		dbgprintf("VBBA: wififix thread started (roam_off + WiiConnect24 pause once the Wi-Fi network is up)\r\n");
	} else dbgprintf("VBBA: no memory for the wififix thread\r\n");
'''

MAIN = "\tvg_wf_report();\n"

SHUT = r'''	if (vgWfThread) thread_cancel(vgWfThread, 0);
	if (vgWfState == 3 && kdFd >= 0 && vgWfKdOut) { memset(vgWfKdOut, 0, 32); sync_after_write(vgWfKdOut, 32); IOS_Ioctl(kdFd, 3, NULL, 0, vgWfKdOut, 32); }   /* WiiConnect24's scheduler as it was */
	if (vgWfWl >= 0) { IOS_Close(vgWfWl); vgWfWl = -1; }
'''


def patch(s):
    for a in (INCLUDE, FUNCS_ANCHOR, INIT_ANCHOR, MAIN_ANCHOR, SHUT_ANCHOR):
        assert s.count(a) == 1, "anchor not unique: %r (%d)" % (a[:60], s.count(a))
    s = s.replace(INCLUDE, INCLUDE + '#include "vbba_wl.h"   /* NIN_MS_WIFIFIX */\n')
    s = s.replace(FUNCS_ANCHOR, '\theap_free(vgHeap, name);\n}\n' + FUNCS + '\n/* ---- start-up trace')
    s = s.replace(INIT_ANCHOR, INIT + INIT_ANCHOR)
    s = s.replace(MAIN_ANCHOR, MAIN_ANCHOR + MAIN)
    s = s.replace(SHUT_ANCHOR, SHUT + SHUT_ANCHOR)
    return s


if __name__ == "__main__":
    p = sys.argv[1]
    s = open(p, newline="").read()
    assert "\r\n" not in s, "CRLF source not handled"
    open(p, "w", newline="").write(patch(s))
