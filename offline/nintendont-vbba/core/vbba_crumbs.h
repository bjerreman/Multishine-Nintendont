/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * Breadcrumbs: where each vBBA thread is, written with a few stores and published in the spare words 121..127 of the 512-byte status
 * block (the block that was already there in 5c5ae7b; nothing is added to memory, so the heap and the layout stay as they were).
 * Purpose: after a freeze ONE photo of the probe page says which thread stopped where, which IOS call
 * was in flight, which request slots were busy, and how deep the stacks went, instead of a guess per run.
 *
 *   word 121  FAST  fast EXI thread (VBBAThread, every function it calls): [31:26] site BCF_*, [25:0] HW_TIMER low 26 bits at that site
 *   word 122  MAIN  Nintendont's main loop inside the vBBA hooks:           [31:26] site BCM_*, [25:0] HW_TIMER low 26 bits
 *   word 123  DONE  completion thread (VBBADoneThread):                     [31:26] site BCD_*, [25:16] completions seen (mod 1024), [15:0] last result (low 16 bits)
 *   word 124  IOS   last IOS call made by the glue: [31:28] call BCI_*, [27:24] state (1 in the call, 2 returned >= 0, 3 returned < 0), [23:16] slot, [15:0] return code (low 16 bits)
 *   word 125  USED  bit i = request slot i holds an unfinished IOS request (32 slots)
 *   word 126  HDR   [31:24] 0xBC, [23:20] version 1, [19:10] fast-thread stack used / 16, [9:0] completion-thread stack used / 4 (bytes; 0xA5 fill, scanned by the main thread)
 *   word 127  TRAIL the last five FAST sites, newest in the low 6 bits: [29:0]
 * Queue canary block (second area, 64 words = 256 bytes, the top 256 bytes of OUR heap area: the glue creates its heap 256 bytes shorter, so nothing
 * that was allocated before moves; fixed address ARM 0x131BDF00 = PowerPC uncached 0xD31BDF00; the main loop copies and flushes it with the status block):
 * it answers whether IOS writes behind the end of the message-queue array.  The queue memory is filled with
 * 0xC3 before mqueue_create, so every word IOS stores shows against that pattern.
 *   BCX_MAGIC 'BCXE', BCX_VER 1, BCX_QBYTES allocated bytes of the queue block, BCX_QENT entries passed to mqueue_create, BCX_QADDR ARM address of the block,
 *   BCX_CHG number of words behind the array (4 x entries .. block end) that no longer hold 0xC3, BCX_FIRST word index (from the array start) of the first changed one
 *   and BCX_FIRSTV its value, BCX_LAST / BCX_LASTV the last one, BCX_FSTK / BCX_DSTK ARM addresses of the two thread stacks, BCX_QHEAD 16 words from the array start
 *   (what IOS stores per entry), BCX_QEND 16 words from word entries-4 on (the end of the array and the bytes behind it), BCX_NEXT 16 words from the first byte after the
 *   block (the neighbour: allocator header and the fast stack's far end, which starts as 0xA5 fill).
 * A tick is HW_TIMER (1.898 MHz): 26 bits wrap after 35 s; the reader subtracts from the newest full tick it knows (word 75 main_tick or word 65 hb_tick).
 * Header only, no libc: used by the kernel glue (gnu89), the probe (PowerPC) and the host tests.
 */
#ifndef VBBA_CRUMBS_H
#define VBBA_CRUMBS_H

#define VBBA_BC_FAST 121
#define VBBA_BC_MAIN 122
#define VBBA_BC_DONE 123
#define VBBA_BC_IOS 124
#define VBBA_BC_USED 125
#define VBBA_BC_HDR 126
#define VBBA_BC_TRAIL 127
#define VBBA_BCX_WORDS 64
#define VBBA_BCX_MAGIC 0x42435845u   /* 'BCXE' */
enum { BCX_MAGIC = 0, BCX_VER, BCX_QBYTES, BCX_QENT, BCX_QADDR, BCX_FSTK, BCX_DSTK, BCX_CHG, BCX_FIRST, BCX_FIRSTV, BCX_LAST, BCX_LASTV, BCX_QHEAD = 16, BCX_QEND = 32, BCX_NEXT = 48 };
#define VBBA_BC_MAGIC 0xBCu
#define VBBA_BC_VERSION 1u
#define VBBA_BC_TICK_MASK 0x03FFFFFFu

/* sites of the fast thread (1..63) */
enum vbba_bcf {
	BCF_NONE = 0,
	BCF_LOOP,        /* top of the thread loop */
	BCF_HOLD,        /* hold (memory card save): sleeping */
	BCF_EXISTEP,     /* before EXIServiceStep (a command is pending) */
	BCF_EXIDONE,     /* after it */
	BCF_BG,          /* before the background tick (chip poll, transport poll, IOS completions) */
	BCF_BGDONE,
	BCF_STATUS,      /* before the status block update and its cache flush */
	BCF_STATUSDONE,
	BCF_YIELD,       /* burst over: before the 100 us sleep that lets the main loop run */
	BCF_YIELDED,
	BCF_IDLE,        /* before the idle sleep */
	BCF_IDLED,
	BCF_DEGRADE,     /* watchdog: priority lowered */
	BCF_EXIT,        /* thread loop left */
	BCF_POLL,        /* vg_poll (IOS completions to the transport) entered */
	BCF_POLLITEM,    /* a completed slot is being handled */
	BCF_POLLTMO,     /* timeout scan */
	BCF_POLLOUT,     /* vg_poll leaves */
	BCF_SUBMIT,      /* vg_submit entered */
	BCF_IOSCALL,     /* about to call an IOS_*Async syscall (word 124 says which) */
	BCF_IOSRET,      /* it returned */
	BCF_IMM,         /* EXI immediate transfer of the virtual BBA */
	BCF_IMMDONE,
	BCF_DMAIN,       /* DMA: before sync_before_read of the guest buffer */
	BCF_DMACHIP,     /* DMA: chip model */
	BCF_DMAOUT,      /* DMA: before sync_after_write */
	BCF_DMADONE,
	BCF_SELECT,      /* EXI select of the virtual BBA */
	BCF_COUNT
};
/* sites of the main loop (inside the hooks) */
enum vbba_bcm {
	BCM_NONE = 0,
	BCM_UPDATE,      /* VBBAUpdate entered (one per main-loop pass) */
	BCM_START,       /* vg_start begins */
	BCM_STARTED,     /* vg_start done, threads running */
	BCM_TAKEOVER,    /* watchdog took the thread over */
	BCM_OPEN,        /* blocking IOS_Open (vg_reopen) */
	BCM_OPENED,
	BCM_HOLDWAIT,    /* VBBAHoldBegin: waiting for the thread to leave its command */
	BCM_HOLDON,      /* hold in force */
	BCM_HOLDEND,
	BCM_SHUT,        /* VBBAShutdown begins */
	BCM_SHUTC1,      /* before thread_cancel(fast) */
	BCM_SHUTC2,      /* before thread_cancel(done) */
	BCM_SHUTSTAT,    /* final status update */
	BCM_SHUTLOG,     /* dbgprintf block (SD writes) */
	BCM_SHUTCLOSE,   /* IOS_Close of our handles */
	BCM_SHUTHEAP,    /* heap_destroy */
	BCM_SHUTDONE,
	BCM_UPDDONE,     /* VBBAUpdate left */
	BCM_COUNT
};
/* sites of the completion thread */
enum vbba_bcd {
	BCD_NONE = 0,
	BCD_RECV,        /* blocked in mqueue_recv (the normal resting place) */
	BCD_GOT,         /* a completion message arrived */
	BCD_ACKED,       /* mqueue_ack done */
	BCD_PUSHED,      /* entry in the ring */
	BCD_OVF,         /* ring full, entry dropped */
	BCD_COUNT
};
/* IOS calls the glue makes (word 124) */
enum vbba_bci {
	BCI_NONE = 0,
	BCI_NCD,         /* ncd/manage ioctlv 7 (link status) */
	BCI_KD,          /* kd/request ioctl 6 (NWC24 start-up) */
	BCI_IFSTART,     /* ip/top 0x1F so_startinterface */
	BCI_GETIF,       /* ip/top 0x1C getinterfaceopt */
	BCI_HOSTID,      /* ip/top 0x10 gethostid */
	BCI_SOCKET,      /* 0x0F */
	BCI_BIND,        /* 0x02 */
	BCI_SENDTO,      /* 0x0D */
	BCI_RECVFROM,    /* 0x0C */
	BCI_CLOSE,       /* 0x03 */
	BCI_COUNT
};
enum { BCS_IN = 1, BCS_OK = 2, BCS_ERR = 3 };

static inline unsigned vbba_bc_w(unsigned site, unsigned tick) { return (site << 26) | (tick & VBBA_BC_TICK_MASK); }
static inline unsigned vbba_bc_site(unsigned w) { return w >> 26; }
static inline unsigned vbba_bc_tick(unsigned w) { return w & VBBA_BC_TICK_MASK; }
/* ticks between the crumb and a newer full tick `ref` (0 when the crumb is newer than ref by less than half the wrap) */
static inline unsigned vbba_bc_age(unsigned ref, unsigned w)
{
	unsigned d = (ref - vbba_bc_tick(w)) & VBBA_BC_TICK_MASK;
	return d >= 0x02000000u ? 0u : d;
}
static inline unsigned vbba_bc_done_w(unsigned site, unsigned count, unsigned res) { return (site << 26) | ((count & 0x3FFu) << 16) | (res & 0xFFFFu); }
static inline unsigned vbba_bc_done_count(unsigned w) { return (w >> 16) & 0x3FFu; }
static inline int vbba_bc_done_res(unsigned w) { return (int)(short)(w & 0xFFFFu); }
static inline unsigned vbba_bc_ios_w(unsigned id, unsigned state, unsigned slot, int rc) { return (id << 28) | ((state & 15u) << 24) | ((slot & 255u) << 16) | ((unsigned)rc & 0xFFFFu); }
static inline unsigned vbba_bc_ios_id(unsigned w) { return w >> 28; }
static inline unsigned vbba_bc_ios_state(unsigned w) { return (w >> 24) & 15u; }
static inline unsigned vbba_bc_ios_slot(unsigned w) { return (w >> 16) & 255u; }
static inline int vbba_bc_ios_rc(unsigned w) { return (int)(short)(w & 0xFFFFu); }
static inline unsigned vbba_bc_hdr_w(unsigned fast_bytes, unsigned done_bytes)
{
	unsigned f = fast_bytes >> 4, d = done_bytes >> 2;
	return (VBBA_BC_MAGIC << 24) | (VBBA_BC_VERSION << 20) | ((f > 1023u ? 1023u : f) << 10) | (d > 1023u ? 1023u : d);
}
static inline int vbba_bc_hdr_ok(unsigned w) { return (w >> 24) == VBBA_BC_MAGIC && ((w >> 20) & 15u) == VBBA_BC_VERSION; }
static inline unsigned vbba_bc_hdr_fast(unsigned w) { return ((w >> 10) & 1023u) << 4; }
static inline unsigned vbba_bc_hdr_done(unsigned w) { return (w & 1023u) << 2; }
static inline unsigned vbba_bc_trail_push(unsigned trail, unsigned site) { return ((trail << 6) | (site & 63u)) & 0x3FFFFFFFu; }
static inline unsigned vbba_bc_trail_at(unsigned trail, unsigned back) { return (trail >> (6u * back)) & 63u; }  /* back 0 = newest */

#ifdef VBBA_BC_NAMES   /* only the probe and the tests: names of up to 8 characters */
static const char *vbba_bcf_name(unsigned s)
{
	static const char *const n[BCF_COUNT] = { "-", "loop", "hold", "exi", "exiend", "bg", "bgend", "status", "statend", "yield", "yielded", "idle", "idled", "degrade", "exit",
		"poll", "pollitem", "polltmo", "pollout", "submit", "ioscall", "iosret", "imm", "immend", "dma-in", "dma-chip", "dma-out", "dma-end", "select" };
	return s < BCF_COUNT ? n[s] : "?";
}
static const char *vbba_bcm_name(unsigned s)
{
	static const char *const n[BCM_COUNT] = { "-", "update", "start", "started", "takeover", "open", "opened", "holdwait", "holdon", "holdend", "shut",
		"shut-c1", "shut-c2", "shutstat", "shutlog", "shutcls", "shutheap", "shutdone", "updend" };
	return s < BCM_COUNT ? n[s] : "?";
}
static const char *vbba_bcd_name(unsigned s)
{
	static const char *const n[BCD_COUNT] = { "-", "recv", "got", "acked", "pushed", "ovf" };
	return s < BCD_COUNT ? n[s] : "?";
}
static const char *vbba_bci_name(unsigned s)
{
	static const char *const n[BCI_COUNT] = { "-", "ncd", "kd", "ifstart", "getif", "hostid", "socket", "bind", "sendto", "recvfrom", "close" };
	return s < BCI_COUNT ? n[s] : "?";
}
#endif
#endif
