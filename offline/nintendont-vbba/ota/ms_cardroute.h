/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Which memory card the quick set-up uses, and how many blocks a full install takes (design rule).
 * Pure, host-tested (tests/cardroute_test.c, tests/test_cardroute.py); the loader (ms_ota_loader.c card_probe / ms_setup / card_physical / MemoryCardMenu) senses the slots and draws.
 *
 * The block figure lives HERE and nowhere else (MSCR_FULL_BLOCKS).  tests/test_cardroute.py recomputes each part from its source and fails when one moves:
 *   the five files of the card container crd-N.mscf (loader save 11 + MultishineCode 62 + MultishineOvl 66 + MultishineState 1 + MultishineMelee 11 = 151, crd-1283),
 *   MultishinePack, the system files built on the console from the player's disc (the lean pack: 30; 34 once the release ships the stage select entry, the project's other tooling --with-slmap,
 *: the test ties this to the release scripts),
 *   the small files the game writes later, one block each: MultishineIdentity (the sign-in), MultishineOnline, MultishineCodes, MultishineOptions, MultishineAdvisory, MultishineChat (6).
 * It is TODAY's size: an update can make the engine or the overlay bigger, so the screens say "about N blocks today; this can change with updates". */
#ifndef MS_CARDROUTE_H
#define MS_CARDROUTE_H
#include <stdio.h>
#include <string.h>

#define MSCR_FILES_BLOCKS 151u       /* the five files of crd-N.mscf (the runtime figure after a download is crd_set.blocks: mscr_full takes it) */
#define MSCR_PACK_BLOCKS   30u       /* MultishinePack (seed_lean, five entries: the release builds the pack data without --with-slmap) */
#define MSCR_DATA_BLOCKS    6u       /* the six one-block files the game writes */
#define MSCR_FULL_BLOCKS  (MSCR_FILES_BLOCKS + MSCR_PACK_BLOCKS + MSCR_DATA_BLOCKS)   /* 187 today */
#define MSCR_VCARD_BLOCKS 251u       /* the virtual card on the SD: 2 MiB = 256 blocks, 5 for the system area */
#define MSCR_NOURS 12
#define MSCR_PROBE_MS 6000u        /* the longest wait for a busy slot (no card answers at once); B stops it sooner */

/* every file of a full install: the five of the container (the only ones ever deleted, after an A that names them), the pack and the six small files */
static inline const char *const *mscr_our_names(void)
{
	static const char *const n[MSCR_NOURS] = { "SuperSmashBros0110290334", "MultishineCode", "MultishineOvl", "MultishineState", "MultishineMelee", "MultishinePack",
		"MultishineIdentity", "MultishineOnline", "MultishineCodes", "MultishineOptions", "MultishineAdvisory", "MultishineChat" };
	return n;
}
static inline int mscr_is_ours(const char *name)
{
	int i;
	for (i = 0; i < MSCR_NOURS; i++) if (strcmp(name, mscr_our_names()[i]) == 0) return 1;
	return 0;
}

/* the full install for the downloaded five files (0 = not downloaded yet: today's figure) */
static inline unsigned mscr_full(unsigned files_blocks) { return (files_blocks ? files_blocks : MSCR_FILES_BLOCKS) + MSCR_PACK_BLOCKS + MSCR_DATA_BLOCKS; }
/* the free blocks a card still needs: the full install less the blocks of our files already on it (they are replaced or kept, never counted twice) */
static inline unsigned mscr_need(unsigned full, unsigned ours) { return full > ours ? full - ours : 0; }
/* how many blocks are missing (0 = it fits) */
static inline unsigned mscr_short(unsigned free_blocks, unsigned need) { return free_blocks >= need ? 0 : need - free_blocks; }

/* ---- what a slot holds (card_probe) -------------------------------------------------------------------------------------------------------------------------------------- */
#define MSCR_S_NONE    0     /* no card (or B pressed during the probe, or the probe ran out of time) */
#define MSCR_S_READY   1     /* a formatted card of this region, free blocks read */
#define MSCR_S_UNFMT   2     /* not formatted or damaged */
#define MSCR_S_REGION  3     /* a card of another region */
#define MSCR_S_OTHER   4     /* something else in the slot, a read error, or a card that cannot hold the files (sector size) */
struct mscr_slot { int state; unsigned free_blocks; unsigned ours; };

#define MSCR_CON_GC    0     /* a GameCube (not this shell today: kept so the rule is written once) */
#define MSCR_CON_WII   1     /* an original Wii with GameCube ports */
#define MSCR_CON_WIIU  2     /* a Wii U (vWii): no GameCube ports */
#define MSCR_CON_WIIVC 3     /* a Wii VC title: no ports either */
static inline int mscr_console(int wiiu, int wiivc) { return wiiu ? MSCR_CON_WIIU : wiivc ? MSCR_CON_WIIVC : MSCR_CON_WII; }
/* the slots are sensed only where GameCube card ports exist */
static inline int mscr_probe_allowed(int console) { return console == MSCR_CON_GC || console == MSCR_CON_WII; }

#define MSCR_R_VIRTUAL 0     /* the virtual card on the SD, no question */
#define MSCR_R_ASK     1     /* a usable card in slot A: ask */
#define MSCR_R_PHYS    2     /* the answer: the card in slot A */
/* why */
#define MSCR_W_PORTS   0     /* no GameCube card ports */
#define MSCR_W_VCARD   1     /* a virtual card exists already: it stays in use */
#define MSCR_W_NOCARD  2     /* nothing in slot A (a card in slot B only is never installed to) */
#define MSCR_W_UNFMT   3
#define MSCR_W_REGION  4
#define MSCR_W_OTHER   5
#define MSCR_W_SHORT   6     /* a card in slot A, too few free blocks */
#define MSCR_W_FITS    7     /* a card in slot A with room: the question */
struct mscr_decision { int route; int why; unsigned need; unsigned short_by; int default_phys; };

/* the decision of the set-up: only slot A is ever installed to; slot B is information only */
static inline struct mscr_decision mscr_decide(int console, const struct mscr_slot *a, int have_vcard, unsigned full)
{
	struct mscr_decision d;
	d.route = MSCR_R_VIRTUAL; d.need = full; d.short_by = 0; d.default_phys = console == MSCR_CON_GC;
	if (!mscr_probe_allowed(console)) { d.why = MSCR_W_PORTS; return d; }
	if (have_vcard) { d.why = MSCR_W_VCARD; return d; }
	switch (a->state) {
	case MSCR_S_READY: break;
	case MSCR_S_UNFMT: d.why = MSCR_W_UNFMT; return d;
	case MSCR_S_REGION: d.why = MSCR_W_REGION; return d;
	case MSCR_S_OTHER: d.why = MSCR_W_OTHER; return d;
	default: d.why = MSCR_W_NOCARD; return d;
	}
	d.need = mscr_need(full, a->ours);
	d.short_by = mscr_short(a->free_blocks, d.need);
	if (d.short_by) { d.why = MSCR_W_SHORT; return d; }
	d.route = MSCR_R_ASK; d.why = MSCR_W_FITS;
	return d;
}
/* the answer to the question: A takes the default (the physical card on a GameCube, the virtual card on a Wii), B the other one */
static inline int mscr_answer(const struct mscr_decision *d, int pressed_a) { return (pressed_a ? d->default_phys : !d->default_phys) ? MSCR_R_PHYS : MSCR_R_VIRTUAL; }
/* the two keys of the question, in the footer order (B first, then A) */
static inline void mscr_ask_keys(const struct mscr_decision *d, const char **b_act, const char **a_act)
{
	*a_act = d->default_phys ? "Memory card in slot A" : "Virtual card on the SD";
	*b_act = d->default_phys ? "Virtual card on the SD" : "Memory card in slot A";
}

/* ---- the plain lines ------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* the route the set-up took and why (one centred line under the steps; also the ota.log line) */
static inline void mscr_route_text(char *o, unsigned n, const struct mscr_decision *d, int route)
{
	if (route == MSCR_R_PHYS) { snprintf(o, n, "Using the memory card in slot A"); return; }
	switch (d->why) {
	case MSCR_W_PORTS:  snprintf(o, n, "No memory card ports here: using a virtual card"); break;
	case MSCR_W_VCARD:  snprintf(o, n, "Using your virtual card on the SD"); break;
	case MSCR_W_NOCARD: snprintf(o, n, "No memory card found: using a virtual card"); break;
	case MSCR_W_UNFMT:  snprintf(o, n, "The card in slot A is not formatted: using a virtual card"); break;
	case MSCR_W_REGION: snprintf(o, n, "The card in slot A is for another region: using a virtual card"); break;
	case MSCR_W_OTHER:  snprintf(o, n, "The card in slot A cannot be used: using a virtual card"); break;
	case MSCR_W_SHORT:  snprintf(o, n, "The card in slot A is %u block%s short: using a virtual card", d->short_by, d->short_by == 1 ? "" : "s"); break;
	default:            snprintf(o, n, "Using a virtual card"); break;
	}
}
/* what a slot holds, for the question, the set-up and the Memory card screen ("Slot A: 212 free blocks") */
static inline void mscr_slot_text(char *o, unsigned n, char slot, const struct mscr_slot *s)
{
	switch (s->state) {
	case MSCR_S_READY:  snprintf(o, n, "Slot %c: %u free block%s", slot, s->free_blocks, s->free_blocks == 1 ? "" : "s"); break;
	case MSCR_S_UNFMT:  snprintf(o, n, "Slot %c: card not formatted", slot); break;
	case MSCR_S_REGION: snprintf(o, n, "Slot %c: card for another region", slot); break;
	case MSCR_S_OTHER:  snprintf(o, n, "Slot %c: cannot be used", slot); break;
	default:            snprintf(o, n, "Slot %c: no memory card", slot); break;
	}
}
/* the size of a full install, said as today's size */
static inline void mscr_need_text(char *o, unsigned n, unsigned full) { snprintf(o, n, "Multishine needs about %u blocks today. This can change with updates.", full); }
/* not enough room on a physical card, said before anything is written */
static inline void mscr_short_text(char *o, unsigned n, unsigned need, unsigned free_blocks)
{
	snprintf(o, n, "Not enough space: %u blocks needed, %u free (%u short).", need, free_blocks, mscr_short(free_blocks, need));
}
static inline const char *mscr_ask_text(void) { return "Where should Multishine go?"; }
#endif
