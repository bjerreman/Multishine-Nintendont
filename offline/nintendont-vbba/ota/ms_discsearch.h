/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The general disc search: finds the stock Melee v1.02 image on a device without the player having to put it anywhere in particular.  Pure and
 * host-tested (tests/discsearch_test.c): the file system, the clock, the stop key, the log and the lock check are callbacks, so fake directory trees can be searched on the host.
 *
 * Order (the search stops at the first phase that has an image the lock accepts; inside a phase the candidates are ranked, then checked one by one, and the first accepted one wins):
 *   1 games/ as before: games/NAME/game.iso, game.gcm and games/NAME.iso, NAME.gcm
 *   2 the likely folders at the device root (iso, isos, roms, rom, gc, gamecube, wii, backup, backups, dumps, melee, smash; case-insensitive) with one level of folders below them, and the root itself
 *   3 a breadth-first walk of the whole device to depth 3 that skips multishine, saves, apps, bootmii, private, System Volume Information and names starting with '.'; at most 2000 directory
 *     entries and about 6 s of walking, so a large card cannot stall the screen
 * A candidate is a file ending .iso or .gcm of EXACTLY the stock size (1,459,978,240 bytes: every other size cannot pass the lock, so it is never opened).  Every other image-like file (any size, also
 * .ciso / .nkit.iso / .gcz / .rvz ...) and every big file with a Melee name hint is 'near': never a candidate, but counted, logged with its size and named on the screen (mds_near).  Ranking before the check: the name hints
 * 'smash', 'melee', 'gale01', 'gale' (case-insensitive, in the whole path: folder names count; each hint that matches adds one), then phase / folder preference, then name order.  The check
 * (callback) is the header test followed by the disc lock, cache first: a name alone never decides anything. */
#ifndef MS_DISCSEARCH_H
#define MS_DISCSEARCH_H
#include <stdio.h>
#include <string.h>

#define MDS_STOCK_SIZE   1459978240UL
#define MDS_PATHLEN      160
#define MDS_MAX_CAND     64
#define MDS_MAX_ENTRIES  2000
#define MDS_MAX_MS       6000u
#define MDS_MAX_DEPTH    3
#define MDS_QUEUE        256
#define MDS_NAMELEN      96

struct mds_fs {
	void *ctx;
	int (*opendir)(void *ctx, const char *path);                                                       /* 0 = open (one directory open at a time) */
	int (*readdir)(void *ctx, char *name, unsigned n, int *is_dir, unsigned long *size);              /* 1 = an entry, 0 = the end */
	void (*closedir)(void *ctx);
	unsigned (*now)(void *ctx);                                                                        /* milliseconds */
	int (*stop)(void *ctx);                                                                            /* 1 = the player pressed B */
	void (*log)(void *ctx, const char *line);
	int (*check)(void *ctx, const char *path, int cache_only);                                         /* the header test + the disc lock: 1 accepted, 0 refused, -1 (cache_only) no cached verdict */
};

/* near = image-like files that cannot be the stock disc (another size, or a compressed / other format: .ciso, .nkit.iso, .rvz ...) and files whose name hints Melee (any extension): counted once each,
   logged with their size, and the best one (most name hints, then the first) is kept so the screen can name it ("Found X, not a stock v1.02 image") instead of saying nothing was found */
#define MDS_MAX_NEAR 8
#define MDS_NEAR_MIN 100000000UL   /* a file with a Melee name hint but no image extension counts from this size (a disc image in some other container), not a note or a save */
struct mds_stats { unsigned places, entries, cands, checked, accepted; int stopped, capped; unsigned near; char near_path[MDS_PATHLEN]; unsigned long near_size; int near_score;
                   unsigned opens_failed; char seen_near[MDS_MAX_NEAR][MDS_PATHLEN]; };

struct mds_cand { char path[MDS_PATHLEN]; int score, phase, done; };

struct mds_state {
	const struct mds_fs *fs;
	struct mds_stats *st;
	struct mds_cand c[MDS_MAX_CAND];
	unsigned n;
	unsigned t0, budget_ms;
	unsigned dirs, capents;                                                                            /* entries since the last stop poll; entries of phases 2 and 3 (the cap) */
};

static inline int mds_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int mds_ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) if (mds_lc((unsigned char)*a) != mds_lc((unsigned char)*b)) return 0; return *a == *b; }
static inline int mds_icmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = mds_lc((unsigned char)*a), y = mds_lc((unsigned char)*b);
		if (x != y) return x < y ? -1 : 1;
		if (!x) return 0;
	}
}
static inline int mds_has_ext(const char *n, const char *ext)
{
	size_t l = strlen(n), e = strlen(ext), i;
	if (l < e) return 0;
	for (i = 0; i < e; i++) if (mds_lc((unsigned char)n[l - e + i]) != ext[i]) return 0;
	return 1;
}
static inline int mds_is_image_name(const char *n) { return mds_has_ext(n, ".iso") || mds_has_ext(n, ".gcm"); }
/* any disc-image-looking name, also the formats the lock can never accept (compressed, NKit, Wii containers) */
static inline int mds_is_imagelike_name(const char *n)
{
	static const char *const e[] = { ".iso", ".gcm", ".ciso", ".cso", ".gcz", ".rvz", ".wia", ".wbfs", ".nkit.gcz", ".tgc", ".dol.iso" };
	unsigned i;
	for (i = 0; i < sizeof e / sizeof e[0]; i++) if (mds_has_ext(n, e[i])) return 1;
	return 0;
}
static inline int mds_is_game_name(const char *n) { return mds_ieq(n, "game.iso") || mds_ieq(n, "game.gcm"); }
static inline int mds_skip_dir(const char *n)
{
	static const char *const s[] = { "multishine", "saves", "apps", "bootmii", "private", "system volume information" };
	unsigned i;
	if (n[0] == '.') return 1;                                                                         /* '.', '..', '._x', hidden */
	for (i = 0; i < sizeof s / sizeof s[0]; i++) if (mds_ieq(n, s[i])) return 1;
	return 0;
}
static inline int mds_likely_dir(const char *n)
{
	static const char *const s[] = { "iso", "isos", "roms", "rom", "gc", "gamecube", "wii", "backup", "backups", "dumps", "melee", "smash" };
	unsigned i;
	for (i = 0; i < sizeof s / sizeof s[0]; i++) if (mds_ieq(n, s[i])) return 1;
	return 0;
}
static inline int mds_has_sub(const char *hay, const char *needle)                                      /* case-insensitive substring */
{
	size_t n = strlen(needle), i, j;
	for (i = 0; hay[i]; i++) {
		for (j = 0; j < n && hay[i + j] && mds_lc((unsigned char)hay[i + j]) == needle[j]; j++) ;
		if (j == n) return 1;
	}
	return 0;
}
/* how many of the name hints the whole path carries (0..4) */
static inline int mds_score(const char *path)
{
	static const char *const h[] = { "smash", "melee", "gale01", "gale" };
	int s = 0;
	unsigned i;
	for (i = 0; i < sizeof h / sizeof h[0]; i++) s += mds_has_sub(path, h[i]);
	return s;
}
static inline void mds_join(char *out, unsigned n, const char *dir, const char *name)
{
	size_t l = strlen(dir);
	snprintf(out, n, "%s%s%s", dir, (l && dir[l - 1] == '/') ? "" : "/", name);
}

static inline void mds_log(struct mds_state *s, const char *fmt, const char *a, unsigned long b, unsigned long c)
{
	char t[240];
	if (!s->fs->log) return;
	snprintf(t, sizeof t, fmt, a, b, c);
	s->fs->log(s->fs->ctx, t);
}

static inline void mds_add(struct mds_state *s, const char *path, int phase)
{
	unsigned i;
	if (strlen(path) >= MDS_PATHLEN || s->n >= MDS_MAX_CAND) return;
	for (i = 0; i < s->n; i++) if (mds_ieq(s->c[i].path, path)) return;                                 /* found by an earlier phase already (and checked there) */
	strcpy(s->c[s->n].path, path);
	s->c[s->n].score = mds_score(path);
	s->c[s->n].phase = phase;
	s->c[s->n].done = 0;
	s->n++;
	s->st->cands++;
}

/* a file that looks like a disc image or carries a Melee name hint but cannot be a candidate: remember it once (the walk sees a folder in more than one phase) */
static inline void mds_near(struct mds_state *s, const char *path, unsigned long size)
{
	struct mds_stats *st = s->st;
	unsigned i, k = st->near < MDS_MAX_NEAR ? st->near : MDS_MAX_NEAR;
	int sc;
	char t[MDS_PATHLEN + 120];
	if (strlen(path) >= MDS_PATHLEN) return;
	for (i = 0; i < k; i++) if (mds_ieq(st->seen_near[i], path)) return;
	if (st->near < MDS_MAX_NEAR) strcpy(st->seen_near[st->near], path);
	st->near++;
	sc = mds_score(path);
	if (!st->near_path[0] || sc > st->near_score) { strcpy(st->near_path, path); st->near_size = size; st->near_score = sc; }
	snprintf(t, sizeof t, "disc search: seen %s, %lu bytes: not a stock v1.02 image (needs %lu bytes, .iso or .gcm)", path, size, (unsigned long)MDS_STOCK_SIZE);
	if (s->fs->log) s->fs->log(s->fs->ctx, t);
}

static inline int mds_poll_stop(struct mds_state *s)
{
	if (s->st->stopped) return 1;
	if (s->fs->stop && s->fs->stop(s->fs->ctx)) s->st->stopped = 1;
	return s->st->stopped;
}

/* a device root the file system refuses as "sd:/": tried once more as "sd:" (the walk must reach the root; a console showed only 2 places on a card with several folders) -> 1 = open */
static inline int mds_open_root_bare(struct mds_state *s, const char *dir)
{
	char r[16];
	size_t l = strlen(dir);
	if (l < 2 || l >= sizeof r || dir[l - 1] != '/' || dir[l - 2] != ':') return 0;
	memcpy(r, dir, l - 1); r[l - 1] = 0;
	if (s->fs->opendir(s->fs->ctx, r) != 0) return 0;
	if (s->fs->log) { char t[64]; snprintf(t, sizeof t, "disc search: %s opened as %s", dir, r); s->fs->log(s->fs->ctx, t); }
	return 1;
}

/* list one directory: image files of the stock size become candidates (only_game: only game.iso / game.gcm), sub-directories (not the skipped ones) are returned in sub[] (nsub in/out, max cap).
   0 = listed (or could not be opened), -1 = stop / cap reached (the walk ends) */
static inline int mds_scan(struct mds_state *s, const char *dir, int phase, int only_game, char (*sub)[MDS_NAMELEN], unsigned *nsub, unsigned cap)
{
	char name[MDS_NAMELEN], p[MDS_PATHLEN];
	int isdir;
	unsigned long size;
	if (s->fs->opendir(s->fs->ctx, dir) != 0 && !mds_open_root_bare(s, dir)) {
		char t[MDS_PATHLEN + 40];
		s->st->opens_failed++;
		snprintf(t, sizeof t, "disc search: cannot open %s (phase %d)", dir, phase);
		if (s->fs->log) s->fs->log(s->fs->ctx, t);
		return 0;
	}
	s->st->places++;
	while (s->fs->readdir(s->fs->ctx, name, sizeof name, &isdir, &size)) {
		s->st->entries++;
		if (phase == 3 && s->budget_ms && (unsigned)(s->fs->now(s->fs->ctx) - s->t0) > s->budget_ms) { s->st->capped = 1; break; }
		if (phase >= 2 && ++s->capents > MDS_MAX_ENTRIES) { s->st->capped = 1; break; }
		if ((++s->dirs & 15u) == 0 && mds_poll_stop(s)) break;
		if (isdir) {
			if (mds_skip_dir(name)) continue;
			if (sub && *nsub < cap) { strcpy(sub[*nsub], name); (*nsub)++; }
		} else if (mds_is_image_name(name) && size == MDS_STOCK_SIZE && (!only_game || mds_is_game_name(name))) {
			mds_join(p, sizeof p, dir, name);
			mds_add(s, p, phase);
		} else if (!(mds_is_image_name(name) && size == MDS_STOCK_SIZE) && (mds_is_imagelike_name(name) || (mds_score(name) > 0 && size >= MDS_NEAR_MIN))) {   /* the size filter never drops a likely file silently */
			mds_join(p, sizeof p, dir, name);
			mds_near(s, p, size);
		}
	}
	s->fs->closedir(s->fs->ctx);
	return (s->st->stopped || s->st->capped) ? -1 : 0;
}

/* check the not yet checked candidates of `phase` in rank order (score high first, then name order); the first the lock accepts is appended to out (up to max).  all = keep checking the rest of the phase
   (the card build wants alternatives), else stop at the first.  Returns the number accepted in this call. */
static inline unsigned mds_check_phase(struct mds_state *s, int phase, int all, char (*out)[MDS_PATHLEN], unsigned max, unsigned have)
{
	unsigned order[MDS_MAX_CAND], m = 0, i, j, got = 0;
	char t[MDS_PATHLEN + 40];
	for (i = 0; i < s->n; i++) if (s->c[i].phase == phase && !s->c[i].done) order[m++] = i;
	for (i = 1; i < m; i++) {                                                                           /* insertion sort: score desc, then name */
		unsigned k = order[i];
		for (j = i; j > 0; j--) {
			const struct mds_cand *a = &s->c[order[j - 1]], *b = &s->c[k];
			if (a->score > b->score || (a->score == b->score && mds_icmp(a->path, b->path) <= 0)) break;
			order[j] = order[j - 1];
		}
		order[j] = k;
	}
	if (m) {
		char l[MDS_MAX_CAND * 12 + 40];
		size_t w = 0;
		w += (size_t)snprintf(l + w, sizeof l - w, "disc search: phase %d ranking:", phase);
		for (i = 0; i < m && w + 14 < sizeof l; i++) w += (size_t)snprintf(l + w, sizeof l - w, " %u(score %d)", order[i], s->c[order[i]].score);
		if (s->fs->log) s->fs->log(s->fs->ctx, l);
	}
	for (i = 0; i < m && have + got < max; i++) {
		struct mds_cand *c = &s->c[order[i]];
		unsigned t0;
		int r;
		if (mds_poll_stop(s)) break;
		t0 = s->fs->now(s->fs->ctx);
		r = s->fs->check(s->fs->ctx, c->path, 1);
		if (r < 0) r = s->fs->check(s->fs->ctx, c->path, 0);
		c->done = 1;
		s->st->checked++;
		snprintf(t, sizeof t, "disc search: %s -> %s (%u ms)", c->path, r > 0 ? "accepted" : "refused", (unsigned)(s->fs->now(s->fs->ctx) - t0));
		if (s->fs->log) s->fs->log(s->fs->ctx, t);
		if (r > 0) { strcpy(out[have + got], c->path); got++; s->st->accepted++; if (!all) break; }
	}
	return got;
}

/* the whole search of one device root (root = "sd:/" or "usb:/", games = its games folder); returns how many stock images were accepted (written to out, at most max) */
static inline unsigned mds_search(const struct mds_fs *fs, const char *root, const char *games, int all_in_phase, char (*out)[MDS_PATHLEN], unsigned max, struct mds_stats *st)
{
	static char sub[MDS_QUEUE][MDS_NAMELEN], sub2[MDS_QUEUE][MDS_NAMELEN];
	static char q[MDS_QUEUE][MDS_PATHLEN];
	static unsigned char qd[MDS_QUEUE];
	static struct mds_state s;
	unsigned i, nsub = 0, got = 0, qh = 0, qt = 0;
	char p[MDS_PATHLEN];
	memset(st, 0, sizeof *st);
	memset(&s, 0, sizeof s);
	s.fs = fs; s.st = st;
	/* phase 1: games/ */
	if (mds_scan(&s, games, 1, 0, sub, &nsub, MDS_QUEUE) == 0)
		for (i = 0; i < nsub && !st->stopped; i++) { mds_join(p, sizeof p, games, sub[i]); mds_scan(&s, p, 1, 1, 0, 0, 0); }
	got += mds_check_phase(&s, 1, all_in_phase, out, max, got);
	if (got || st->stopped) goto done;
	/* phase 2: the likely folders at the root, one level of folders below them, and the root itself */
	nsub = 0;
	if (mds_scan(&s, root, 2, 0, sub, &nsub, MDS_QUEUE) == 0)
		for (i = 0; i < nsub && !st->stopped && !st->capped; i++) {
			unsigned n2 = 0, j;
			if (!mds_likely_dir(sub[i])) continue;
			mds_join(p, sizeof p, root, sub[i]);
			if (mds_scan(&s, p, 2, 0, sub2, &n2, MDS_QUEUE) < 0) break;
			for (j = 0; j < n2 && !st->stopped && !st->capped; j++) { char p2[MDS_PATHLEN]; mds_join(p2, sizeof p2, p, sub2[j]); if (mds_scan(&s, p2, 2, 0, 0, 0, 0) < 0) break; }
		}
	got += mds_check_phase(&s, 2, all_in_phase, out, max, got);
	if (got || st->stopped) goto done;
	/* phase 3: breadth-first walk to depth 3 */
	s.t0 = fs->now(fs->ctx); s.budget_ms = MDS_MAX_MS;
	strncpy(q[0], root, MDS_PATHLEN - 1); q[0][MDS_PATHLEN - 1] = 0; qd[0] = 0; qt = 1;
	while (qh < qt && !st->stopped && !st->capped) {
		const unsigned d = qd[qh];
		char dir[MDS_PATHLEN];
		unsigned n3 = 0, j;
		strcpy(dir, q[qh]); qh++;
		if (mds_scan(&s, dir, 3, 0, sub, &n3, MDS_QUEUE) < 0) break;
		if (d >= MDS_MAX_DEPTH) continue;
		for (j = 0; j < n3 && qt < MDS_QUEUE; j++) {
			mds_join(q[qt], MDS_PATHLEN, dir, sub[j]);
			if (strlen(q[qt]) >= MDS_PATHLEN - 1) continue;
			qd[qt] = (unsigned char)(d + 1); qt++;
		}
	}
	got += mds_check_phase(&s, 3, all_in_phase, out, max, got);
done:
	{
		char t[200];
		snprintf(t, sizeof t, "disc search: %s: %u places, %u entries, %u candidates, %u checked, %u accepted, %u other images%s%s%s", root, st->places, st->entries, st->cands, st->checked, st->accepted, st->near, st->opens_failed ? ", some folders could not be opened" : "", st->capped ? ", cap reached" : "", st->stopped ? ", stopped by the player" : "");
		if (fs->log) fs->log(fs->ctx, t);
	}
	return got;
}
#endif
