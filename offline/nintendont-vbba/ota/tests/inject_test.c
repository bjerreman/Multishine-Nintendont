/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Host driver for ms_inject_apply.h with the generated ms_inject_tab.h (tests/test_inject.py runs it):
 *   inject_test MODE      MODE = ok | other-word | other-cave | badmagic | toomany | outofrange | applied
 * A fake game memory (0x80000000 .. 0x81200000) holds the retail words the table expects (or not), the table is packed exactly like the loader does and applied exactly like the launcher does.
 * Prints STATUS n, CHANGED n (words that differ from before) and, for ok, "W address value" for every changed word. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned *fake;
#define MSIJ_MEM(a) ((volatile unsigned *)&fake[((a) - 0x80000000U) / 4U])
#include "ms_inject_apply.h"
#include "ms_inject_tab.h"

int main(int argc, char **argv)
{
	const unsigned words = (MSIJ_HI - 0x80000000U) / 4U;
	unsigned *before, tab[8 + 3 * MSIJ_NHOOKS + MSIJ_CAVE_NWORDS + 8], i, n, changed = 0, st;
	const char *mode = argc > 1 ? argv[1] : "ok";
	if (argc == 6 && !strcmp(mode, "decide")) { printf("DECIDE %d\n", msij_decide(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]))); return 0; }
	fake = (unsigned *)calloc(words, 4);
	before = (unsigned *)malloc((size_t)words * 4);
	for (i = 0; i < words; i++) fake[i] = 0xA5A50000U + (i & 0xFFFFU);                   /* filler that is nobody's code */
	for (i = 0; i < MSIJ_NHOOKS; i++) *MSIJ_MEM(ms_inject_hooks[i][0]) = ms_inject_hooks[i][1];
	*MSIJ_MEM(MSIJ_CAVE_ADDR) = MSIJ_CAVE_OLD_FIRST;
	if (!strcmp(mode, "other-word")) *MSIJ_MEM(ms_inject_hooks[2][0]) ^= 0x10U;
	if (!strcmp(mode, "other-cave")) *MSIJ_MEM(MSIJ_CAVE_ADDR) ^= 0x10U;
	if (!strcmp(mode, "applied")) for (i = 0; i < MSIJ_NHOOKS; i++) *MSIJ_MEM(ms_inject_hooks[i][0]) = ms_inject_hooks[i][2];    /* a disc that already carries the patch */
	memcpy(before, fake, (size_t)words * 4);
	n = msij_pack((volatile msij_u32 *)tab, MSIJ_NHOOKS, ms_inject_hooks, MSIJ_CAVE_ADDR, MSIJ_CAVE_NWORDS, MSIJ_CAVE_OLD_FIRST, ms_inject_cave);
	if (!strcmp(mode, "badmagic")) tab[0] ^= 1U;
	if (!strcmp(mode, "toomany")) tab[2] = 17U;
	if (!strcmp(mode, "outofrange")) tab[8] = 0x80000100U;
	st = msij_apply((volatile msij_u32 *)tab);
	for (i = 0; i < words; i++) if (fake[i] != before[i]) changed++;
	printf("STATUS %u\nCHANGED %u\nTABLEWORDS %u\n", st, changed, n);
	if (!strcmp(mode, "ok")) for (i = 0; i < words; i++) if (fake[i] != before[i]) printf("W %08X %08X\n", 0x80000000U + 4U * i, fake[i]);
	return 0;
}
