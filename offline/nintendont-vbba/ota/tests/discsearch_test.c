/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_discsearch.h on fake directory trees.  Prints OK or the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_discsearch.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define ST MDS_STOCK_SIZE

struct node { char path[MDS_PATHLEN]; int dir; unsigned long size; int lock; int cached; int checked; };
static struct node T[6000];
static unsigned NT;
static char cur[MDS_PATHLEN];
static unsigned cur_i, clock_ms, ms_per_entry, stop_after_polls, polls, full_checks, root_slash_fails;

static void reset(void) { root_slash_fails = 0; NT = 0; clock_ms = 0; ms_per_entry = 0; stop_after_polls = 0; polls = 0; full_checks = 0; }
static void add(const char *p, int dir, unsigned long size, int lock, int cached) { snprintf(T[NT].path, MDS_PATHLEN, "%s", p); T[NT].dir = dir; T[NT].size = size; T[NT].lock = lock; T[NT].cached = cached; T[NT].checked = 0; NT++; }
static void D(const char *p) { add(p, 1, 0, 0, 0); }
static void F(const char *p, unsigned long size, int lock) { add(p, 0, size, lock, 0); }

static void norm(char *o, const char *p) { size_t l; strcpy(o, p); l = strlen(o); while (l > 1 && o[l - 1] == '/') o[--l] = 0; }
static void parent_of(char *o, const char *p) { char *s; strcpy(o, p); s = strrchr(o, '/'); if (s) *s = 0; }

static int f_open(void *c, const char *path)
{
	unsigned i;
	char n[MDS_PATHLEN];
	(void)c;
	if (root_slash_fails && !strcmp(path, "sd:/")) return -1;                  /* a file system that wants the bare "sd:" for its root */
	norm(n, path);
	if (strcmp(n, "sd:") != 0) { int ok = 0; for (i = 0; i < NT; i++) if (T[i].dir && mds_ieq(T[i].path, n)) ok = 1; if (!ok) return -1; }
	strcpy(cur, n); cur_i = 0;
	return 0;
}
static int f_read(void *c, char *name, unsigned n, int *isdir, unsigned long *size)
{
	(void)c;
	clock_ms += ms_per_entry;
	for (; cur_i < NT; cur_i++) {
		char par[MDS_PATHLEN];
		parent_of(par, T[cur_i].path);
		if (mds_ieq(par, cur)) {
			snprintf(name, n, "%s", strrchr(T[cur_i].path, '/') + 1);
			*isdir = T[cur_i].dir; *size = T[cur_i].size;
			cur_i++;
			return 1;
		}
	}
	return 0;
}
static void f_close(void *c) { (void)c; }
static unsigned f_now(void *c) { (void)c; return clock_ms; }
static int f_stop(void *c) { (void)c; polls++; return stop_after_polls && polls >= stop_after_polls; }
static char LOG[32768];
static void f_log(void *c, const char *l) { (void)c; if (strlen(LOG) + strlen(l) + 2 < sizeof LOG) { strcat(LOG, l); strcat(LOG, "\n"); } }
static int f_check(void *c, const char *path, int cache_only)
{
	unsigned i;
	(void)c;
	for (i = 0; i < NT; i++) if (!T[i].dir && !strcmp(T[i].path, path)) {
		if (T[i].size != ST) return -99;                                    /* a file of another size must never reach the check */
		if (cache_only) return T[i].cached ? T[i].lock : -1;
		T[i].checked++; full_checks++;
		return T[i].lock;
	}
	return -99;
}
static const struct mds_fs FS = { 0, f_open, f_read, f_close, f_now, f_stop, f_log, f_check };

static char out[8][MDS_PATHLEN];
static struct mds_stats stt;
static unsigned run(int all, unsigned max) { LOG[0] = 0; polls = 0; return mds_search(&FS, "sd:/", "sd:/games", all, out, max, &stt); }
static int checked_of(const char *p) { unsigned i; for (i = 0; i < NT; i++) if (!strcmp(T[i].path, p)) return T[i].checked; return -1; }

int main(void)
{
	unsigned n, i;
	/* helpers of the pure part */
	CHECK(mds_score("sd:/Smash/GALE01.iso") == 3 && mds_score("sd:/x/Super Smash Bros. Melee.iso") == 2 && mds_score("sd:/x/y.iso") == 0 && mds_score("sd:/gale_x.iso") == 1);
	CHECK(mds_skip_dir("Multishine") && mds_skip_dir("System Volume Information") && mds_skip_dir("._x") && mds_skip_dir(".Trashes") && !mds_skip_dir("games"));
	CHECK(mds_likely_dir("ISOS") && mds_likely_dir("Melee") && !mds_likely_dir("stuff"));
	CHECK(mds_is_image_name("A.ISO") && mds_is_image_name("a.gcm") && !mds_is_image_name("a.ciso"));

	/* 1: a stock image named oddly, three folders deep: found by the walk */
	reset(); D("sd:/stuff"); D("sd:/stuff/a"); D("sd:/stuff/a/b"); F("sd:/stuff/a/b/q7.iso", ST, 1); F("sd:/stuff/readme.txt", 10, 0);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/stuff/a/b/q7.iso") && stt.places >= 4 && !stt.capped);
	CHECK(strstr(LOG, "accepted") && strstr(LOG, "places"));
	/* 2: the depth limit: depth 3 is listed, depth 4 is not */
	reset(); D("sd:/a"); D("sd:/a/b"); D("sd:/a/b/c"); D("sd:/a/b/c/d"); F("sd:/a/b/c/d/x.iso", ST, 1);
	n = run(0, 1); CHECK(n == 0);
	F("sd:/a/b/c/y.iso", ST, 1); n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/a/b/c/y.iso"));
	/* 3: a modified same-size image with the better name is checked first and refused; the stock one wins; never a name-only hit */
	reset(); D("sd:/x"); F("sd:/x/Super Smash Bros Melee GALE01.iso", ST, 0); F("sd:/x/zzz.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/x/zzz.iso") && checked_of("sd:/x/Super Smash Bros Melee GALE01.iso") == 1 && stt.checked == 2);
	/* 4: a decoy of the wrong size is never opened or checked (f_check would return -99) */
	reset(); D("sd:/x"); F("sd:/x/Super Smash Bros. Melee.iso", 1234567, 1); F("sd:/x/other.gcm", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/x/other.gcm") && stt.cands == 1);
	reset(); D("sd:/x"); F("sd:/x/Super Smash Bros. Melee.iso", 1234567, 1);
	n = run(0, 1); CHECK(n == 0 && stt.cands == 0 && full_checks == 0);
	/* 5: phase order: games/ before everything else, however good the other name is; the walk is not even started */
	reset(); D("sd:/games"); D("sd:/games/zz"); F("sd:/games/zz/game.iso", ST, 1); F("sd:/melee.iso", ST, 1); D("sd:/deep"); F("sd:/deep/smash melee gale01.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/games/zz/game.iso") && full_checks == 1);
	/* games/ keeps its layouts: only game.iso / game.gcm inside folders, any .iso directly in games/ */
	reset(); D("sd:/games"); D("sd:/games/a"); F("sd:/games/a/other.iso", ST, 1);
	n = run(0, 1); CHECK(n == 0 || out[0][0] == 's');                                              /* found later by the walk, not as a games/ layout */
	CHECK(n == 1 && !strcmp(out[0], "sd:/games/a/other.iso") && stt.places >= 3);
	/* 6: a likely folder at the root (any case) is found in phase 2: a deeper image in another place is never checked */
	reset(); D("sd:/ISOS"); F("sd:/ISOS/m.iso", ST, 1); D("sd:/other"); D("sd:/other/e"); F("sd:/other/e/better smash melee.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/ISOS/m.iso") && full_checks == 1);
	reset(); F("sd:/root.iso", ST, 1);                                                             /* the root itself */
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/root.iso"));
	reset(); D("sd:/Backup"); D("sd:/Backup/dump1"); F("sd:/Backup/dump1/game.iso", ST, 1);        /* one level below a likely folder */
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/Backup/dump1/game.iso"));
	/* 7: skipped folders are never searched */
	reset(); D("sd:/multishine"); F("sd:/multishine/a.iso", ST, 1); D("sd:/saves"); F("sd:/saves/a.iso", ST, 1); D("sd:/.hidden"); F("sd:/.hidden/a.iso", ST, 1); D("sd:/System Volume Information"); F("sd:/System Volume Information/a.iso", ST, 1);
	D("sd:/apps"); F("sd:/apps/a.iso", ST, 1); D("sd:/bootmii"); F("sd:/bootmii/a.iso", ST, 1); D("sd:/private"); F("sd:/private/a.iso", ST, 1); D("sd:/._x"); F("sd:/._x/a.iso", ST, 1);
	n = run(0, 1); CHECK(n == 0 && stt.cands == 0);
	/* 8: the entry cap: 3000 folders at the root, the stock image after them */
	reset(); for (i = 0; i < 3000; i++) { char p[40]; snprintf(p, sizeof p, "sd:/d%04u", i); D(p); }
	F("sd:/zlast.iso", ST, 1);
	n = run(0, 1); CHECK(n == 0 && stt.capped && stt.places <= 2000 + 5 && stt.entries <= 2000 + 20 && strstr(LOG, "cap reached"));
	/* the time cap: 10 ms per entry, 6 s = 600 entries; a stock image deeper in the queue is not reached */
	reset(); ms_per_entry = 10; for (i = 0; i < 700; i++) { char p[40]; snprintf(p, sizeof p, "sd:/d%04u", i); D(p); }
	D("sd:/zz"); F("sd:/zz/z.iso", ST, 1);
	n = run(0, 1); CHECK(n == 0 && stt.capped);
	reset(); ms_per_entry = 1; D("sd:/zz"); F("sd:/zz/z.iso", ST, 1); for (i = 0; i < 700; i++) { char p[40]; snprintf(p, sizeof p, "sd:/d%04u", i); D(p); }   /* fast enough: no cap, found */
	n = run(0, 1); CHECK(n == 1 && !stt.capped);
	/* 9: B stops the search */
	reset(); for (i = 0; i < 300; i++) { char p[40]; snprintf(p, sizeof p, "sd:/e%03u", i); D(p); }
	F("sd:/e000/x.iso", ST, 1);
	stop_after_polls = 0; n = run(0, 1); CHECK(n == 1);
	{ unsigned save = 0; (void)save; }
	reset(); for (i = 0; i < 300; i++) { char p[40]; snprintf(p, sizeof p, "sd:/e%03u", i); D(p); }
	F("sd:/e000/x.iso", ST, 1); stop_after_polls = 3;
	n = run(0, 1); CHECK(n == 0 && stt.stopped && strstr(LOG, "stopped by the player") && full_checks == 0);
	/* 10: ranking inside a phase: hints first (folder names count), then name order; the first LOCK-passing one wins */
	reset(); D("sd:/games"); F("sd:/games/b.iso", ST, 1); F("sd:/games/a.iso", ST, 1); F("sd:/games/melee.iso", ST, 0); F("sd:/games/Smash Melee GALE01.iso", ST, 0);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/games/a.iso") && stt.checked == 3);        /* smash/melee/gale01 (refused), melee (refused), then a.iso */
	reset(); D("sd:/games"); D("sd:/games/Super Smash Melee"); F("sd:/games/Super Smash Melee/game.iso", ST, 1); F("sd:/games/a.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/games/Super Smash Melee/game.iso") && full_checks == 1);
	/* all_in_phase: the whole phase is checked and the alternatives come back in rank order (the card build tries the next one when a build fails) */
	n = run(1, 4); CHECK(n == 2 && !strcmp(out[0], "sd:/games/Super Smash Melee/game.iso") && !strcmp(out[1], "sd:/games/a.iso"));
	n = run(1, 1); CHECK(n == 1);
	/* the cache: a cached accepted lower-ranked image does not jump the queue; the winner is the first lock-passing one in rank order */
	reset(); D("sd:/games"); F("sd:/games/a.iso", ST, 1); F("sd:/games/melee.iso", ST, 1); T[NT - 2].cached = 1;
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/games/melee.iso") && checked_of("sd:/games/a.iso") == 0);
	/* a cached refusal is honoured (no second full check), then the next one passes */
	reset(); D("sd:/games"); F("sd:/games/melee.iso", ST, 0); F("sd:/games/zz.iso", ST, 1); T[NT - 2].cached = 1;
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/games/zz.iso") && checked_of("sd:/games/melee.iso") == 0);
	/* nothing passes: the counts for the message */
	reset(); D("sd:/games"); F("sd:/games/a.iso", ST, 0); F("sd:/games/b.gcm", ST, 0);
	n = run(0, 1); CHECK(n == 0 && stt.cands == 2 && stt.checked == 2 && stt.places >= 1);
	/* our card: games/ holds 'Super Smash Bros. Melee.iso' of another size: never a candidate, but seen, logged with its size and named */
	reset(); D("sd:/apps"); D("sd:/games"); D("sd:/multishine"); D("sd:/private"); F("sd:/games/Super Smash Bros. Melee.iso", ST - 32768UL, 1); F("sd:/boot.dol", 100, 0);
	n = run(0, 1); CHECK(n == 0 && stt.cands == 0 && full_checks == 0 && stt.near == 1 && !strcmp(stt.near_path, "sd:/games/Super Smash Bros. Melee.iso") && stt.near_size == ST - 32768UL);
	CHECK(strstr(LOG, "seen sd:/games/Super Smash Bros. Melee.iso, 1459945472 bytes") && strstr(LOG, "1 other images") && stt.places >= 4);   /* walked broadly: games/, the root twice, games/ again */
	/* compressed and NKit images, and a big file with a Melee name, count as seen; a small note named melee does not */
	reset(); D("sd:/games"); F("sd:/games/melee.ciso", 800000000UL, 1); F("sd:/x.nkit.iso", 700000000UL, 1); F("sd:/Melee backup.zip", 900000000UL, 1); F("sd:/melee notes.txt", 300, 0); F("sd:/games/z.gcz", 600000000UL, 1);
	n = run(0, 1); CHECK(n == 0 && stt.near == 4 && full_checks == 0 && !strstr(LOG, "melee notes.txt"));
	CHECK(strstr(stt.near_path, "elee") != NULL);                                                 /* the best named one is kept for the screen */
	/* a stock image two folders down below a plain folder, and one in the root: both found */
	reset(); D("sd:/games"); D("sd:/stuff"); D("sd:/stuff/a"); F("sd:/stuff/a/Melee.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/stuff/a/Melee.iso") && stt.near == 0);
	reset(); D("sd:/games"); F("sd:/Super Smash Bros. Melee.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && !strcmp(out[0], "sd:/Super Smash Bros. Melee.iso"));
	/* a stock-size image in games/NAME/ that is not called game.iso: not a games/ layout, found by the walk, never counted as 'other' */
	reset(); D("sd:/games"); D("sd:/games/m"); F("sd:/games/m/Melee.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && stt.near == 0);
	/* a root the file system only opens as "sd:": the walk still reaches it and below */
	reset(); root_slash_fails = 1; D("sd:/stuff"); D("sd:/stuff/a"); F("sd:/stuff/a/q.iso", ST, 1);
	n = run(0, 1); CHECK(n == 1 && strstr(LOG, "opened as sd:") && stt.opens_failed == 1);   /* only the missing games/ */
	/* nothing at all: no near file, nothing to name */
	reset(); D("sd:/games"); n = run(0, 1); CHECK(n == 0 && stt.near == 0 && stt.near_path[0] == 0);
	/* a missing games folder and an empty card are fine */
	reset(); n = run(0, 1); CHECK(n == 0 && stt.cands == 0);
	printf("OK\n");
	return 0;
}
