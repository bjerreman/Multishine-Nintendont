/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The disc image file browser of the Multishine start screen: what the player gets when no stock Melee image was found.  A simple browser over the
 * device (SD, USB when present): folders and .iso / .gcm files, folders first, case-insensitive name order, names starting with '.' hidden (that covers '._' files of a Mac), file sizes in MB.
 * Pure and host-tested (tests/browse_test.c): the directory reader is the same callback table as the disc search (struct mds_fs: opendir / readdir / closedir), the loader draws and asks the
 * disc lock; everything that decides (sorting, filtering, paging, the selection, the path, the keys, the texts) lives here.
 *
 * Keys: Up / Down move (wrapping at the ends, as the loader's game list), Left / Right page (clamped), A opens a folder or picks a file, B goes up one folder (at the device root: leave the browser),
 * Y switches SD / USB when both are there.  A picked file is checked by the loader (header test, then the disc lock): msb_key returns MSB_R_PICK and the path in msb_pick_path; the loader then calls
 * msb_set_verdict: a stock image ends the browser (MSB_R_LOCKED), a refused one stays with one red line saying why and a red X at the file's row. */
#ifndef MS_BROWSE_H
#define MS_BROWSE_H
#include "ms_discsearch.h"

#define MSB_MAX      512      /* entries kept of one folder; a larger folder shows its first 512 in name order and says so */
#define MSB_ROWS     14       /* rows of the list on the screen (text rows of 20 px from y 90) */
#define MSB_NAME     MDS_NAMELEN

struct msb_ent { char name[MSB_NAME]; unsigned long size; int is_dir; };

struct msb_state {
	int usb;                  /* 0 = SD, 1 = USB */
	int has_usb;              /* both devices are there: Y switches */
	char path[MDS_PATHLEN];   /* "sd:/" or "sd:/games/isos" (no trailing slash except at the root) */
	unsigned n, sel, top;     /* entries, selected index, first visible index */
	int capped;               /* the folder had more than MSB_MAX entries */
	int open_failed;          /* the folder could not be listed */
	int mark_idx;             /* index of the entry that carries the check / X mark, -1 = none */
	int mark_kind;            /* MSB_MARK_GOOD / MSB_MARK_BAD */
	char msg[120];            /* the one line under the list; msg_bad = red, else muted */
	int msg_bad;
	char pick[MDS_PATHLEN];   /* the file the last A picked */
	struct msb_ent e[MSB_MAX];
};

#define MSB_MARK_GOOD 1
#define MSB_MARK_BAD  2

/* keys and what a key did */
#define MSB_K_UP    1
#define MSB_K_DOWN  2
#define MSB_K_LEFT  3
#define MSB_K_RIGHT 4
#define MSB_K_A     5
#define MSB_K_B     6
#define MSB_K_Y     7

#define MSB_R_NONE   0        /* handled inside (moved, opened a folder, switched device): redraw */
#define MSB_R_PICK   1        /* a file was picked: msb_pick_path, the loader checks it */
#define MSB_R_LEAVE  2        /* B at the device root: back to the start screen */
#define MSB_R_LOCKED 3        /* (msb_set_verdict) a stock image: the browser ends with it */

/* ---- the filter and the order -------------------------------------------------------------------------------------------------------------------------------------------- */
static inline int msb_visible(const char *name, int is_dir)
{
	if (!name[0] || name[0] == '.') return 0;                  /* hidden, '.', '..', '._x' */
	return is_dir ? 1 : mds_is_image_name(name);
}
/* a before b: folders first, then case-insensitive name order, then the exact bytes (two names that differ only in case keep a fixed order) */
static inline int msb_before(const struct msb_ent *a, const struct msb_ent *b)
{
	int c;
	if (a->is_dir != b->is_dir) return a->is_dir;
	c = mds_icmp(a->name, b->name);
	if (c) return c < 0;
	return strcmp(a->name, b->name) < 0;
}

/* ---- paths ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static inline void msb_root(struct msb_state *s, int usb) { s->usb = usb ? 1 : 0; snprintf(s->path, sizeof s->path, "%s", usb ? "usb:/" : "sd:/"); }
static inline int msb_is_root(const char *path) { const char *c = strchr(path, ':'); return c && c[1] == '/' && c[2] == 0; }
/* the folder above `path` -> out; the name of the folder left -> leaf (for selecting it again); the root has no parent (returns 0, out = path) */
static inline int msb_parent(const char *path, char *out, unsigned n, char *leaf, unsigned ln)
{
	size_t l = strlen(path), k;
	char tmp[MDS_PATHLEN];
	snprintf(out, n, "%s", path);
	if (leaf && ln) leaf[0] = 0;
	if (msb_is_root(path)) return 0;
	snprintf(tmp, sizeof tmp, "%s", path);
	while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
	for (k = l; k > 0 && tmp[k - 1] != '/'; k--) ;
	if (leaf && ln) snprintf(leaf, ln, "%s", tmp + k);
	if (k == 0 || tmp[k - 1] != '/') { snprintf(out, n, "%s", path); return 0; }
	tmp[k] = 0;                                                  /* keeps the slash: "sd:/" or "sd:/games/" */
	if (!msb_is_root(tmp)) tmp[k - 1] = 0;                       /* "sd:/games" */
	snprintf(out, n, "%s", tmp);
	return 1;
}
/* path + name -> out; 0 when it would not fit */
static inline int msb_join(char *out, unsigned n, const char *dir, const char *name)
{
	size_t l = strlen(dir), m = strlen(name);
	if (l + m + 2 > n || l + m + 2 > MDS_PATHLEN) return 0;
	mds_join(out, n, dir, name);
	return 1;
}

/* ---- loading a folder ----------------------------------------------------------------------------------------------------------------------------------------------- */
/* read the folder s->path through fs (opendir / readdir / closedir only): visible entries, kept sorted, at most MSB_MAX (the first MSB_MAX in order).  The selection goes to the first row. */
static inline void msb_load(struct msb_state *s, const struct mds_fs *fs)
{
	char name[MSB_NAME];
	int isdir;
	unsigned long size;
	s->n = 0; s->sel = 0; s->top = 0; s->capped = 0; s->open_failed = 0; s->mark_idx = -1; s->mark_kind = 0; s->msg[0] = 0; s->msg_bad = 0;
	if (fs->opendir(fs->ctx, s->path) != 0) { s->open_failed = 1; snprintf(s->msg, sizeof s->msg, "This folder could not be opened."); s->msg_bad = 1; return; }
	while (fs->readdir(fs->ctx, name, sizeof name, &isdir, &size)) {
		struct msb_ent t;
		unsigned i;
		if (!msb_visible(name, isdir)) continue;
		snprintf(t.name, sizeof t.name, "%s", name); t.size = isdir ? 0 : size; t.is_dir = isdir ? 1 : 0;
		for (i = s->n; i > 0 && msb_before(&t, &s->e[i - 1]); i--) ;      /* insertion point: after every entry that is not after t */
		if (i >= MSB_MAX) { s->capped = 1; continue; }                     /* t sorts behind all the kept ones and the list is full */
		if (s->n >= MSB_MAX) { s->capped = 1; s->n = MSB_MAX - 1; }       /* full: the last one drops out */
		memmove(&s->e[i + 1], &s->e[i], (s->n - i) * sizeof s->e[0]);
		s->e[i] = t; s->n++;
	}
	fs->closedir(fs->ctx);
	if (s->capped) snprintf(s->msg, sizeof s->msg, "A large folder: the first %u entries are shown.", (unsigned)MSB_MAX);
}

static inline void msb_open(struct msb_state *s, const struct mds_fs *fs, int usb, int has_usb)
{
	s->has_usb = has_usb ? 1 : 0;
	msb_root(s, usb);
	msb_load(s, fs);
}

/* ---- the selection and the window ----------------------------------------------------------------------------------------------------------------------------------- */
static inline void msb_fix_window(struct msb_state *s)
{
	if (s->n == 0) { s->sel = 0; s->top = 0; return; }
	if (s->sel >= s->n) s->sel = s->n - 1;
	if (s->sel < s->top) s->top = s->sel;
	if (s->sel >= s->top + MSB_ROWS) s->top = s->sel - MSB_ROWS + 1;
	if (s->n <= MSB_ROWS) s->top = 0;
	else if (s->top + MSB_ROWS > s->n) s->top = s->n - MSB_ROWS;
}
static inline void msb_clear_note(struct msb_state *s) { s->mark_idx = -1; s->mark_kind = 0; if (!s->capped && !s->open_failed) { s->msg[0] = 0; s->msg_bad = 0; } }
static inline void msb_move(struct msb_state *s, int dir)                 /* Up / Down: one row, wrapping at the ends */
{
	if (s->n == 0) return;
	if (dir < 0) s->sel = s->sel == 0 ? s->n - 1 : s->sel - 1;
	else s->sel = s->sel + 1 >= s->n ? 0 : s->sel + 1;
	msb_clear_note(s);
	msb_fix_window(s);
}
static inline void msb_page(struct msb_state *s, int dir)                 /* Left / Right: a page, clamped at the first / last row */
{
	if (s->n == 0) return;
	if (dir < 0) s->sel = s->sel > MSB_ROWS ? s->sel - MSB_ROWS : 0;
	else s->sel = s->sel + MSB_ROWS >= s->n ? s->n - 1 : s->sel + MSB_ROWS;
	msb_clear_note(s);
	msb_fix_window(s);
}
static inline void msb_select_name(struct msb_state *s, const char *name)
{
	unsigned i;
	for (i = 0; i < s->n; i++) if (s->e[i].is_dir && mds_ieq(s->e[i].name, name)) { s->sel = i; break; }
	msb_fix_window(s);
}

/* ---- keys ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static inline int msb_key(struct msb_state *s, int key, const struct mds_fs *fs)
{
	switch (key) {
	case MSB_K_UP: msb_move(s, -1); break;
	case MSB_K_DOWN: msb_move(s, 1); break;
	case MSB_K_LEFT: msb_page(s, -1); break;
	case MSB_K_RIGHT: msb_page(s, 1); break;
	case MSB_K_Y:
		if (s->has_usb) { msb_root(s, !s->usb); msb_load(s, fs); }
		break;
	case MSB_K_B:
		if (msb_is_root(s->path)) return MSB_R_LEAVE;
		{
			char up[MDS_PATHLEN], leaf[MSB_NAME];
			msb_parent(s->path, up, sizeof up, leaf, sizeof leaf);
			snprintf(s->path, sizeof s->path, "%s", up);
			msb_load(s, fs);
			msb_select_name(s, leaf);
		}
		break;
	case MSB_K_A:
		if (s->n == 0) break;
		if (s->e[s->sel].is_dir) {
			char p[MDS_PATHLEN];
			if (!msb_join(p, sizeof p, s->path, s->e[s->sel].name)) { snprintf(s->msg, sizeof s->msg, "This folder is too deep to open."); s->msg_bad = 1; break; }
			snprintf(s->path, sizeof s->path, "%s", p);
			msb_load(s, fs);
		} else {
			if (!msb_join(s->pick, sizeof s->pick, s->path, s->e[s->sel].name)) { snprintf(s->msg, sizeof s->msg, "This file name is too long."); s->msg_bad = 1; break; }
			return MSB_R_PICK;
		}
		break;
	default: break;
	}
	return MSB_R_NONE;
}

/* the loader's verdict on the picked file: accepted (a stock image) -> MSB_R_LOCKED; refused -> the red X at the row and `why` (the plain reason, see msb_why_*) in one red line */
static inline int msb_set_verdict(struct msb_state *s, int accepted, const char *why)
{
	s->mark_idx = (int)s->sel;
	if (accepted) { s->mark_kind = MSB_MARK_GOOD; s->msg[0] = 0; s->msg_bad = 0; return MSB_R_LOCKED; }
	s->mark_kind = MSB_MARK_BAD;
	snprintf(s->msg, sizeof s->msg, "Not stock Melee v1.02: %s", why && why[0] ? why : "this image is not accepted");
	s->msg_bad = 1;
	return MSB_R_NONE;
}
/* the player pressed B while the file was being checked: nothing is decided */
static inline void msb_check_stopped(struct msb_state *s) { s->mark_idx = -1; s->mark_kind = 0; snprintf(s->msg, sizeof s->msg, "Check stopped."); s->msg_bad = 0; }

/* ---- texts ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static inline void msb_size_text(char *o, unsigned n, unsigned long size)
{
	if (size < 1048576UL) snprintf(o, n, "<1 MB");
	else snprintf(o, n, "%lu MB", (size + 524288UL) / 1048576UL);
}
/* the refusal reasons that come before the lock (the lock's own come from msdl_detail through ms_disc_why) */
static inline void msb_why_size(char *o, unsigned n, unsigned long size)
{
	char t[24];
	msb_size_text(t, sizeof t, size);
	snprintf(o, n, "the size is %s, a Melee disc image is %lu MB", t, (MDS_STOCK_SIZE + 524288UL) / 1048576UL);
}
static inline void msb_why_header(char *o, unsigned n, const char *id6)
{
	if (!strncmp(id6, "GALE01", 6)) snprintf(o, n, "another Melee revision (not v1.02)");
	else if (!strncmp(id6, "GAL", 3)) snprintf(o, n, "another Melee version (%.6s)", id6);
	else snprintf(o, n, "not Melee NTSC-U (%.6s)", id6[0] ? id6 : "unknown");
}
/* the path line above the list: the path, shortened from the left with ".." when it is wider than `maxc` characters */
static inline void msb_path_text(char *o, unsigned n, const char *path, unsigned maxc)
{
	size_t l = strlen(path);
	if (maxc >= 4 && l > maxc) snprintf(o, n, "..%s", path + l - (maxc - 2));
	else snprintf(o, n, "%s", path);
}
/* the row text of an entry: folders end with '/', files carry their size in MB (the loader right-aligns the size column) */
static inline void msb_row_name(char *o, unsigned n, const struct msb_ent *e) { if (e->is_dir) snprintf(o, n, "%.60s/", e->name); else snprintf(o, n, "%.60s", e->name); }

/* the centred line in an empty list (NULL when there are rows) */
static inline const char *msb_empty_line(const struct msb_state *s) { return s->n ? 0 : (s->open_failed ? "Nothing to show." : "No folders or disc images here."); }

/* the footer: B goes up (Back at the root), A opens / picks, Left/Right page, Y switches the device when both are there */
struct msb_keys { int n; const char *key[5]; const char *act[5]; };
static inline void msb_footer(const struct msb_state *s, struct msb_keys *k)
{
	int i = 0;
	k->key[i] = "B"; k->act[i] = msb_is_root(s->path) ? "Back" : "Up"; i++;
	k->key[i] = "A"; k->act[i] = (s->n && !s->e[s->sel].is_dir) ? "Select" : "Open"; i++;
	k->key[i] = "Left/Right"; k->act[i] = "Page"; i++;
	if (s->has_usb) { k->key[i] = "Y"; k->act[i] = s->usb ? "SD" : "USB"; i++; }
	k->n = i;
}
#endif
