/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Keeping the old card file before a new virtual card is written (card_virtual in ms_ota_loader.c): pure decision logic, host-tested (tests/bak_test.c).
 * Try the rename first; if it fails for ANY reason (the Wii's FatFs answered FR_NO_FILE for a GALE.raw that exists: a stale directory cache) make the backup by a verified COPY instead.
 * Only when both fail is there an error, and then nothing has been changed (the old file is untouched). */
#ifndef MS_BAK_H
#define MS_BAK_H

enum { MSBAK_RENAMED = 0, MSBAK_COPIED = 1, MSBAK_FAILED = 2 };

struct msbak_ops {
	void *ctx;
	int (*rename)(void *ctx);        /* 0 = ok, else the error code */
	int (*copy)(void *ctx);          /* copy + verify read-back: 0 = ok, else the error code of the failing step */
	void (*note)(void *ctx, int state, int rename_rc, int copy_rc);   /* log */
};

/* -> MSBAK_*; *code = the code to show on failure (the copy's, the last step tried), 0 otherwise.  The copy is only tried after a failed rename; the rename is never tried twice here. */
static inline int msbak_keep(const struct msbak_ops *o, int *code)
{
	int rr = o->rename(o->ctx), cr = 0, st;
	*code = 0;
	if (rr == 0) st = MSBAK_RENAMED;
	else {
		cr = o->copy(o->ctx);
		if (cr == 0) st = MSBAK_COPIED;
		else { st = MSBAK_FAILED; *code = cr; }
	}
	if (o->note) o->note(o->ctx, st, rr, cr);
	return st;
}

/* after a failed write: how the old card file comes back.  A rename backup is renamed back, a copy backup is copied back; the backup copy stays when the copy back fails.  -> 1 = the old file is back */
static inline int msbak_restore(int state, int (*rename_back)(void *), int (*copy_back)(void *), void *ctx)
{
	if (state == MSBAK_RENAMED) return rename_back(ctx) == 0;
	if (state == MSBAK_COPIED) return copy_back(ctx) == 0;
	return 0;
}


/* Which backup name to use for the old card file: slots 0..98 = GALE.raw.old, .old2 .. .old99.  exists[i] = the slot is taken, when[i] = its modification time as one number (FatFs fdate << 16 | ftime; 0 = unknown).
   The first free slot wins.  When all MSBAK_N are taken the OLDEST is dropped and its name reused (*drop = 1): the smallest time; when that is not clear (a time is unknown, or all are equal) the last slot (.old99),
   among equal smallest times the highest slot.  Never blocks. */
#define MSBAK_N 99
static inline int msbak_pick(const int exists[MSBAK_N], const unsigned long when[MSBAK_N], int *drop)
{
	int i, best = MSBAK_N - 1, unclear = 0;
	unsigned long mn = 0xFFFFFFFFul, mx = 0;
	*drop = 0;
	for (i = 0; i < MSBAK_N; i++) if (!exists[i]) return i;
	*drop = 1;
	for (i = 0; i < MSBAK_N; i++) { if (when[i] == 0) unclear = 1; if (when[i] < mn) mn = when[i]; if (when[i] > mx) mx = when[i]; }
	if (unclear || mn == mx) return MSBAK_N - 1;
	for (i = MSBAK_N - 1; i >= 0; i--) if (when[i] == mn) { best = i; break; }
	return best;
}

#endif
