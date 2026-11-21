/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_bak.h (keeping the old card file: rename, else verified copy, else error); tests/test_theme.py builds and runs it. */
#include <stdio.h>
#include "ms_bak.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
struct sim { int rename_rc, copy_rc, renames, copies, noted_state, restored_by; };
static int s_rename(void *c) { struct sim *s = c; s->renames++; return s->rename_rc; }
static int s_copy(void *c) { struct sim *s = c; s->copies++; return s->copy_rc; }
static void s_note(void *c, int st, int rr, int cr) { struct sim *s = c; s->noted_state = st; (void)rr; (void)cr; }
static int b_ren(void *c) { ((struct sim *)c)->restored_by = 1; return 0; }
static int b_cpy(void *c) { ((struct sim *)c)->restored_by = 2; return 0; }
static int b_cpy_fail(void *c) { (void)c; return 5; }
int main(void)
{
	struct sim s;
	struct msbak_ops o;
	int code, st;
	o.ctx = &s; o.rename = s_rename; o.copy = s_copy; o.note = s_note;
	s = (struct sim){ 0, 0, 0, 0, -1, 0 };                       /* rename works: no copy is made */
	st = msbak_keep(&o, &code);
	CHECK(st == MSBAK_RENAMED && code == 0 && s.renames == 1 && s.copies == 0 && s.noted_state == MSBAK_RENAMED);
	s = (struct sim){ 4, 0, 0, 0, -1, 0 };                       /* FR_NO_FILE from the rename: kept by copy, no error */
	st = msbak_keep(&o, &code);
	CHECK(st == MSBAK_COPIED && code == 0 && s.renames == 1 && s.copies == 1 && s.noted_state == MSBAK_COPIED);
	s = (struct sim){ 4, 7, 0, 0, -1, 0 };                       /* both fail: error with the copy's code, nothing else tried */
	st = msbak_keep(&o, &code);
	CHECK(st == MSBAK_FAILED && code == 7 && s.renames == 1 && s.copies == 1 && s.noted_state == MSBAK_FAILED);
	o.note = 0;                                                  /* the log hook is optional */
	s = (struct sim){ 1, 0, 0, 0, -1, 0 };
	CHECK(msbak_keep(&o, &code) == MSBAK_COPIED);
	/* restore after a failed write */
	s = (struct sim){ 0 };
	CHECK(msbak_restore(MSBAK_RENAMED, b_ren, b_cpy, &s) == 1 && s.restored_by == 1);
	s = (struct sim){ 0 };
	CHECK(msbak_restore(MSBAK_COPIED, b_ren, b_cpy, &s) == 1 && s.restored_by == 2);
	s = (struct sim){ 0 };
	CHECK(msbak_restore(MSBAK_COPIED, b_ren, b_cpy_fail, &s) == 0 && s.restored_by == 0);
	CHECK(msbak_restore(MSBAK_FAILED, b_ren, b_cpy, &s) == 0 && s.restored_by == 0);
	{   /* the slot chooser */
		int ex[MSBAK_N], dr, i;
		unsigned long w[MSBAK_N];
		for (i = 0; i < MSBAK_N; i++) { ex[i] = 1; w[i] = 1000 + (unsigned long)i; }
		ex[0] = 0; CHECK(msbak_pick(ex, w, &dr) == 0 && dr == 0);                      /* .old free: first */
		ex[0] = 1; ex[3] = 0; CHECK(msbak_pick(ex, w, &dr) == 3 && dr == 0);           /* the lowest free slot */
		ex[1] = 0; CHECK(msbak_pick(ex, w, &dr) == 1 && dr == 0);
		for (i = 0; i < MSBAK_N; i++) ex[i] = 1;                                             /* all 99 taken: the oldest goes */
		ex[98] = 0; CHECK(msbak_pick(ex, w, &dr) == 98 && dr == 0);                   /* the last free slot is still used before anything is dropped */
		ex[98] = 1;
		w[40] = 500; CHECK(msbak_pick(ex, w, &dr) == 40 && dr == 1);
		w[60] = 500; CHECK(msbak_pick(ex, w, &dr) == 60 && dr == 1);                     /* tie for the oldest: the higher slot */
		for (i = 0; i < MSBAK_N; i++) w[i] = 777;                                            /* all equal: .old99 */
		CHECK(msbak_pick(ex, w, &dr) == MSBAK_N - 1 && dr == 1);
		for (i = 0; i < MSBAK_N; i++) w[i] = 1000 + (unsigned long)i; w[2] = 0;              /* a time unknown: .old99 */
		CHECK(msbak_pick(ex, w, &dr) == MSBAK_N - 1 && dr == 1);
	}
	printf(fails ? "bak: %d FAILED\n" : "bak: all passed\n", fails);
	return fails != 0;
}
