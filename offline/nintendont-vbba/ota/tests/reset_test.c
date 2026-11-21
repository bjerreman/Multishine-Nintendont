/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_reset.h (tests/test_reset.py runs it): the lists, the plan on a fake SD, the one-generation backup, failures, the confirm state machine and the 2 s hold.  Prints OK. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_reset.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* a fake SD: a flat set of full paths (a folder is its own path; its children are listed as "folder/child" too, moved with it) */
#define MAXP 80
struct fake { char p[MAXP][96]; int n; char badren[96]; int ren_lies; char baddel[96]; char badmkdir; int touched_outside, log_n; int logs[40]; char logn[40][32]; };
static int fx_find(struct fake *f, const char *s) { int i; for (i = 0; i < f->n; i++) if (!strcmp(f->p[i], s)) return i; return -1; }
static void fx_add(struct fake *f, const char *s) { if (fx_find(f, s) < 0) { snprintf(f->p[f->n++], 96, "%s", s); } }
static int under(const char *s, const char *dir) { size_t l = strlen(dir); return !strncmp(s, dir, l) && (s[l] == '/' || s[l] == 0); }
static int fx_exists(void *c, const char *p) { return fx_find(c, p) >= 0; }
static void chk_scope(struct fake *f, const char *p) { if (!under(p, "sd:/multishine")) f->touched_outside++; }
static int fx_rename(void *c, const char *a, const char *b)
{
	struct fake *f = c; int i; size_t la = strlen(a);
	chk_scope(f, a); chk_scope(f, b);
	if (!strcmp(f->badren, a)) return -9;                     /* a real failure: nothing moved */
	for (i = 0; i < f->n; i++) if (under(f->p[i], a)) { char t[96]; snprintf(t, sizeof t, "%s%s", b, f->p[i] + la); snprintf(f->p[i], 96, "%s", t); }
	return f->ren_lies ? 1 : 0;                                /* the SD driver reported an error for a rename that did happen */
}
static int fx_remove(void *c, const char *a)
{
	struct fake *f = c; int i = 0;
	chk_scope(f, a);
	if (!strcmp(f->baddel, a)) return -7;
	while (i < f->n) { if (under(f->p[i], a)) { memmove(f->p[i], f->p[f->n - 1], 96); f->n--; } else i++; }
	return 0;
}
static int fx_mkdir(void *c, const char *a) { struct fake *f = c; chk_scope(f, a); if (f->badmkdir) return -5; fx_add(f, a); return 0; }
static void fx_log(void *c, const char *n, int how, int rc) { struct fake *f = c; (void)rc; if (f->log_n < 40) { f->logs[f->log_n] = how; snprintf(f->logn[f->log_n], 32, "%s", n); f->log_n++; } }
static struct msr_ops ops(struct fake *f) { struct msr_ops o; o.ctx = f; o.exists = fx_exists; o.rename = fx_rename; o.remove = fx_remove; o.mkdir = fx_mkdir; o.log = fx_log; return o; }

static void fill(struct fake *f)
{
	int i;
	memset(f, 0, sizeof *f);
	for (i = 0; MSR_REMOVE[i]; i++) { char p[96]; snprintf(p, sizeof p, "sd:/multishine/%s", MSR_REMOVE[i]); fx_add(f, p); }
	for (i = 0; MSR_KEEP[i]; i++) { char p[96]; if (!strcmp(MSR_KEEP[i], MSR_BACKUP)) continue; snprintf(p, sizeof p, "sd:/multishine/%s", MSR_KEEP[i]); fx_add(f, p); }
	fx_add(f, "sd:/multishine/saves/GALE.raw"); fx_add(f, "sd:/multishine/saves/GALE_B.raw"); fx_add(f, "sd:/multishine/saves/GALE.raw.old2"); fx_add(f, "sd:/multishine/saves/GALE.raw.bak");
	fx_add(f, "sd:/games/melee.iso"); fx_add(f, "sd:/apps/x/boot.dol"); fx_add(f, "sd:/boot.dol"); fx_add(f, "sd:/ndebug.log"); fx_add(f, "sd:/multishine/unknown.txt");
}
static int has(struct fake *f, const char *p) { return fx_find(f, p) >= 0; }

static void lists(void)
{
	int i, j;
	for (i = 0; MSR_REMOVE[i]; i++) {
		CHECK(msr_name_ok(MSR_REMOVE[i]));
		CHECK(!msr_in(MSR_KEEP, MSR_REMOVE[i]));                       /* the lists do not overlap */
		CHECK(msr_class(MSR_REMOVE[i]) == MSR_C_REMOVE);
		for (j = i + 1; MSR_REMOVE[j]; j++) CHECK(strcmp(MSR_REMOVE[i], MSR_REMOVE[j]));
	}
	for (i = 0; MSR_KEEP[i]; i++) CHECK(msr_class(MSR_KEEP[i]) == MSR_C_KEEP);
	/* our keep list */
	CHECK(msr_class("shell.dol") == MSR_C_KEEP && msr_class("shl.st") == MSR_C_KEEP && msr_class("shl.ok") == MSR_C_KEEP && msr_class("shell.bak") == MSR_C_KEEP);
	CHECK(msr_class("ota_off") == MSR_C_KEEP && msr_class("no_drive_probe") == MSR_C_KEEP && msr_class("native_si") == MSR_C_KEEP && msr_class("no_inject") == MSR_C_KEEP && msr_class("teardown_full") == MSR_C_KEEP);
	/* our remove list */
	{ const char *w[] = { "saves", "card.mscf", "crd.st", "pack.mscf", "pack.st", "pack.bak", "disc.cache", "ota.chk", "nincfg.bin", "autoboot_set", "slotb_done", "slotb_fail", "slotb_existing", "ota.log", "inject", "inplace", "reboot", "WPROBE.TMP", "backup", NULL };
	  for (i = 0; w[i]; i++) CHECK(msr_class(w[i]) == MSR_C_REMOVE); }
	CHECK(msr_class("games") == MSR_C_OTHER && msr_class("apps") == MSR_C_OTHER && msr_class("../saves") == MSR_C_OTHER && msr_class("saves/GALE.raw") == MSR_C_OTHER && msr_class("") == MSR_C_OTHER && msr_class("..") == MSR_C_OTHER);
}

static void plan(void)
{
	struct fake f; struct msr_ops o; struct msr_result r; int rc;
	fill(&f); o = ops(&f);
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_OK && r.failed == 0 && r.deleted == 0 && !r.aborted);
	CHECK(r.moved == 26 && r.absent == 0);
	CHECK(f.touched_outside == 0);
	/* removed: moved into reset_backup, saves folder whole with its contents */
	CHECK(!has(&f, "sd:/multishine/saves") && !has(&f, "sd:/multishine/saves/GALE.raw") && !has(&f, "sd:/multishine/card.mscf") && !has(&f, "sd:/multishine/nincfg.bin"));
	CHECK(has(&f, "sd:/multishine/reset_backup/saves/GALE.raw") && has(&f, "sd:/multishine/reset_backup/saves/GALE_B.raw") && has(&f, "sd:/multishine/reset_backup/saves/GALE.raw.old2") && has(&f, "sd:/multishine/reset_backup/saves/GALE.raw.bak"));
	CHECK(has(&f, "sd:/multishine/reset_backup/nincfg.bin") && has(&f, "sd:/multishine/reset_backup/ota.log"));
	/* kept: every keep-listed name, the unknown file, everything outside multishine/ */
	{ int i; for (i = 0; MSR_KEEP[i]; i++) { char p[96]; if (!strcmp(MSR_KEEP[i], MSR_BACKUP)) continue; snprintf(p, sizeof p, "sd:/multishine/%s", MSR_KEEP[i]); CHECK(has(&f, p)); } }
	CHECK(has(&f, "sd:/multishine/shell.dol") && has(&f, "sd:/multishine/shl.st") && has(&f, "sd:/multishine/shl.ok") && has(&f, "sd:/multishine/shell.bak"));
	CHECK(has(&f, "sd:/multishine/unknown.txt") && has(&f, "sd:/games/melee.iso") && has(&f, "sd:/apps/x/boot.dol") && has(&f, "sd:/boot.dol") && has(&f, "sd:/ndebug.log"));
	CHECK(f.log_n == 26);
	/* run again on the clean SD: the first backup is dropped (one generation), nothing to move */
	CHECK(has(&f, "sd:/multishine/reset_backup/saves/GALE.raw"));
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_OK && r.moved == 0 && r.absent == 26);
	CHECK(!has(&f, "sd:/multishine/reset_backup/saves/GALE.raw") && has(&f, "sd:/multishine/reset_backup"));
	/* a second save, then a second reset: only the newest generation exists */
	fx_add(&f, "sd:/multishine/saves"); fx_add(&f, "sd:/multishine/saves/GALE.raw"); fx_add(&f, "sd:/multishine/card.mscf");
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_OK && r.moved == 2);
	CHECK(has(&f, "sd:/multishine/reset_backup/saves/GALE.raw") && has(&f, "sd:/multishine/reset_backup/card.mscf") && !has(&f, "sd:/multishine/reset_backup/nincfg.bin"));
	CHECK(f.touched_outside == 0);
}

static void failures(void)
{
	struct fake f; struct msr_ops o; struct msr_result r; int rc; char t[100];
	/* the SD driver lies about a rename that happened: counted as moved */
	fill(&f); f.ren_lies = 1; o = ops(&f);
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_OK && r.moved == 26 && r.failed == 0 && r.deleted == 0 && has(&f, "sd:/multishine/reset_backup/saves/GALE.raw"));
	/* one rename really fails: falls back to a delete, reported as deleted, the rest still goes */
	fill(&f); snprintf(f.badren, sizeof f.badren, "sd:/multishine/crd.st"); o = ops(&f);
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_OK && r.deleted == 1 && r.moved == 25 && r.failed == 0 && !has(&f, "sd:/multishine/crd.st") && !has(&f, "sd:/multishine/reset_backup/crd.st"));
	{ int i, seen = 0; for (i = 0; i < f.log_n; i++) if (!strcmp(f.logn[i], "crd.st")) { seen = 1; CHECK(f.logs[i] == MSR_H_DELETED); } CHECK(seen); }
	/* neither move nor delete works for one file: partial, named, the others are done */
	fill(&f); snprintf(f.badren, sizeof f.badren, "sd:/multishine/saves"); snprintf(f.baddel, sizeof f.baddel, "sd:/multishine/saves"); o = ops(&f);
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_PARTIAL && r.failed == 1 && !strcmp(r.first_fail, "saves") && r.first_rc == -9);
	CHECK(has(&f, "sd:/multishine/saves/GALE.raw") && !has(&f, "sd:/multishine/card.mscf") && has(&f, "sd:/multishine/reset_backup/card.mscf"));
	{ int i, seen = 0; for (i = 0; i < f.log_n; i++) if (!strcmp(f.logn[i], "saves")) { seen = 1; CHECK(f.logs[i] == MSR_H_FAILED); } CHECK(seen); }
	msr_fail_text(t, sizeof t, &r); CHECK(strstr(t, "saves") != NULL && strstr(t, "-9") != NULL);
	/* the old backup cannot be cleared: nothing is touched, not even a delete fallback */
	fill(&f); fx_add(&f, "sd:/multishine/reset_backup"); fx_add(&f, "sd:/multishine/reset_backup/old.raw"); snprintf(f.baddel, sizeof f.baddel, "sd:/multishine/reset_backup"); o = ops(&f);
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_ABORT && r.aborted && r.moved == 0 && r.deleted == 0 && has(&f, "sd:/multishine/saves/GALE.raw") && has(&f, "sd:/multishine/nincfg.bin") && has(&f, "sd:/multishine/reset_backup/old.raw"));
	msr_fail_text(t, sizeof t, &r); CHECK(strstr(t, "Nothing was changed") != NULL);
	/* the backup folder cannot be made: nothing is touched */
	fill(&f); f.badmkdir = 1; o = ops(&f);
	rc = msr_run(&o, &r);
	CHECK(rc == MSR_ABORT && has(&f, "sd:/multishine/saves/GALE.raw") && has(&f, "sd:/multishine/card.mscf") && f.log_n == 1);
	CHECK(f.touched_outside == 0);
}

static void ui(void)
{
	struct msr_ui u; unsigned t; int r, went = 0;
	/* opens on Back; a tap of A there leaves, and a B leaves */
	msr_ui_begin(&u); CHECK(u.focus == MSR_F_BACK);
	CHECK(msr_ui_frame(&u, 0, 0, 0, 1, 1, 0) == MSR_EXIT);
	msr_ui_begin(&u); CHECK(msr_ui_frame(&u, 0, 0, 1, 0, 0, 0) == MSR_EXIT);
	/* the A that opened the screen (still down, no edge) does nothing at all */
	msr_ui_begin(&u); CHECK(msr_ui_frame(&u, 0, 0, 0, 0, 1, 0) == MSR_NONE);
	/* down moves to Remove; a tap of A there does nothing */
	CHECK(msr_ui_frame(&u, 0, 1, 0, 0, 0, 10) == MSR_REDRAW && u.focus == MSR_F_REMOVE);
	CHECK(msr_ui_frame(&u, 0, 0, 0, 1, 1, 20) == MSR_REDRAW);              /* the hold starts (redraw) */
	CHECK(msr_ui_frame(&u, 0, 0, 0, 0, 0, 120) != MSR_GO);                  /* released after 100 ms */
	for (t = 200; t < 5000; t += 20) CHECK(msr_ui_frame(&u, 0, 0, 0, 0, 0, t) != MSR_GO);
	/* 1999 ms of continuous A is not enough, 2000 is */
	CHECK(msr_ui_frame(&u, 0, 0, 0, 1, 1, 10000) == MSR_REDRAW);
	for (t = 10020; t < 11999; t += 20) { r = msr_ui_frame(&u, 0, 0, 0, 0, 1, t); CHECK(r != MSR_GO && r != MSR_EXIT); }
	CHECK(msr_ui_frame(&u, 0, 0, 0, 0, 1, 11999) == MSR_NONE || msr_ui_frame(&u, 0, 0, 0, 0, 1, 11999) == MSR_REDRAW);
	CHECK(u.pm < 1000);
	r = msr_ui_frame(&u, 0, 0, 0, 0, 1, 12000); CHECK(r == MSR_GO);
	/* released at 1.5 s and pressed again: the count starts over */
	msr_ui_begin(&u); msr_ui_frame(&u, 1, 0, 0, 0, 0, 0);
	msr_ui_frame(&u, 0, 0, 0, 1, 1, 100);
	for (t = 120; t < 1600; t += 20) msr_ui_frame(&u, 0, 0, 0, 0, 1, t);
	msr_ui_frame(&u, 0, 0, 0, 0, 0, 1600);                                  /* up */
	msr_ui_frame(&u, 0, 0, 0, 1, 1, 1700);
	for (t = 1720; t < 3600; t += 20) { r = msr_ui_frame(&u, 0, 0, 0, 0, 1, t); if (r == MSR_GO) went = 1; }
	CHECK(!went);                                                            /* 1900 ms since the second press: not yet */
	CHECK(msr_ui_frame(&u, 0, 0, 0, 0, 1, 3700) == MSR_GO);
	/* B during a hold leaves and cancels; moving the focus cancels */
	msr_ui_begin(&u); msr_ui_frame(&u, 0, 1, 0, 0, 0, 0); msr_ui_frame(&u, 0, 0, 0, 1, 1, 10);
	CHECK(msr_ui_frame(&u, 0, 0, 1, 0, 1, 500) == MSR_EXIT && !u.hold_on);
	msr_ui_begin(&u); msr_ui_frame(&u, 0, 1, 0, 0, 0, 0); msr_ui_frame(&u, 0, 0, 0, 1, 1, 10);
	CHECK(msr_ui_frame(&u, 1, 0, 0, 0, 1, 500) == MSR_REDRAW && u.focus == MSR_F_BACK && !u.hold_on);
	CHECK(msr_ui_frame(&u, 0, 0, 0, 0, 1, 2600) != MSR_GO);                  /* A still down on Back with no edge: nothing */
}

int main(void)
{
	lists(); plan(); failures(); ui();
	puts(fails ? "FAILED" : "OK");
	return fails != 0;
}
