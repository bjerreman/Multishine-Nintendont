/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The teardown steps of the in-place hand-over, in order (ms_chain.c runs them, tests/chain_steps_test.c checks the table).  Wii evidence: the hand-over hung between 'chain step 3' and
 * 'chain step 4', i.e. inside the old teardown block.  Default set = what is needed to leave the hardware quiet and what cannot block: the audio DMA, the video registers, a GX FIFO flush (no wait).
 * The rest is NOT run by default (marker SD:/multishine/teardown_full runs every step, each with a breadcrumb before and after, to find the one that blocks):
 *   gx_drawdone   waits for the draw-done interrupt (LWP_ThreadSleep until the interrupt wakes it): can block for good after an IOS reload or with the loader's GX state
 *   stm_close / es_close   IOS_Close on file descriptors the IOS58 reload made stale (the stub never closes them either: the Homebrew Channel's are left open)
 *   exception_closeall   swallows every exception from here on (an exception during the copy is then invisible instead of the exception screen)
 *   thread_closeall   spares the running thread (libogc lwp_threads.c) but is not needed: the tramp runs with interrupts off, no scheduler runs after it */
#ifndef MS_CHAIN_STEPS_H
#define MS_CHAIN_STEPS_H
enum { MSCH_AUDIO = 0, MSCH_VIDEO, MSCH_GXFLUSH, MSCH_GXDONE, MSCH_STM, MSCH_ES, MSCH_EXC, MSCH_THR, MSCH_NSTEPS };
struct msch_step { const char *name; int full_only; };
static const struct msch_step msch_steps[MSCH_NSTEPS] = {
    { "AUDIO_StopDMA", 0 }, { "VIDEO_Flush", 0 }, { "GX_Flush", 0 },
    { "GX_DrawDone", 1 }, { "__STM_Close", 1 }, { "__ES_Close", 1 }, { "__exception_closeall", 1 }, { "__lwp_thread_closeall", 1 }
};
#endif
