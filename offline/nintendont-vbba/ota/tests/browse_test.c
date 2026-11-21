/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_browse.h (the file browser of the start screen) on fake directory trees.  Prints OK or the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_browse.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define ST MDS_STOCK_SIZE

struct node { char path[MDS_PATHLEN]; int dir; unsigned long size; int stock; };
static struct node T[3000];
static unsigned NT;
static char cur[MDS_PATHLEN];
static unsigned cur_i, opens;
static void reset(void) { NT = 0; opens = 0; }
static void add(const char *p, int dir, unsigned long size, int stock) { snprintf(T[NT].path, MDS_PATHLEN, "%s", p); T[NT].dir = dir; T[NT].size = size; T[NT].stock = stock; NT++; }
static void D(const char *p) { add(p, 1, 0, 0); }
static void F(const char *p, unsigned long size, int stock) { add(p, 0, size, stock); }
static void norm(char *o, const char *p) { size_t l; strcpy(o, p); l = strlen(o); while (l > 1 && o[l - 1] == '/' && o[l - 2] != ':') o[--l] = 0; }
static void parent_of(char *o, const char *p) { char *s; strcpy(o, p); s = strrchr(o, '/'); if (s) { if (s > o && s[-1] == ':') s[1] = 0; else *s = 0; } }

static int f_open(void *c, const char *path)
{
	unsigned i;
	char n[MDS_PATHLEN];
	(void)c;
	opens++;
	norm(n, path);
	if (strcmp(n, "sd:/") != 0 && strcmp(n, "usb:/") != 0) { int ok = 0; for (i = 0; i < NT; i++) if (T[i].dir && !strcmp(T[i].path, n)) ok = 1; if (!ok) return -1; }
	strcpy(cur, n); cur_i = 0;
	return 0;
}
static int f_read(void *c, char *name, unsigned n, int *isdir, unsigned long *size)
{
	(void)c;
	for (; cur_i < NT; cur_i++) {
		char par[MDS_PATHLEN];
		parent_of(par, T[cur_i].path);
		if (!strcmp(par, cur)) {
			snprintf(name, n, "%s", strrchr(T[cur_i].path, '/') + 1);
			*isdir = T[cur_i].dir; *size = T[cur_i].size;
			cur_i++;
			return 1;
		}
	}
	return 0;
}
static void f_close(void *c) { (void)c; }
static const struct mds_fs FS = { 0, f_open, f_read, f_close, 0, 0, 0, 0 };

static int names(const struct msb_state *s, const char *want)       /* the list as "a,b,c" equals want */
{
	char t[2048] = "";
	unsigned i;
	for (i = 0; i < s->n; i++) { if (i) strcat(t, ","); strcat(t, s->e[i].name); }
	if (strcmp(t, want)) printf("  list is [%s], wanted [%s]\n", t, want);
	return !strcmp(t, want);
}

static struct msb_state S;

int main(void)
{
	char a[160], b[160], leaf[96];
	unsigned i;

	/* ---- filter + order: folders first, case-insensitive, hidden names and other file types gone ---- */
	reset();
	D("sd:/games"); D("sd:/Zeta"); D("sd:/alpha"); D("sd:/.Trash"); D("sd:/._games"); D("sd:/Beta");
	F("sd:/melee.iso", ST, 1); F("sd:/Another.GCM", 1000, 0); F("sd:/._melee.iso", 4096, 0); F("sd:/notes.txt", 10, 0); F("sd:/.hidden.iso", ST, 0); F("sd:/zebra.ISO", 5, 0); F("sd:/Apple.iso", 2000000, 0);
	msb_open(&S, &FS, 0, 0);
	CHECK(!strcmp(S.path, "sd:/") && S.usb == 0 && S.has_usb == 0);
	CHECK(names(&S, "alpha,Beta,games,Zeta,Another.GCM,Apple.iso,melee.iso,zebra.ISO"));
	CHECK(S.n == 8 && S.sel == 0 && S.top == 0 && !S.capped && !S.open_failed);
	CHECK(S.e[0].is_dir && !S.e[4].is_dir && S.e[5].size == 2000000UL);
	CHECK(!msb_visible(".", 1) && !msb_visible("..", 1) && !msb_visible("._x", 0) && msb_visible("x.gcm", 0) && !msb_visible("x.gcz", 0) && !msb_visible("x.iso.bak", 0) && msb_visible("Folder.iso", 1));

	/* ---- sizes in MB ---- */
	msb_size_text(a, sizeof a, ST); CHECK(!strcmp(a, "1392 MB"));
	msb_size_text(a, sizeof a, 2000000UL); CHECK(!strcmp(a, "2 MB"));
	msb_size_text(a, sizeof a, 1000UL); CHECK(!strcmp(a, "<1 MB"));
	msb_size_text(a, sizeof a, 1048576UL * 700); CHECK(!strcmp(a, "700 MB"));

	/* ---- paths: parent, root, join ---- */
	CHECK(msb_is_root("sd:/") && msb_is_root("usb:/") && !msb_is_root("sd:/a") && !msb_is_root("sd:"));
	CHECK(msb_parent("sd:/games/isos", a, sizeof a, leaf, sizeof leaf) == 1 && !strcmp(a, "sd:/games") && !strcmp(leaf, "isos"));
	CHECK(msb_parent("sd:/games", a, sizeof a, leaf, sizeof leaf) == 1 && !strcmp(a, "sd:/") && !strcmp(leaf, "games"));
	CHECK(msb_parent("sd:/games/", a, sizeof a, leaf, sizeof leaf) == 1 && !strcmp(a, "sd:/") && !strcmp(leaf, "games"));
	CHECK(msb_parent("usb:/", a, sizeof a, leaf, sizeof leaf) == 0 && !strcmp(a, "usb:/"));
	CHECK(msb_join(a, sizeof a, "sd:/", "x") && !strcmp(a, "sd:/x") && msb_join(b, sizeof b, "sd:/g", "x.iso") && !strcmp(b, "sd:/g/x.iso"));
	{ char longname[200]; memset(longname, 'n', sizeof longname - 1); longname[sizeof longname - 1] = 0; CHECK(!msb_join(a, sizeof a, "sd:/g", longname)); }
	msb_path_text(a, sizeof a, "sd:/games/isos", 60); CHECK(!strcmp(a, "sd:/games/isos"));
	msb_path_text(a, sizeof a, "sd:/aaaaaaaaaa/bbbbbbbbbb/cccccccccc", 20); CHECK(strlen(a) == 20 && !strncmp(a, "..", 2) && !strcmp(a + 2, "bbbbbbb/cccccccccc"));

	/* ---- open a folder (A), go up (B, selects the folder we came from), B at the root leaves ---- */
	reset();
	D("sd:/games"); D("sd:/games/isos"); D("sd:/zz"); F("sd:/games/a.iso", 9, 0); F("sd:/games/isos/melee.iso", ST, 1); F("sd:/games/isos/old.gcm", 123, 0);
	msb_open(&S, &FS, 0, 0);
	CHECK(names(&S, "games,zz"));
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_NONE && !strcmp(S.path, "sd:/games") && names(&S, "isos,a.iso"));
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_NONE && !strcmp(S.path, "sd:/games/isos") && names(&S, "melee.iso,old.gcm"));
	CHECK(msb_key(&S, MSB_K_B, &FS) == MSB_R_NONE && !strcmp(S.path, "sd:/games") && S.sel == 0 && S.e[S.sel].is_dir && !strcmp(S.e[S.sel].name, "isos"));
	CHECK(msb_key(&S, MSB_K_B, &FS) == MSB_R_NONE && !strcmp(S.path, "sd:/") && !strcmp(S.e[S.sel].name, "games"));
	CHECK(msb_key(&S, MSB_K_B, &FS) == MSB_R_LEAVE && !strcmp(S.path, "sd:/"));             /* B at the device root: back to the start screen */
	/* pick a file: A on it returns the path, the loader checks it */
	msb_key(&S, MSB_K_A, &FS); msb_key(&S, MSB_K_A, &FS);                                   /* games, isos */
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_PICK && !strcmp(S.pick, "sd:/games/isos/melee.iso"));

	/* ---- the verdicts: a stock image locks in, a refused one stays with one line and a red X ---- */
	CHECK(msb_set_verdict(&S, 1, "") == MSB_R_LOCKED && S.mark_kind == MSB_MARK_GOOD && S.mark_idx == (int)S.sel && S.msg[0] == 0);
	msb_key(&S, MSB_K_DOWN, &FS);
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_PICK && !strcmp(S.pick, "sd:/games/isos/old.gcm"));
	{ char why[96]; msb_why_size(why, sizeof why, 123); CHECK(!strcmp(why, "the size is <1 MB, a Melee disc image is 1392 MB"));
	  CHECK(msb_set_verdict(&S, 0, why) == MSB_R_NONE && S.mark_kind == MSB_MARK_BAD && S.mark_idx == (int)S.sel && S.msg_bad);
	  CHECK(!strcmp(S.msg, "Not stock Melee v1.02: the size is <1 MB, a Melee disc image is 1392 MB")); }
	CHECK(S.n == 2 && !strcmp(S.path, "sd:/games/isos"));                                   /* it stays in the browser, same folder */
	msb_key(&S, MSB_K_UP, &FS);                                                             /* moving clears the mark and the line */
	CHECK(S.mark_idx == -1 && S.msg[0] == 0);
	CHECK(msb_set_verdict(&S, 0, "") == MSB_R_NONE && strstr(S.msg, "this image is not accepted") != NULL);
	msb_check_stopped(&S); CHECK(S.mark_idx == -1 && !S.msg_bad && !strcmp(S.msg, "Check stopped."));
	{ char why[96];
	  msb_why_header(why, sizeof why, "GALE01"); CHECK(!strcmp(why, "another Melee revision (not v1.02)"));
	  msb_why_header(why, sizeof why, "GM4E01"); CHECK(!strcmp(why, "not Melee NTSC-U (GM4E01)"));
	  msb_why_header(why, sizeof why, "GALP01"); CHECK(!strcmp(why, "another Melee version (GALP01)")); }

	/* ---- a same-size modified image versus the stock one: both are candidates, the loader's lock tells them apart (the browser itself never decides) ---- */
	reset();
	F("sd:/stock.iso", ST, 1); F("sd:/mod.iso", ST, 0);
	msb_open(&S, &FS, 0, 0);
	CHECK(names(&S, "mod.iso,stock.iso"));
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_PICK && !strcmp(S.pick, "sd:/mod.iso"));
	CHECK(msb_set_verdict(&S, 0, "data window 5 differs") == MSB_R_NONE && !strcmp(S.msg, "Not stock Melee v1.02: data window 5 differs") && S.mark_kind == MSB_MARK_BAD);
	msb_key(&S, MSB_K_DOWN, &FS);
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_PICK && !strcmp(S.pick, "sd:/stock.iso"));
	CHECK(msb_set_verdict(&S, 1, 0) == MSB_R_LOCKED && S.mark_kind == MSB_MARK_GOOD);

	/* ---- an empty folder, a folder that does not open ---- */
	reset();
	D("sd:/empty"); D("sd:/only_txt"); F("sd:/only_txt/a.txt", 1, 0); F("sd:/only_txt/.b.iso", 1, 0);
	msb_open(&S, &FS, 0, 0);
	msb_key(&S, MSB_K_A, &FS);                                                              /* empty */
	CHECK(S.n == 0 && !strcmp(S.path, "sd:/empty") && msb_empty_line(&S) != NULL && !S.open_failed);
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_NONE && msb_key(&S, MSB_K_UP, &FS) == MSB_R_NONE && msb_key(&S, MSB_K_DOWN, &FS) == MSB_R_NONE && msb_key(&S, MSB_K_RIGHT, &FS) == MSB_R_NONE);   /* nothing to act on, nothing breaks */
	CHECK(msb_key(&S, MSB_K_B, &FS) == MSB_R_NONE && !strcmp(S.path, "sd:/") && !strcmp(S.e[S.sel].name, "empty"));
	msb_key(&S, MSB_K_DOWN, &FS); msb_key(&S, MSB_K_A, &FS);                                /* only_txt: nothing visible */
	CHECK(S.n == 0 && !strcmp(S.path, "sd:/only_txt"));
	msb_root(&S, 0); strcpy(S.path, "sd:/gone"); msb_load(&S, &FS);
	CHECK(S.open_failed && S.n == 0 && S.msg_bad && msb_empty_line(&S) != NULL);

	/* ---- a huge folder (more than 256 entries, more than the 512 kept): sorted, paged, capped ---- */
	reset();
	D("sd:/big");
	for (i = 0; i < 700; i++) { char p[64]; snprintf(p, sizeof p, "sd:/big/f%04u.iso", 699 - i); F(p, 100 + i, 0); }       /* added in reverse order */
	for (i = 0; i < 20; i++) { char p[64]; snprintf(p, sizeof p, "sd:/big/d%02u", 19 - i); D(p); }
	msb_open(&S, &FS, 0, 0);
	msb_key(&S, MSB_K_A, &FS);
	CHECK(S.n == MSB_MAX && S.capped && strstr(S.msg, "512") != NULL && !S.msg_bad);
	CHECK(S.e[0].is_dir && !strcmp(S.e[0].name, "d00") && !strcmp(S.e[19].name, "d19") && !S.e[20].is_dir && !strcmp(S.e[20].name, "f0000.iso") && !strcmp(S.e[MSB_MAX - 1].name, "f0491.iso"));
	for (i = 1; i < S.n; i++) CHECK(msb_before(&S.e[i - 1], &S.e[i]));                      /* strictly sorted */
	/* paging: Right moves a page and clamps at the end, Left clamps at the start; the window always holds the selection */
	CHECK(S.sel == 0 && S.top == 0);
	msb_key(&S, MSB_K_RIGHT, &FS); CHECK(S.sel == MSB_ROWS && S.top == 1);
	msb_key(&S, MSB_K_RIGHT, &FS); CHECK(S.sel == 2 * MSB_ROWS && S.top == MSB_ROWS + 1);
	for (i = 0; i < 100; i++) msb_key(&S, MSB_K_RIGHT, &FS);
	CHECK(S.sel == S.n - 1 && S.top == S.n - MSB_ROWS);
	msb_key(&S, MSB_K_DOWN, &FS); CHECK(S.sel == 0 && S.top == 0);                          /* Down at the last row wraps to the first */
	msb_key(&S, MSB_K_UP, &FS); CHECK(S.sel == S.n - 1 && S.top == S.n - MSB_ROWS);         /* Up at the first wraps to the last */
	for (i = 0; i < 100; i++) msb_key(&S, MSB_K_LEFT, &FS);
	CHECK(S.sel == 0 && S.top == 0);
	for (i = 0; i < 300; i++) { msb_key(&S, MSB_K_DOWN, &FS); CHECK(S.sel >= S.top && S.sel < S.top + MSB_ROWS); }       /* 300 single steps: more than 256, always on screen */
	CHECK(S.sel == 300);
	for (i = 0; i < 300; i++) { msb_key(&S, MSB_K_UP, &FS); CHECK(S.sel >= S.top && S.sel < S.top + MSB_ROWS); }
	CHECK(S.sel == 0 && S.top == 0);
	CHECK(S.msg[0] != 0);                                                                   /* the capped note stays while moving */
	/* an uncapped folder of 300 entries pages without a cap */
	reset();
	D("sd:/m");
	for (i = 0; i < 300; i++) { char p[64]; snprintf(p, sizeof p, "sd:/m/g%03u.gcm", i); F(p, 5, 0); }
	msb_open(&S, &FS, 0, 0); msb_key(&S, MSB_K_A, &FS);
	CHECK(S.n == 300 && !S.capped && S.msg[0] == 0);
	for (i = 0; i < 40; i++) msb_key(&S, MSB_K_RIGHT, &FS);
	CHECK(S.sel == 299 && S.top == 300 - MSB_ROWS);
	/* a short list never scrolls */
	reset(); F("sd:/a.iso", 1, 0); F("sd:/b.iso", 1, 0); F("sd:/c.iso", 1, 0);
	msb_open(&S, &FS, 0, 0);
	msb_key(&S, MSB_K_RIGHT, &FS); CHECK(S.sel == 2 && S.top == 0);
	msb_key(&S, MSB_K_LEFT, &FS); CHECK(S.sel == 0 && S.top == 0);

	/* ---- the device switch: Y toggles SD / USB when both are there, never otherwise; each device has its own root ---- */
	reset();
	D("sd:/games"); F("sd:/s.iso", 7, 0); D("usb:/roms"); D("usb:/Backup"); F("usb:/u.gcm", 7, 0);
	msb_open(&S, &FS, 0, 1);
	CHECK(S.usb == 0 && S.has_usb && names(&S, "games,s.iso"));
	msb_key(&S, MSB_K_A, &FS);                                                              /* into sd:/games */
	CHECK(msb_key(&S, MSB_K_Y, &FS) == MSB_R_NONE && S.usb == 1 && !strcmp(S.path, "usb:/") && names(&S, "Backup,roms,u.gcm"));    /* USB opens at its root */
	CHECK(msb_key(&S, MSB_K_Y, &FS) == MSB_R_NONE && S.usb == 0 && !strcmp(S.path, "sd:/") && names(&S, "games,s.iso"));
	msb_key(&S, MSB_K_Y, &FS); CHECK(msb_key(&S, MSB_K_B, &FS) == MSB_R_LEAVE);             /* B at the USB root leaves too */
	msb_open(&S, &FS, 0, 0);
	CHECK(msb_key(&S, MSB_K_Y, &FS) == MSB_R_NONE && S.usb == 0 && !strcmp(S.path, "sd:/"));  /* one device: Y does nothing */
	msb_open(&S, &FS, 1, 0);
	CHECK(S.usb == 1 && !strcmp(S.path, "usb:/") && names(&S, "Backup,roms,u.gcm"));         /* USB only (SD not mounted) */
	/* a file picked on USB */
	msb_open(&S, &FS, 1, 1);
	msb_key(&S, MSB_K_DOWN, &FS); msb_key(&S, MSB_K_DOWN, &FS);
	CHECK(msb_key(&S, MSB_K_A, &FS) == MSB_R_PICK && !strcmp(S.pick, "usb:/u.gcm"));

	/* ---- the footer states the keys on every screen ---- */
	{
		struct msb_keys k;
		reset(); D("sd:/g"); F("sd:/a.iso", 1, 0);
		msb_open(&S, &FS, 0, 0);
		msb_footer(&S, &k);
		CHECK(k.n == 3 && !strcmp(k.key[0], "B") && !strcmp(k.act[0], "Back") && !strcmp(k.act[1], "Open") && !strcmp(k.key[2], "Left/Right") && !strcmp(k.act[2], "Page"));   /* root: B = Back; on a folder A = Open */
		msb_key(&S, MSB_K_DOWN, &FS); msb_footer(&S, &k); CHECK(!strcmp(k.act[1], "Select"));      /* on a file A = Select */
		msb_key(&S, MSB_K_UP, &FS); msb_key(&S, MSB_K_A, &FS); msb_footer(&S, &k); CHECK(!strcmp(k.act[0], "Up"));    /* in a folder B = Up */
		msb_open(&S, &FS, 0, 1); msb_footer(&S, &k);
		CHECK(k.n == 4 && !strcmp(k.key[3], "Y") && !strcmp(k.act[3], "USB"));
		msb_key(&S, MSB_K_Y, &FS); msb_footer(&S, &k); CHECK(!strcmp(k.act[3], "SD"));
	}
	/* a name that is exactly at the limit and the long-name guard: names of MSB_NAME or more never arrive (the reader skips them), shorter ones are kept whole */
	reset();
	{ char p[200], nm[MSB_NAME]; memset(nm, 'x', MSB_NAME - 5); nm[MSB_NAME - 5] = 0; strcat(nm, ".iso"); snprintf(p, sizeof p, "sd:/%s", nm); F(p, 1, 0); msb_open(&S, &FS, 0, 0); CHECK(S.n == 1 && !strcmp(S.e[0].name, nm)); }
	printf("OK\n");
	return 0;
}
