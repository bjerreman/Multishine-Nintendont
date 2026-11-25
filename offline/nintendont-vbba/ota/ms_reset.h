/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Reset Multishine: Start on the About page removes ALL Multishine settings and the virtual memory cards so the player can set up fresh.  Pure decisions, host-tested
 * (tests/reset_test.c): the lists, the plan, the confirm screen's state machine, the 2 s hold, the one-generation backup and the partial-failure rules.  The loader (ms_ota_loader.c ms_reset_screen)
 * draws and supplies the file operations.
 *
 * Only names directly inside SD:/multishine/ are ever touched, and only names in MSR_REMOVE.  A removed file is MOVED into SD:/multishine/reset_backup/ (one generation: the previous
 * reset_backup folder is deleted first).  A move that fails falls back to a delete of that file; a file that can neither be moved nor deleted is a failure that is logged and reported
 * with its name, the rest still goes on (no silent half-way stop).  If the backup folder cannot be cleared or made NOTHING is touched (never delete without a chance to undo).
 * The SD driver reports errors for renames that did happen: the directory is trusted (old name gone, new there = moved), as bk_rename does. */
#ifndef MS_RESET_H
#define MS_RESET_H
#include <stdio.h>
#include <string.h>

#define MSR_DIR    "sd:/multishine"
#define MSR_BACKUP "reset_backup"          /* name inside MSR_DIR */
#define MSR_HOLD_MS 2000u                   /* hold A for this long, continuously */

/* what goes: names inside SD:/multishine/ (files, or a folder = moved whole) */
static const char *const MSR_REMOVE[] = {
	"saves",                                  /* the virtual cards: GALE.raw, GALE_B.raw, GALE_B.tmp and every .old / .bak backup inside */
	"card.mscf", "card.bak", "card.new", "crd.st", "crd.app", "crd.pend",
	"pack.mscf", "pack.bak", "pack.new", "pack.st",
	"disc.cache", "ota.chk", "nincfg.bin", "autoboot_set",
	"slotb_done", "slotb_fail", "slotb_existing", "physical_card",       /* physical_card: the set-up chose the memory card in slot A (the card itself is never touched by a reset) */
	"ota.log", "ndebug.old",
	"inject", "inplace", "reboot", "WPROBE.TMP", "backup",
	NULL
};
/* what stays: the shell and its update state, the kernel and its update state, network caches, and the switches the player or tester made on purpose; reset_backup is the backup itself */
static const char *const MSR_KEEP[] = {
	"shell.dol", "shell.bak", "shell.new", "shell.bad", "shl.st", "shl.ok",
	"kernel.bin", "kernel.bak", "kernel.new", "kernel.bad", "ota.st", "chain.try", "chain.fail", "krn.rst",
	"ota.ip", "tls.seed", "ota_url.txt",
	"ota_off", "no_drive_probe", "native_si", "no_native_si", "no_inject", "teardown_full", "no_chain", "diag.no", "any_disc", "dns_on", "no_dns", "no_conn_check", "conn_check_on", "conn.txt", "no_card_update", "dev", "verbose", "ui_43", "ui_wide",
	MSR_BACKUP,
	NULL
};
enum { MSR_C_OTHER = 0, MSR_C_REMOVE = 1, MSR_C_KEEP = 2 };
static inline int msr_in(const char *const *l, const char *n) { int i; for (i = 0; l[i]; i++) if (!strcmp(l[i], n)) return 1; return 0; }
/* a plain name: no path separator, no dots-only, not empty */
static inline int msr_name_ok(const char *n) { return n && n[0] && !strchr(n, '/') && !strchr(n, '\\') && !strchr(n, ':') && strcmp(n, ".") && strcmp(n, ".."); }
static inline int msr_class(const char *n)
{
	if (!msr_name_ok(n)) return MSR_C_OTHER;
	if (msr_in(MSR_KEEP, n)) return MSR_C_KEEP;          /* keep wins, always */
	if (msr_in(MSR_REMOVE, n)) return MSR_C_REMOVE;
	return MSR_C_OTHER;                                    /* unknown names are left alone */
}

/* ---- the run ------------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { MSR_H_ABSENT = 0, MSR_H_MOVED = 1, MSR_H_DELETED = 2, MSR_H_FAILED = 3 };
struct msr_ops {
	void *ctx;
	int (*exists)(void *ctx, const char *path);                       /* a file or a folder */
	int (*rename)(void *ctx, const char *from, const char *to);       /* 0 = ok (a folder moves whole) */
	int (*remove)(void *ctx, const char *path);                       /* a file, or a folder with everything in it: 0 = ok */
	int (*mkdir)(void *ctx, const char *path);
	void (*log)(void *ctx, const char *name, int how, int rc);        /* one call per name, how = MSR_H_* (not for absent names) */
};
struct msr_result { int moved, deleted, failed, absent, aborted; char first_fail[32]; int first_rc; };
enum { MSR_OK = 0, MSR_PARTIAL = 1, MSR_ABORT = 2 };

/* -> MSR_OK all gone; MSR_PARTIAL some names failed (res->failed, first_fail); MSR_ABORT nothing was touched (the old backup could not be cleared or the new folder not made) */
static inline int msr_run(const struct msr_ops *o, struct msr_result *res)
{
	char bk[96], src[96], dst[96];
	int i;
	memset(res, 0, sizeof *res);
	snprintf(bk, sizeof bk, "%s/%s", MSR_DIR, MSR_BACKUP);
	if (o->exists(o->ctx, bk)) {                                       /* one generation: the previous backup goes first */
		o->remove(o->ctx, bk);
		if (o->exists(o->ctx, bk)) { res->aborted = 1; snprintf(res->first_fail, sizeof res->first_fail, "%s", MSR_BACKUP); res->first_rc = -1; if (o->log) o->log(o->ctx, MSR_BACKUP, MSR_H_FAILED, -1); return MSR_ABORT; }
	}
	{
		const int mr = o->mkdir(o->ctx, bk);
		if (mr != 0 && !o->exists(o->ctx, bk)) { res->aborted = 1; snprintf(res->first_fail, sizeof res->first_fail, "%s", MSR_BACKUP); res->first_rc = mr; if (o->log) o->log(o->ctx, MSR_BACKUP, MSR_H_FAILED, mr); return MSR_ABORT; }
	}
	for (i = 0; MSR_REMOVE[i]; i++) {
		const char *n = MSR_REMOVE[i];
		int rr, how, rc = 0;
		if (msr_class(n) != MSR_C_REMOVE) continue;                    /* never a keep-listed or unknown name, whatever the table says */
		snprintf(src, sizeof src, "%s/%s", MSR_DIR, n);
		snprintf(dst, sizeof dst, "%s/%s/%s", MSR_DIR, MSR_BACKUP, n);
		if (!o->exists(o->ctx, src)) { res->absent++; continue; }
		rr = o->rename(o->ctx, src, dst);
		if (rr == 0 || (!o->exists(o->ctx, src) && o->exists(o->ctx, dst))) { how = MSR_H_MOVED; res->moved++; }
		else {                                                          /* the move failed: delete instead; the rename's code is the one reported if that fails too */
			const int dr = o->remove(o->ctx, src);
			rc = rr;
			if (dr == 0 || !o->exists(o->ctx, src)) { how = MSR_H_DELETED; res->deleted++; }
			else { how = MSR_H_FAILED; res->failed++; if (!res->first_fail[0]) { snprintf(res->first_fail, sizeof res->first_fail, "%s", n); res->first_rc = rr; } }
		}
		if (o->log) o->log(o->ctx, n, how, rc);
	}
	return res->failed ? MSR_PARTIAL : MSR_OK;
}

/* ---- the confirm screen: two rows, focus starts on Back; a tap of A on Remove does nothing, holding A for MSR_HOLD_MS does it ---------------------------------------------------- */
enum { MSR_F_BACK = 0, MSR_F_REMOVE = 1 };
enum { MSR_NONE = 0, MSR_REDRAW = 1, MSR_EXIT = 2, MSR_GO = 3 };
struct msr_ui { int focus; int hold_on; unsigned t0, pm; };
static inline void msr_ui_begin(struct msr_ui *u) { u->focus = MSR_F_BACK; u->hold_on = 0; u->t0 = 0; u->pm = 0; }
/* one frame.  up/down/b/a_edge = pressed now (edges; the A that opened the screen is not one), a_held = A is down this frame, now in ms.  A B leaves at once, also during a hold. */
static inline int msr_ui_frame(struct msr_ui *u, int up, int down, int b, int a_edge, int a_held, unsigned now)
{
	if (b) { u->hold_on = 0; u->pm = 0; return MSR_EXIT; }
	if (up || down) {
		u->focus = u->focus == MSR_F_BACK ? MSR_F_REMOVE : MSR_F_BACK;
		u->hold_on = 0; u->pm = 0;
		return MSR_REDRAW;
	}
	if (u->focus == MSR_F_BACK) return a_edge ? MSR_EXIT : MSR_NONE;
	if (!u->hold_on) {
		if (!a_edge) return MSR_NONE;                                  /* only a fresh press starts a hold */
		u->hold_on = 1; u->t0 = now; u->pm = 0;
		return MSR_REDRAW;
	}
	if (!a_held) { const int ch = u->pm != 0; u->hold_on = 0; u->pm = 0; return ch ? MSR_REDRAW : MSR_NONE; }   /* released early: nothing happens */
	{
		const unsigned el = now - u->t0, pm = el >= MSR_HOLD_MS ? 1000u : el * 1000u / MSR_HOLD_MS;
		const int ch = (pm / 25u) != (u->pm / 25u);
		u->pm = pm;
		if (el >= MSR_HOLD_MS) { u->hold_on = 0; return MSR_GO; }
		return ch ? MSR_REDRAW : MSR_NONE;
	}
}

/* ---- texts -------------------------------------------------------------------------------------------------------------------------------------------------------- */
#define MSR_TXT_WARN  "Your memory cards and settings will be removed. Your Melee save is on the card: this cannot be undone."
#define MSR_TXT_KEPT  "A copy goes to SD:/multishine/reset_backup until the next reset. The app, its updates and your games stay."
#define MSR_TXT_DONE  "Done. Starting over."
/* the red line of a failure: the file name and the code; an abort says nothing was changed */
static inline void msr_fail_text(char *o, unsigned n, const struct msr_result *r)
{
	if (r->aborted) snprintf(o, n, "Could not prepare %s (%d). Nothing was changed.", r->first_fail, r->first_rc);
	else snprintf(o, n, "Could not remove %s (%d)%s.", r->first_fail, r->first_rc, r->failed > 1 ? " and more" : "");
}
#endif
