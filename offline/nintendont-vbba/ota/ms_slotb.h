/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The backup card in slot B: an extra, empty, correctly formatted virtual card, SD:/multishine/saves/GALE_B.raw, the same size as slot A, so the player can copy the save
 * and the Multishine files to it with Melee's own memory card manager.  Pure decisions and the create procedure with injected file operations: host-tested (tests/slotb_test.c); the loader
 * (ms_ota_loader.c slotb_ensure) gathers the facts and supplies the operations.
 *
 * What the kernel does (Nintendont kernel/GCNCard.c, EXI.c): at its start it clears NIN_CFG_MC_SLOTB, loads slot A, then tries /saves/<GAMEID>_B.raw (GALE_B.raw here, the Multishine path patch maps
 * /saves to SD:/multishine/saves).  A missing file, a size that is not a valid card size, or A + B above 16 MiB just disables slot B (a log line), and a loaded slot B SETS the bit itself.  So the
 * loader never touches that bit: the file is the switch (memory card emulation, which ms_cfg_force turns on while GALE.raw exists, is the only other condition).
 *
 * Rules: slot A is never touched; an existing B is never overwritten; one 2 MiB write at most (bounded cost); never when the SD is short of space; a marker (slotb_done) remembers that the card was
 * made (or seen), so a player who deletes B does not get it back at every start; a failed attempt counts in slotb_fail (its size = the number of failures), three failures stop the tries (the
 * set-up ignores that and the marker, because it is the one moment a fresh card pair is wanted).  A failure never fails the set-up or a start. */
#ifndef MS_SLOTB_H
#define MS_SLOTB_H

#include "ms_vroot.h"
#define MSB_RAW   ms_vpath("saves/GALE_B.raw")
#define MSB_TMP   ms_vpath("saves/GALE_B.tmp")       /* written whole first, then renamed: a half-written card never has the name the kernel looks for */
#define MSB_MARK  ms_vpath("slotb_done")
#define MSB_FAILS "sd:/multishine/slotb_fail"
#define MSB_MAX_FAILS 3
#define MSB_KERNEL_MAX (16u * 1024u * 1024u)               /* the kernel keeps both cards in 16 MB of RAM */
#define MSB_MIN_SIZE   (512u * 1024u)

struct msb_facts {
	int sd_ok;              /* the SD is mounted */
	int a_exists;           /* GALE.raw is there */
	unsigned long a_size;   /* its size in bytes (0 = unknown) */
	int b_exists;           /* GALE_B.raw is there */
	int marker;             /* slotb_done is there */
	int fails;              /* the number of failed tries (size of slotb_fail) */
	int from_setup;         /* called by the quick set-up right after slot A was made */
	int free_known;         /* the free space could be read */
	unsigned long free_bytes;
};

enum {
	MSB_CREATE = 0,         /* make it */
	MSB_SKIP_SD,            /* no SD */
	MSB_SKIP_NO_A,          /* slot A does not exist: nothing to back up beside */
	MSB_SKIP_EXISTS,        /* B is there: never touched (the caller records the marker when it is missing) */
	MSB_SKIP_SIZE,          /* A is not a size the kernel can pair with a B card */
	MSB_SKIP_MARKER,        /* made once before and deleted by the player */
	MSB_SKIP_FAILS,         /* tried and failed three times */
	MSB_SKIP_SPACE          /* the SD is short of space (or its space is unknown) */
};

/* a card size the kernel accepts for B beside an A of the same size: a power of two, 512 KiB .. 8 MiB (2 x size <= 16 MiB) */
static inline int msb_size_ok(unsigned long a)
{
	return a >= MSB_MIN_SIZE && (a & (a - 1)) == 0 && 2ul * a <= MSB_KERNEL_MAX;
}
/* the SD is short when it cannot hold `need` plus 256 KB (the same rule as msota_space_ok) */
static inline int msb_space_short(unsigned long free_bytes, unsigned long need) { return free_bytes < need + 0x40000ul; }

static inline int msb_decide(const struct msb_facts *f)
{
	if (!f->sd_ok) return MSB_SKIP_SD;
	if (!f->a_exists) return MSB_SKIP_NO_A;
	if (f->b_exists) return MSB_SKIP_EXISTS;
	if (!msb_size_ok(f->a_size)) return MSB_SKIP_SIZE;
	if (!f->from_setup && f->marker) return MSB_SKIP_MARKER;
	if (!f->from_setup && f->fails >= MSB_MAX_FAILS) return MSB_SKIP_FAILS;
	if (!f->free_known || msb_space_short(f->free_bytes, f->a_size)) return MSB_SKIP_SPACE;
	return MSB_CREATE;
}
static inline const char *msb_skip_text(int d)
{
	switch (d) {
	case MSB_CREATE: return "create";
	case MSB_SKIP_SD: return "skip: no SD";
	case MSB_SKIP_NO_A: return "skip: no slot A card";
	case MSB_SKIP_EXISTS: return "skip: already there";
	case MSB_SKIP_SIZE: return "skip: slot A size not pairable";
	case MSB_SKIP_MARKER: return "skip: made before and removed by the player";
	case MSB_SKIP_FAILS: return "skip: three failed tries";
	case MSB_SKIP_SPACE: return "skip: SD space short or unknown";
	}
	return "";
}

/* the one line of the start screen, About and the Memory card screen */
static inline const char *msb_line(int ready) { return ready ? "Backup card (slot B): ready" : "Backup card (slot B): none"; }

/* ---- the create procedure -------------------------------------------------------------------------------------------------------------------------------------------- */
struct msb_ops {
	void *ctx;
	int (*exists)(void *ctx, const char *path);
	int (*unlink)(void *ctx, const char *path);
	int (*write_tmp)(void *ctx);                       /* format, write MSB_TMP whole and read the head back: 0 = ok, else the code of the failing step */
	int (*rename)(void *ctx, const char *from, const char *to);   /* 0 = ok, else the code (the SD driver also reports errors for renames that happened: the directory decides) */
	int (*put_marker)(void *ctx, const char *path, int count);    /* a small file: count bytes */
	void (*log)(void *ctx, const char *what, int rc);
};
enum { MSB_R_OK = 0, MSB_R_WRITE, MSB_R_RENAME, MSB_R_RACE };

/* -> MSB_R_*.  OK: GALE_B.raw is there, the marker is written (a failed marker write is only logged: the card exists).  Every failure leaves no GALE_B.tmp and no GALE_B.raw made by this call, and
   counts one failure (fails = the count before this try) */
static inline int msb_create(const struct msb_ops *o, int fails)
{
	int rc, r;
	if (o->exists(o->ctx, MSB_TMP)) o->unlink(o->ctx, MSB_TMP);          /* the rest of an earlier attempt */
	rc = o->write_tmp(o->ctx);
	if (rc != 0) {
		o->unlink(o->ctx, MSB_TMP);
		o->put_marker(o->ctx, MSB_FAILS, fails + 1);
		if (o->log) o->log(o->ctx, "write", rc);
		return MSB_R_WRITE;
	}
	if (o->exists(o->ctx, MSB_RAW)) {                                    /* it appeared meanwhile: never overwritten */
		o->unlink(o->ctx, MSB_TMP);
		if (o->log) o->log(o->ctx, "race", 0);
		return MSB_R_RACE;
	}
	r = o->rename(o->ctx, MSB_TMP, MSB_RAW);
	if (r != 0 && !(o->exists(o->ctx, MSB_RAW) && !o->exists(o->ctx, MSB_TMP))) {   /* trust the directory, as bk_rename does */
		o->unlink(o->ctx, MSB_TMP);
		o->put_marker(o->ctx, MSB_FAILS, fails + 1);
		if (o->log) o->log(o->ctx, "rename", r);
		return MSB_R_RENAME;
	}
	if (o->put_marker(o->ctx, MSB_MARK, 1) != 0 && o->log) o->log(o->ctx, "marker", 0);
	return MSB_R_OK;
}
#endif
