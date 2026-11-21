/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_cardroute.h: the route of the quick set-up (no card / card in A / card in B / unformatted / other region / short / fits), the answer keys, the block figures and the texts.
 * Prints OK or the first failure. */
#include <stdio.h>
#include <string.h>
#include "ms_cardroute.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static struct mscr_slot S(int st, unsigned fr, unsigned ours) { struct mscr_slot s; s.state = st; s.free_blocks = fr; s.ours = ours; return s; }

int main(void)
{
	struct mscr_slot none = S(MSCR_S_NONE, 0, 0), a;
	struct mscr_decision d;
	char t[160];
	const char *ba, *aa;
	const unsigned F = MSCR_FULL_BLOCKS;
	/* the figure: one place, today 151 + 30 + 6 */
	CHECK(MSCR_FULL_BLOCKS == 187u && mscr_full(0) == 187u && mscr_full(151) == 187u && mscr_full(155) == 191u);
	CHECK(MSCR_VCARD_BLOCKS == 2u * 1024u * 1024u / 0x2000u - 5u && MSCR_FULL_BLOCKS <= MSCR_VCARD_BLOCKS);
	CHECK(mscr_need(190, 0) == 190 && mscr_need(190, 150) == 40 && mscr_need(190, 200) == 0);
	CHECK(mscr_short(189, 190) == 1 && mscr_short(190, 190) == 0 && mscr_short(0, 190) == 190);
	CHECK(mscr_is_ours("MultishineMelee") && mscr_is_ours("MultishinePack") && mscr_is_ours("MultishineIdentity") && mscr_is_ours("SuperSmashBros0110290334") && !mscr_is_ours("SuperSmashBros0110290334x") && !mscr_is_ours("MultishineBridge"));
	/* consoles */
	CHECK(mscr_console(0, 0) == MSCR_CON_WII && mscr_console(1, 0) == MSCR_CON_WIIU && mscr_console(0, 1) == MSCR_CON_WIIVC && mscr_console(1, 1) == MSCR_CON_WIIU);
	CHECK(mscr_probe_allowed(MSCR_CON_WII) && mscr_probe_allowed(MSCR_CON_GC) && !mscr_probe_allowed(MSCR_CON_WIIU) && !mscr_probe_allowed(MSCR_CON_WIIVC));
	/* no card sensed: the virtual card, no question */
	d = mscr_decide(MSCR_CON_WII, &none, 0, F); CHECK(d.route == MSCR_R_VIRTUAL && d.why == MSCR_W_NOCARD);
	mscr_route_text(t, sizeof t, &d, d.route); CHECK(!strcmp(t, "No memory card found: using a virtual card"));
	/* a card in slot B only: never installed to, the virtual card (B is information only) */
	d = mscr_decide(MSCR_CON_WII, &none, 0, F); CHECK(d.route == MSCR_R_VIRTUAL);
	/* a card in slot A with room: the question; A = the virtual card on a Wii, the memory card on a GameCube */
	a = S(MSCR_S_READY, 212, 0);
	d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.route == MSCR_R_ASK && d.why == MSCR_W_FITS && d.need == 187 && !d.default_phys);
	CHECK(mscr_answer(&d, 1) == MSCR_R_VIRTUAL && mscr_answer(&d, 0) == MSCR_R_PHYS);
	mscr_ask_keys(&d, &ba, &aa); CHECK(!strcmp(aa, "Virtual card on the SD") && !strcmp(ba, "Memory card in slot A"));
	d = mscr_decide(MSCR_CON_GC, &a, 0, F); CHECK(d.route == MSCR_R_ASK && d.default_phys && mscr_answer(&d, 1) == MSCR_R_PHYS && mscr_answer(&d, 0) == MSCR_R_VIRTUAL);
	mscr_ask_keys(&d, &ba, &aa); CHECK(!strcmp(aa, "Memory card in slot A") && !strcmp(ba, "Virtual card on the SD"));
	mscr_route_text(t, sizeof t, &d, MSCR_R_PHYS); CHECK(!strcmp(t, "Using the memory card in slot A"));
	/* exactly enough, one short, our files already there count */
	a = S(MSCR_S_READY, 187, 0); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.route == MSCR_R_ASK);
	a = S(MSCR_S_READY, 186, 0); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.route == MSCR_R_VIRTUAL && d.why == MSCR_W_SHORT && d.short_by == 1);
	mscr_route_text(t, sizeof t, &d, d.route); CHECK(!strcmp(t, "The card in slot A is 1 block short: using a virtual card"));
	a = S(MSCR_S_READY, 36, 151); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.route == MSCR_R_ASK && d.need == 36);
	a = S(MSCR_S_READY, 12, 0); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.short_by == 175);
	mscr_short_text(t, sizeof t, 182, 12); CHECK(!strcmp(t, "Not enough space: 182 blocks needed, 12 free (170 short)."));
	/* an unformatted card, another region, something else: the virtual card, said plainly */
	a = S(MSCR_S_UNFMT, 0, 0); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.route == MSCR_R_VIRTUAL && d.why == MSCR_W_UNFMT);
	mscr_route_text(t, sizeof t, &d, d.route); CHECK(!strcmp(t, "The card in slot A is not formatted: using a virtual card"));
	a = S(MSCR_S_REGION, 0, 0); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.why == MSCR_W_REGION);
	a = S(MSCR_S_OTHER, 0, 0); d = mscr_decide(MSCR_CON_WII, &a, 0, F); CHECK(d.why == MSCR_W_OTHER);
	/* no ports (Wii U, Wii VC): never sensed, never asked; a virtual card that exists stays */
	a = S(MSCR_S_READY, 251, 0);
	d = mscr_decide(MSCR_CON_WIIU, &a, 0, F); CHECK(d.route == MSCR_R_VIRTUAL && d.why == MSCR_W_PORTS);
	mscr_route_text(t, sizeof t, &d, d.route); CHECK(!strcmp(t, "No memory card ports here: using a virtual card"));
	d = mscr_decide(MSCR_CON_WIIVC, &a, 0, F); CHECK(d.why == MSCR_W_PORTS);
	d = mscr_decide(MSCR_CON_WII, &a, 1, F); CHECK(d.route == MSCR_R_VIRTUAL && d.why == MSCR_W_VCARD);
	/* slot lines and the size line */
	a = S(MSCR_S_READY, 212, 0); mscr_slot_text(t, sizeof t, 'A', &a); CHECK(!strcmp(t, "Slot A: 212 free blocks"));
	a = S(MSCR_S_READY, 1, 0); mscr_slot_text(t, sizeof t, 'B', &a); CHECK(!strcmp(t, "Slot B: 1 free block"));
	mscr_slot_text(t, sizeof t, 'B', &none); CHECK(!strcmp(t, "Slot B: no memory card"));
	a = S(MSCR_S_UNFMT, 0, 0); mscr_slot_text(t, sizeof t, 'A', &a); CHECK(!strcmp(t, "Slot A: card not formatted"));
	mscr_need_text(t, sizeof t, F); CHECK(!strcmp(t, "Multishine needs about 187 blocks today. This can change with updates."));
	/* no 'MiB', no '59' in any player text */
	CHECK(!strstr(mscr_ask_text(), "MiB"));
	printf("OK\n");
	return 0;
}
