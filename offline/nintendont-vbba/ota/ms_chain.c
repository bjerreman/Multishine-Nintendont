/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The shell side of the in-place hand-over to a freshly installed shell: the stub's proven chain-load (msdol_check, the position
 * independent trampoline of tramp.S at 0x81300000, the Homebrew Channel argument block) run from the shell's early check.  The decision is msota_chain_decide (ms_ota_core.c, host-tested);
 * this file only does the irreversible part, and only after everything that can fail has been checked. */
#include <gccore.h>
#ifndef MS_CHAIN_TEST
#include <ogc/es.h>
#include <ogc/stm.h>
#endif
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "ms_ota_core.h"
#include "ms_chain_steps.h"

#define TRAMP_ADDR 0x81300000u           /* as in the stub: above MSDOL_HI, nothing of the shell lands here */
#define ARGS_ADDR  0x81308000u           /* the command line for the next shell */
#define ARGS_MAX   0x1000u
#define CHAIN_MARK 0x81309F00u           /* a word that survives the hand-over (outside the shell's sections, the trampoline and the argument block): 'MSCH' = this start is a hand-over */
#define CHAIN_MAGIC 0x4D534348u

struct tramp_sec { u32 src, dst, len; };
struct tramp_args { u32 entry, n, bss, bss_len; struct tramp_sec sec[MSDOL_NSEC]; u32 probe0, probe1, mbox, xfb, xfb_words, xfb_stride; };    /* ms_tramp2.S reads this layout */
extern void ms_tramp2_start(void), ms_tramp2_end(void);
#define MBOX_ADDR 0x81309F40u            /* the tramp's report (ms_tramp2.S): survives into the new shell, which writes it to ota.log */
#ifndef MS_CHAIN_TEST
extern void __exception_closeall(void);
extern void __lwp_thread_closeall(void);
#endif

/* the guard word and the flavour are read once (the first call, main()'s very first action) and cleared at once; this run keeps the answer.  They used to stay in MEM1: after the DSI of
   a warm reload could start the loader again with 'MSCH' + 'LATE' still there. */
static int g_ch_taken, g_ch_marker, g_ch_late;
static void chain_take(void)
{
	if (g_ch_taken) return;
	g_ch_taken = 1;
	msota_chain_take((volatile mw2_u32 *)CHAIN_MARK, &g_ch_marker, &g_ch_late);
	DCFlushRange((void *)CHAIN_MARK, 32);
}
int ms_chain_marker(void) { chain_take(); return g_ch_marker; }
int ms_chain_late(void) { chain_take(); return g_ch_late; }

/* is the Nintendont kernel really running?  (msota_kernel_probe, host-tested; RESET_STATUS / HID_CHANGE / HID_CFG_SIZE through the uncached MEM2 alias, up to 1 s) */
#ifndef MS_CHAIN_TEST
static void probe_wait(void *ctx) { (void)ctx; usleep(1000); }
int ms_kernel_alive(void)
{
	return msota_kernel_probe((volatile mw2_u32 *)0xD3003420u, (volatile mw2_u32 *)0xD3003444u, (volatile mw2_u32 *)0xD3003448u, 1000u, probe_wait, NULL);
}
#endif

/* a buffer from the bottom of the MEM2 arena (the source of the copy must not be in MEM1: the sections land there); NULL when there is none */
#ifdef MS_CHAIN_TEST                      /* the emulator test (stub/tests/chain_caller_test.sh): a GameCube build, no MEM2: buffers come from MEM1 at 0x80C00000 */
void *ms_chain_alloc2(u32 size)
{
	static u32 next = 0x80C00000u;
	u8 *p = (u8 *)next;
	next += (size + 31u) & ~31u;
	return p;
}
#else
void *ms_chain_alloc2(u32 size)
{
	u8 *p = (u8 *)(((u32)SYS_GetArena2Lo() + 31u) & ~31u);
	u8 *hi = (u8 *)SYS_GetArena2Hi();
	size = (size + 31u) & ~31u;
	if (!p || (u32)p < 0x90000000u || p + size > hi) return NULL;
	SYS_SetArena2Lo(p + size);
	return p;
}
#endif

/* what the tramp left in its mailbox (the new shell calls this at its start): 1 = a report was there, the text in out */
/* NOTE (crashed after a chained late hand-over: 'Exception (DSI)', r8 = 0x10801080 in _malloc_r during FT_Init_FreeType): the new shell used to paint a stage square and then CLEAR the old
   shell's XFB from its main().  That memory is not the old shell's any more: the new shell's libogc heap (newlib malloc had already set up its top chunk before main()) starts inside the address range the old
   shell's XFB occupied, so the clear (black = 0x10801080 in YUYV) overwrote the malloc chunk header, and the first malloc used 0x10801080 as a chunk pointer.  The new shell must NEVER write the old XFB.  The
   tramp's own squares (before the jump, while nothing of the new shell exists) stay; the tramp's mailbox words 8..10 (xfb, stride, words) are no longer read by anybody. */

int ms_chain_report(char *out, unsigned n)
{
	volatile u32 *m = (volatile u32 *)MBOX_ADDR;
	int ok = m[0] == 0x54524D50u;
	if (ok) snprintf(out, n, "tramp report: status %u, copy attempts %u, entry word %08X, probe %08X = %08X, probe %08X = %08X", (unsigned)m[1], (unsigned)m[2], (unsigned)m[3], (unsigned)m[4], (unsigned)m[5], (unsigned)m[6], (unsigned)m[7]);
	if (ok) { m[0] = 0; DCFlushRange((void *)MBOX_ADDR, 32); }
	return ok;
}

/* start the DOL (already in MEM2, already checked by the caller) through the trampoline; returns only if the DOL is refused or the trampoline buffer cannot be made, both before anything irreversible.
   hooks (may be NULL): crumb(text) writes a breadcrumb line to the SD while it is still mounted, shutdown() closes the SD / USB at the last moment before the copy. */
int ms_chain_run(const u8 *dol, u32 len, int argc, char **argv, const struct ms_chain_hooks *hk)
{
	struct msdol_plan plan;
	struct tramp_args *args;
	u32 code_len = (u32)((u8 *)ms_tramp2_end - (u8 *)ms_tramp2_start), i, alen, first_end, p0, p1;
	struct __argv *a = (struct __argv *)0x80001808;
	char blk[ARGS_MAX], cr[200];
	void (*go)(const struct tramp_args *);
#define CRUMB(...) do { if (hk && hk->crumb) { snprintf(cr, sizeof cr, __VA_ARGS__); hk->crumb(cr); } } while (0)
	if (msdol_check(dol, len, &plan) != MSDOL_OK) return -1;
	args = (struct tramp_args *)ms_chain_alloc2(sizeof *args);
	if (!args || code_len > 0x1000u) return -2;
	memset(args, 0, sizeof *args);
	args->entry = plan.entry; args->n = plan.n; args->bss = plan.bss; args->bss_len = plan.bss_len;
	for (i = 0; i < plan.n; i++) { args->sec[i].src = (u32)dol + plan.sec[i].src; args->sec[i].dst = plan.sec[i].dst; args->sec[i].len = plan.sec[i].len; }
	first_end = plan.sec[0].dst + plan.sec[0].len;
	p0 = plan.sec[0].len > 0x100u ? first_end - 0x7Cu : plan.sec[0].dst;       /* the last bytes of the first section (for the shell: the code the crash of died in) */
	p1 = plan.entry + 0x180u;                                                  /* the first call of the crt0 */
	args->probe0 = p0; args->probe1 = p1; args->mbox = MBOX_ADDR;
	CRUMB("chain step 1: plan ok, entry %08X, %u sections, bss %08X+%X, first section %08X+%X, tramp %u bytes at %08X, args %08X, dol buffer %08X", (unsigned)plan.entry, (unsigned)plan.n,
	      (unsigned)plan.bss, (unsigned)plan.bss_len, (unsigned)plan.sec[0].dst, (unsigned)plan.sec[0].len, (unsigned)code_len, TRAMP_ADDR, (unsigned)args, (unsigned)dol);
	{                                                                          /* what the image holds at the probes (what the copy must put there) */
		const u8 *i0 = dol + plan.sec[0].src;
		u32 w0 = 0, w1 = 0, w2 = 0, k;
		if (plan.entry >= plan.sec[0].dst && plan.entry + 4 <= first_end) for (k = 0; k < 4; k++) w0 = (w0 << 8) | i0[(plan.entry - plan.sec[0].dst) + k];
		if (p0 >= plan.sec[0].dst && p0 + 4 <= first_end) for (k = 0; k < 4; k++) w1 = (w1 << 8) | i0[(p0 - plan.sec[0].dst) + k];
		if (p1 >= plan.sec[0].dst && p1 + 4 <= first_end) for (k = 0; k < 4; k++) w2 = (w2 << 8) | i0[(p1 - plan.sec[0].dst) + k];
		CRUMB("chain step 2: image words: entry %08X, probe %08X = %08X, probe %08X = %08X", (unsigned)w0, (unsigned)p0, (unsigned)w1, (unsigned)p1, (unsigned)w2);
	}
	alen = msota_args_build(blk, sizeof blk, argc, (const char *const *)argv);
#ifndef MS_CHAIN_TEST
	{
		GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
		args->xfb = (u32)VIDEO_GetCurrentFramebuffer();
		args->xfb_words = rm ? (u32)rm->fbWidth * (u32)rm->xfbHeight / 2u : 0;
		args->xfb_stride = rm ? (u32)rm->fbWidth / 2u : 0;
	}
#else
	args->xfb = 0xC0B00000u; args->xfb_words = 320u * 480u; args->xfb_stride = 320u;           /* the emulator test: a fake XFB, the stage squares are checked in memory */
#endif
	DCFlushRange(args, sizeof *args);
	DCFlushRange((void *)dol, len);
	CRUMB("chain step 3: args flushed, argument block %u bytes, xfb %08X; next: teardown steps", (unsigned)alen, (unsigned)args->xfb);
	/* stop what the shell started, step by step, each with a breadcrumb before and after (the SD is still mounted: a hang leaves the name of the call as the last line of ota.log).  Default set
	   only (ms_chain_steps.h); hk->full runs every step.  Nothing here may wait for an interrupt or an IOS reply. */
	for (i = 0; i < MSCH_NSTEPS; i++) {
		if (msch_steps[i].full_only && !(hk && hk->full) && !(i == MSCH_STM && hk && hk->late)) continue;      /* a LATE hand-over keeps this IOS (no reload): the old shell releases its own STM hook here, while its callback is still valid (the new shell's libogc cannot: it is refused a second hook; releasing it from the NEW shell blacked the screen) */
		CRUMB("teardown: %s ...", msch_steps[i].name);
#ifndef MS_CHAIN_TEST                      /* the emulator test has no shell around it: no video, GX, audio, ES or STM to stop */
		switch (i) {
		case MSCH_AUDIO: AUDIO_StopDMA(); break;
		case MSCH_VIDEO: VIDEO_Flush(); break;                                 /* the screen is NOT blanked: the message stays up (a failed copy paints it red) */
		case MSCH_GXFLUSH: GX_Flush(); break;
		case MSCH_GXDONE: GX_DrawDone(); break;
		case MSCH_STM: __STM_Close(); break;
		case MSCH_ES: __ES_Close(); break;
		case MSCH_EXC: __exception_closeall(); break;
		case MSCH_THR: __lwp_thread_closeall(); break;
		}
#endif
		CRUMB("teardown: %s done", msch_steps[i].name);
	}
	if (alen) {
		memcpy((void *)ARGS_ADDR, blk, alen);
		DCFlushRange((void *)ARGS_ADDR, alen);
		a->argvMagic = ARGV_MAGIC;
		a->commandLine = (char *)ARGS_ADDR;
		a->length = (int)alen;
		a->argc = 0; a->argv = NULL; a->endARGV = NULL;
		DCFlushRange(a, sizeof *a);
	}
	CRUMB("chain step 4: teardown done, argument block written; next: guard word, SD shut down, tramp, jump (the log ends with the next line if the SD is closed)");
	*(volatile u32 *)(CHAIN_MARK + 4) = (hk && hk->late) ? MSOTA_CHAIN_LATE : 0;                    /* the flavour: the new shell skips its IOS reload / patch and kernel load after a late hand-over */
	*(volatile u32 *)CHAIN_MARK = CHAIN_MAGIC;
	DCFlushRange((void *)CHAIN_MARK, 32);
	memset((void *)MBOX_ADDR, 0, 64);
	DCFlushRange((void *)MBOX_ADDR, 64);
	memcpy((void *)TRAMP_ADDR, (void *)ms_tramp2_start, code_len);
	DCFlushRange((void *)TRAMP_ADDR, code_len);
	ICInvalidateRange((void *)TRAMP_ADDR, code_len);
	CRUMB("chain step 5: tramp copied to %08X, SD shut down now, jumping (a later start with chain.try left over crashed after this line)", TRAMP_ADDR);
	if (hk && hk->shutdown) hk->shutdown();
	go = (void (*)(const struct tramp_args *))TRAMP_ADDR;
	go(args);
	return 0;
}
