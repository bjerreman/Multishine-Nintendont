/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Nintendont shell, Memory card creation: the Multishine system files (the card file MultishinePack) are built INTO the new virtual card.
 * The pure part, platform neutral (no heap of its own, no file system): the loader (ms_ota_loader.c) and tests/pack_test.c compile this same file.
 *   1. the data container pack-N.mscf, checked with the pinned keys like every other kind
 *   2. the player's own retail Melee disc image: the four menu files are found in its file system table (GCM FST), nothing is copied but what the build reads
 *   3. the build: the SAME code as the game and the system-files builder
 *   4. the pack goes into the card image as a normal card file (mscard_add), after the five files; a pack the old card already had is carried over instead of rebuilt
 * Needs offline/seed (seed_lean.h, seed_slp.h) and ms_card_core.h. */
#ifndef MS_PACK_CORE_H
#define MS_PACK_CORE_H
#include "ms_card_core.h"
#include "seed_lean.h"

#define MSPK_ABI        4u             /* MSCF ABI of the system-files data container (pack-N.mscf) */
#define MSPK_MAX_BLOCKS 128u           /* the project's other tooling MAX_FILE: header block + payload */
#define MSPK_MAX_FILE   ((MSPK_MAX_BLOCKS + 3u) * 0x2000u)
#define MSPK_NAME       "MultishinePack"
#define MSPK_MAX_PACK_BLOCKS 40u       /* the stock pack is 30 blocks; anything that is not about that size is not ours */
#define MSPK_DISC_SIZE  1459978240u    /* a full 1:1 GameCube disc image (the only size the game list offers) */
#define MSPK_FST_MAX    (512u * 1024u)

enum { MSPK_OK = 0, MSPK_E_READ = -1, MSPK_E_MEM = -2, MSPK_E_FST = -3, MSPK_E_NOFILES = -4, MSPK_E_NODISC = -5, MSPK_E_INPUT = -6, MSPK_E_BUILD = -7, MSPK_E_PACK = -8, MSPK_E_DISCHDR = -9 };

/* ---- 1. the data container ---------------------------------------------------------------------------------------------------------------------------------------------------- */
struct mspk_data {
    const mw2_u8 *pl;                  /* the payload (points into the checked file buffer) */
    mw2_u32 plen;
    mw2_u32 off[12], len[12];          /* by slp_io id 4..9: MnExtAll MnMaAll SdMenu SdSlChr .usd.diff, slpCSS.dat, head.bin; 11 (optional): MnSlMap.usd.diff */
    mw2_u32 seq;
};
/* the payload of a container: 1 = ok and *out filled, 0 = refused */
int mspk_parse(const mw2_u8 *pl, mw2_u32 n, struct mspk_data *out);
/* a served pack-N.mscf: the same order of checks as msota_check (size, header, ABI 4, seq, header CRC, payload CRC, signature, nothing after the payload), then the payload (MSOTA_E_PAYLOAD) */
int msota_check_pack(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct mspk_data *out);

/* ---- 2. the player's disc ----------------------------------------------------------------------------------------------------------------------------------------------------- */
/* read len bytes at byte offset off of the disc image into dst (any alignment); 0 = ok */
typedef int (*mspk_rd_fn)(void *ctx, mw2_u32 off, mw2_u8 *dst, mw2_u32 len);
struct mspk_host {
    void *ctx;
    mspk_rd_fn rd;
    mw2_u32 disc_size;
    void *(*alloc)(void *ctx, mw2_u32 n);              /* 32-byte aligned (memalign), 0 = none */
    void (*release)(void *ctx, void *p);
};
struct mspk_loc { mw2_u32 off, len; };
/* the cheap header check of the image's first 0x20 bytes: game ID GALE01, disc 0, revision 2, the GameCube magic 0xC2339F3D at 0x1C (the game list's test); 1 = yes */
int mspk_disc_header_ok(const mw2_u8 *hdr32);
/* the root-level files MnExtAll.usd MnMaAll.usd SdMenu.usd SdSlChr.usd in the disc's FST -> loc[0..3]; MSPK_E_FST / MSPK_E_NOFILES / MSPK_E_READ / MSPK_E_MEM */
int mspk_find_files(const struct mspk_host *h, struct mspk_loc loc[5]);       /* 0..3 are required, [4] (MnSlMap.usd, the optional pack entry) is len 0 when the disc lacks it */

/* ---- 3. the build ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
struct mspk_build {
    struct slpl sl;
    struct slpl_io io;
    const struct mspk_host *h;
    const struct mspk_data *pk;
    struct mspk_loc loc[5];      /* the player's disc files by slp_io id 0..3 and, optional, [4] = MnSlMap.usd (slp_io id 10) */
    int begun, done;
};
/* MSPK_OK = ready to step; MSPK_E_NODISC = the disc is not the stock retail files (a size differs or a file is missing); MSPK_E_FST / _READ / _MEM */
int mspk_begin(struct mspk_build *b, const struct mspk_host *h, const struct mspk_data *pk);
/* one slice of work: 0 = running (*pct 0..99), 1 = done (*pct 100), else MSPK_E_INPUT (another disc or a changed file: a CRC differs) / _READ / _MEM / _BUILD */
int mspk_step(struct mspk_build *b, mw2_u32 budget, unsigned *pct);
/* the finished card file: the 0x40-byte directory entry followed by blocks * 0x2000 bytes; valid after mspk_step returned 1 until mspk_end */
const mw2_u8 *mspk_gci(const struct mspk_build *b, mw2_u32 *len);
void mspk_end(struct mspk_build *b);

/* ---- 4. the card -------------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* a MultishinePack card file (entry + blocks) that is whole: the name, a plausible size, 'MSPK' table, its CRC, every entry's stored CRC (the same test as seed_plan.c sp_scan_pack): 1 = ok */
int mspk_pack_valid(const mw2_u8 *gci, mw2_u32 len);
enum { MSPK_ADD_OK = 0, MSPK_ADD_BAD = 1, MSPK_ADD_NOSPACE = 2, MSPK_ADD_FAILED = 3 };
/* put the pack into the card image (after the five files): MSPK_ADD_BAD = not a whole pack, _NOSPACE = fewer free blocks than it needs (nothing changed), _FAILED = the card did not check afterwards (the caller drops the image) */
int mspk_card_add(mw2_u8 *raw, mw2_u32 size, const mw2_u8 *gci, mw2_u32 len);
/* the MultishinePack of an existing card image (a whole file read from SD, at most 2 MiB): copies its entry + blocks into out (cap bytes) -> 1 and *len when the card checks and the file is whole, 0 otherwise */
int mspk_card_extract(const mw2_u8 *raw, mw2_u32 size, mw2_u8 *out, mw2_u32 cap, mw2_u32 *len);

/* ---- which image on the card is the disc ------------------------------------------------------------------------------------------------------------------------------------- */
/* The card may hold several Melee images (a modified one, the stock one twice, ...).  The pack is built only from one the disc lock accepts (stock NTSC-U v1.02).
   list: the i-th candidate image (a full GALE01 rev 2 image file) in name order -> 1 and its path, 0 = no more.
   check: 1 = the lock accepts it, 0 = it does not, -1 = (cache_only) not known without reading the image. */
typedef int (*mspk_list_fn)(void *ctx, unsigned i, char *path, unsigned n);
typedef int (*mspk_check_fn)(void *ctx, const char *path, int cache_only);
#define MSPK_MAX_CAND 64
#define MSPK_MAX_PICK 8
/* the accepted images, in the order they are to be tried: first those the verdict cache already accepts (name order), then the others the lock accepts (name order), at most `max`.
   path is an array of max strings of plen bytes.  *seen = how many candidates there were.  -> how many were picked (0 = none is the stock disc) */
int mspk_pick_discs(mspk_list_fn list, mspk_check_fn check, void *ctx, char *path, unsigned plen, unsigned max, unsigned *seen);
/* the line for the case that no candidate is the stock disc: 'Found 3 images: none is stock Melee v1.02.' */
void mspk_none_text(char *out, unsigned n, unsigned seen);

/* ---- what the player is told -------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { MSPK_O_BUILT = 0, MSPK_O_KEPT, MSPK_O_NODISC, MSPK_O_BADDISC, MSPK_O_NODOWNLOAD, MSPK_O_BUILDFAIL, MSPK_O_NOSPACE, MSPK_O_DISCREAD, MSPK_O_COUNT };
/* the first line, plain English (the files are 'Multishine system files'); for every outcome but BUILT / KEPT also a second line (mspk_text2) */
const char *mspk_text(int outcome);
const char *mspk_text2(int outcome);
int mspk_outcome_ok(int outcome);                              /* the card holds the system files */
int mspk_outcome_of_build(int rc);                             /* an MSPK_E_* of mspk_begin / mspk_step -> the outcome the screen shows */

#endif
