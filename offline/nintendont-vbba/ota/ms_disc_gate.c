/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The disc lock in the Wii loader, loader-side half: the game list shows only supported discs, and the start of a game is checked before the kernel
 * is told to run it.  The decision itself is ms_disc_lock.c (portable, host-tested).  Only the stock NTSC-U Melee v1.02 image passes. */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "global.h"
#include "menu.h"
#include "font.h"
#include "ff_utf8.h"
#include "ms_disc_lock.h"

static unsigned hidden_discs;

static void path_of(char *out, unsigned n, const char *tail)
{
	snprintf(out, n, "%s:/multishine/%s", GetRootDevice(), tail);
}

/* the game list: the cheap header check of what IsDiscImageValid found (ID, revision, disc number, a 1:1 disc image); the pseudo entries for a real disc and extracted game folders never pass */
int ms_disc_list_ok(const char *id, unsigned rev, unsigned flags, const char *path)
{
	int ok;
	if (path && path[0] == 'd' && path[1] == 'i' && path[2] == ':') return 1;      /* the drive entry 'Boot GC Disc in Drive': always listed; the header is checked after the drive is read (ms_disc_gate_drive) */
	ok = path
	     && !memcmp(id, "GALE01", 6) && rev == 2u
	     && (flags & GIFLAG_FORMAT_MASK) == GIFLAG_FORMAT_FULL && (flags & GIFLAG_DISCNUMBER_MASK) == 0;
	if (!ok) hidden_discs++;
	return ok;
}

static FIL g_f;
/* the file browser's hooks into ms_disc_check_path: ms_disc_poll (called between reads, 1 = the player pressed B: the check ends as unreadable and nothing is cached), ms_disc_aborted (that happened)
   and ms_disc_why (the plain reason of the last refusal, "" after an accepted image; the browser shows it as 'Not stock Melee v1.02: ...') */
int (*ms_disc_poll)(void);
char ms_disc_why[96];
int ms_disc_aborted;
static int rd_cb(void *ctx, mdl_u32 off, mdl_u8 *buf, mdl_u32 len)
{
	UINT got = 0;
	(void)ctx;
	if (ms_disc_poll && ms_disc_poll()) { ms_disc_aborted = 1; return -1; }
	if (f_lseek(&g_f, off) != FR_OK) return -1;
	if (f_read(&g_f, buf, len, &got) != FR_OK || got != len) return -1;
	return 0;
}

extern int ms_ui_quiet;
extern void ms_ota_logf(const char *fmt, ...);     /* ms_ota_loader.c: SD:/multishine/ota.log */
static void refuse4(const char *l1, const char *l2, const char *l3, const char *l4);
static void refuse(const char *l1, const char *l2, const char *l3) { refuse4(l1, l2, l3, ""); }
static void refuse4(const char *l1, const char *l2, const char *l3, const char *l4)
{
	ms_ui_quiet = 0;                       /* the launch screen is up: show this text */
	ClearScreen();
	PrintInfo();
	PrintFormat(DEFAULT_SIZE, MAROON, (640 - (int)GRRLIB_WidthTTF(myFont, l1, DEFAULT_SIZE)) / 2, 212, "%s", l1);
	PrintFormat(DEFAULT_SIZE, MAROON, (640 - (int)GRRLIB_WidthTTF(myFont, l2, DEFAULT_SIZE)) / 2, 232, "%s", l2);
	if (l3[0]) PrintFormat(DEFAULT_SIZE, BLACK, (640 - (int)GRRLIB_WidthTTF(myFont, l3, DEFAULT_SIZE)) / 2, 262, "%s", l3);
	if (l4[0]) PrintFormat(14, 0x98ABC2FF, (640 - (int)GRRLIB_WidthTTF(myFont, l4, 14)) / 2, 288, "%s", l4);       /* the reason class, small and muted */
	ExitToLoader(1);
}

static unsigned read_cache(char *buf)
{
	char p[64];
	FIL f;
	UINT got = 0;
	path_of(p, sizeof p, "disc.cache");
	if (f_open_char(&f, p, FA_READ | FA_OPEN_EXISTING) != FR_OK) return 0;
	f_read(&f, buf, MSDL_CACHE_MAX, &got);
	f_close(&f);
	return got;
}

static void write_cache(const char *buf, unsigned len)
{
	char p[64], d[64];
	FIL f;
	UINT put = 0;
	snprintf(d, sizeof d, "%s:/multishine", GetRootDevice());
	f_mkdir_char(d);
	path_of(p, sizeof p, "disc.cache");
	if (f_open_char(&f, p, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
	f_write(&f, buf, len, &put);
	f_close(&f);                                           /* this loader's f_close can report an error although the data is written: the verdict is recomputed anyway if the cache is damaged */
}

/* one line that explains the decision: path, size, header, a 14 character window map ('.' = the common window matches the stock disc, 'X' = it differs), kind, verdict, cache */
static void disc_log(const char *path, unsigned long size, const struct msdl_info *info, int v, const char *cache)
{
	char map[33];
	int i;
	for (i = 0; i < msdl_ncommon() && i < 32; i++) map[i] = (info->bad_mask >> i) & 1u ? 'X' : '.';
	map[i] = 0;
	ms_ota_logf("disc lock: %s size %lu header %s rev %u windows %s first differing %d kind/verdict %d (%s) cache %s", path, size, info->id, (unsigned)info->rev, map, info->first_bad, v, msdl_reason(v), cache);
}

/* called by main once the game is chosen (list or autoboot) and before anything is started */
extern void ms_inject_check(int disc_ok);         /* ms_inject.c */
void ms_disc_gate(void)
{
	char path[300], l3[80], l4[64], id[7];
	struct msdl_info info;
	char *cache;
	FILINFO fi;
	unsigned long size, mtime;
	unsigned clen;
	int v = 0, cached = 0;
	mdl_u8 rev = 0;
	WCHAR w[300];
	if (!(ncfg->GamePath[0] == 'd' && ncfg->GamePath[1] == 'i' && (ncfg->GamePath[2] == 0 || ncfg->GamePath[2] == ':'))) ms_inject_check(0);     /* a file game starts without the inject until its own check passes; the drive's verdict comes from ms_disc_gate_drive */
	if (ncfg->GamePath[0] == 'd' && ncfg->GamePath[1] == 'i' && (ncfg->GamePath[2] == 0 || ncfg->GamePath[2] == ':')) return;     /* the physical disc: checked by ms_disc_gate_drive once the loader has read its header */
	snprintf(path, sizeof path, "%s:%s", GetRootDevice(), ncfg->GamePath);
	if (f_open_char(&g_f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
		refuse("Multishine needs your own Melee v1.02 (NTSC-U) disc.", "This disc could not be read.", "");
		return;
	}
	memset(&fi, 0, sizeof fi);
	size = (unsigned long)f_size(&g_f);
	mtime = 0;
	{ unsigned i; for (i = 0; i < sizeof w / sizeof w[0] - 1 && path[i]; i++) w[i] = (WCHAR)(unsigned char)path[i]; w[i] = 0; }
	if (f_stat(w, &fi) == FR_OK) mtime = ((unsigned long)fi.fdate << 16) | fi.ftime;
	cache = (char *)memalign(32, MSDL_CACHE_MAX + 64);
	clen = cache ? read_cache(cache) : 0;
	if (cache && msdl_cache_get(cache, clen, msdl_fnv(path), size, mtime, &v)) cached = 1;
	memset(&info, 0, sizeof info);
	if (!cached || v != 2) {                           /* no cache entry, a refusal or an older entry: (re)compute, a refusal must be explained */
		v = msdl_verdict_ex(rd_cb, 0, size, &info);
		if (v == MSDL_READ_ERROR) { f_close(&g_f); if (cache) free(cache); disc_log(path, size, &info, v, "miss"); refuse("Multishine needs your own Melee v1.02 (NTSC-U) disc.", "This disc could not be read.", ""); return; }
		if (cache && !cached) {
			clen = msdl_cache_put(cache, MSDL_CACHE_MAX, clen, msdl_fnv(path), size, mtime, v > 0 ? 2 : 0);
			write_cache(cache, clen);
		}
		disc_log(path, size, &info, v, cached ? "hit (refusal recomputed)" : "miss");
	} else {
		v = MSDL_OK_STOCK;
		ms_ota_logf("disc lock: %s size %lu: accepted from the cache (hit)", path, size);
	}
	if (v > 0) {
		f_close(&g_f);
		if (cache) free(cache);
		ms_inject_check(1);
		return;
	}
	strcpy(id, info.id[0] ? info.id : "??????");
	rev = info.rev;
	f_close(&g_f);
	if (cache) free(cache);
	snprintf(l3, sizeof l3, "Detected: %s, revision %u", id, (unsigned)rev);
	msdl_detail(v, &info, l4, sizeof l4);
	refuse4("Multishine needs your own Melee v1.02 (NTSC-U) disc.", "This disc is not supported.", l3, l4);
}

/* a disc image file (a path of the form "sd:/games/...") against the same lock as a game start, for the Memory card's system files: 1 = accepted (stock v1.02), 0 = refused or not readable,
   -1 = (cache_only) the verdict cache has no accepting line for it.  Uses and feeds the same disc.cache as ms_disc_gate (same key: root device + path, size, mtime). */
int ms_disc_check_path(const char *p, int cache_only)
{
	char path[300];
	struct msdl_info info;
	char *cache;
	FILINFO fi;
	unsigned long size, mtime = 0;
	unsigned clen = 0;
	int v = 0, cached = 0;
	WCHAR w[300];
	ms_disc_why[0] = 0; ms_disc_aborted = 0;
	snprintf(path, sizeof path, "%s:%s", GetRootDevice(), (p[0] && p[1] && p[2] == ':') ? p + 3 : (p[0] && p[1] && p[2] && p[3] == ':') ? p + 4 : p);       /* "sd:/games/..." or "usb:/games/...": the root is the loader's current device (UseSD) */
	if (f_open_char(&g_f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) { snprintf(ms_disc_why, sizeof ms_disc_why, "the file could not be opened"); return 0; }
	memset(&fi, 0, sizeof fi);
	size = (unsigned long)f_size(&g_f);
	{ unsigned i; for (i = 0; i < sizeof w / sizeof w[0] - 1 && path[i]; i++) w[i] = (WCHAR)(unsigned char)path[i]; w[i] = 0; }
	if (f_stat(w, &fi) == FR_OK) mtime = ((unsigned long)fi.fdate << 16) | fi.ftime;
	cache = (char *)memalign(32, MSDL_CACHE_MAX + 64);
	clen = cache ? read_cache(cache) : 0;
	if (cache && msdl_cache_get(cache, clen, msdl_fnv(path), size, mtime, &v)) cached = 1;
	if (cached && v == 2) { f_close(&g_f); if (cache) free(cache); ms_ota_logf("disc lock: %s size %lu: accepted from the cache (card build)", path, size); return 1; }
	if (cache_only) { f_close(&g_f); if (cache) free(cache); return -1; }
	memset(&info, 0, sizeof info);
	{ u64 t0 = gettime();
	v = msdl_verdict_ex(rd_cb, 0, size, &info);
	ms_ota_logf("disc lock: %s full check took %lu ms (cache miss)", path, (unsigned long)ticks_to_millisecs(gettime() - t0)); }      /* the cost of a cold check on this device, once per image (the verdict is cached by path, size, mtime) */
	if (cache && !cached && v != MSDL_READ_ERROR) {
		clen = msdl_cache_put(cache, MSDL_CACHE_MAX, clen, msdl_fnv(path), size, mtime, v > 0 ? 2 : 0);
		write_cache(cache, clen);
	}
	disc_log(path, size, &info, v, "card build");
	if (v <= 0) msdl_detail(v, &info, ms_disc_why, sizeof ms_disc_why);
	f_close(&g_f);
	if (cache) free(cache);
	return v > 0 ? 1 : 0;
}

/* the loader has just read the first 0x20 bytes of the disc in the drive (ID, revision, magic) for its own use: no extra DI access. */
void ms_disc_gate_drive(const unsigned char *hdr)
{
	ms_inject_check(0);
	char id[7], l3[80];
	mdl_u8 rev = 0;
	strcpy(id, "??????");
	if (msdl_header_ok((const mdl_u8 *)hdr, id, &rev)) { ms_inject_check(1); return; }
	id[6] = 0;
	snprintf(l3, sizeof l3, "Detected: %s, revision %u", id, (unsigned)rev);
	refuse("Multishine needs your own Melee v1.02 (NTSC-U) disc.", "This disc is not supported.", l3);
}

unsigned ms_disc_hidden(void) { return hidden_discs; }
