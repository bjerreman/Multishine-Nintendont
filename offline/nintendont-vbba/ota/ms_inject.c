/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Name Entry inject, the loader side.  DEFAULT ON: a game that passed the disc lock (stock NTSC-U Melee v1.02), with memory card emulation on and the
 * card file <device>:/multishine/saves/GALE.raw present, gets the table of ota/ms_inject_tab.h
 * left at MSIJ_AREA just before the jump into the multi-DOL launcher, which applies it after the apploader loaded the game (ms_inject_apply.h).  <device>:/multishine/no_inject turns it off.  What it
 * does for the player: power on / start the game -> Name Entry opens by itself (the exploit loads Multishine, nothing to press), no progressive-scan question, no 'Create Game Data?' question. */
#include <gccore.h>
#include <string.h>
#include <stdio.h>

#include "global.h"
#include "ff_utf8.h"
#include "ms_inject_apply.h"
#include "ms_inject_tab.h"

extern void ms_ota_logf(const char *fmt, ...);     /* ms_ota_loader.c: SD:/multishine/ota.log */
static int want;

static int has_file(const char *tail)
{
	char p[80];
	FIL f;
	snprintf(p, sizeof p, "%s:/multishine/%s", GetRootDevice(), tail);
	if (f_open_char(&f, p, FA_READ | FA_OPEN_EXISTING) != FR_OK) return 0;
	f_close(&f);
	return 1;
}

/* called by the disc gate: disc_ok = the game passed the lock */
void ms_inject_check(int disc_ok)
{
	int d;
	want = 0;
	if (!disc_ok) return;
	d = msij_decide(disc_ok, has_file("no_inject"), (ncfg->Config & NIN_CFG_MEMCARDEMU) != 0, has_file("saves/GALE.raw"));
	if (d != MSIJ_ON) { ms_ota_logf("inject: off: %s", d == MSIJ_OFF_MARKER ? "no_inject marker" : d == MSIJ_OFF_EMU ? "memory card emulation is off" : "no card file saves/GALE.raw (the card route is not in use)"); return; }
	want = 1;
	ms_ota_logf("inject: on (default): %d hooks + cave of %d words at %08X will be applied by the launcher; status word at %08X (1 applied, 2 retail word mismatch = nothing applied, 3 bad table)", (int)MSIJ_NHOOKS, (int)MSIJ_CAVE_NWORDS, (unsigned)MSIJ_CAVE_ADDR, (unsigned)(MSIJ_AREA + 24U));
}

/* right before the jump into the launcher (after the kernel is up) */
void ms_inject_arm(void)
{
	unsigned n;
	if (!want) return;
	n = msij_pack((volatile msij_u32 *)MSIJ_AREA, MSIJ_NHOOKS, ms_inject_hooks, MSIJ_CAVE_ADDR, MSIJ_CAVE_NWORDS, MSIJ_CAVE_OLD_FIRST, ms_inject_cave);
	DCFlushRange((void *)MSIJ_AREA, n * 4U);
}

/* at a later start of the shell (after a return from the game without a power cycle): what the launcher said last time, once */
void ms_inject_prev_status(void)
{
	volatile msij_u32 *t = (volatile msij_u32 *)MSIJ_AREA;
	if (t[0] != MSIJ_MAGIC) return;
	ms_ota_logf("inject: the previous launch left status %u (1 applied, 2 a retail word did not match: nothing applied, normal boot; 3 bad table; 0 not run)", (unsigned)t[6]);
	t[0] = 0;
	DCFlushRange((void *)MSIJ_AREA, 32);
}
