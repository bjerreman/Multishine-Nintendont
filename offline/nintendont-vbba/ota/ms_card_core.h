/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Nintendont shell, Memory card option, the pure part: the card container crd-N.mscf (MSCF ABI 8, tools/card/crd_data.py) and a raw GameCube
 * memory card image.  Platform neutral, no heap: the loader (ms_ota_loader.c) and tests/card_test.c
 * compile this same file.  Needs ms_ota_core.h for the container check. */
#ifndef MS_CARD_CORE_H
#define MS_CARD_CORE_H
#include "ms_ota_core.h"

#define MSCRD_ABI        8u            /* MSCF ABI of the card container (crd-N.mscf): 5 files, with the standard Melee save (ABI 7 had 4 and is refused) */
#define MSCRD_SHELL_ABI  1u            /* what this shell offers to a card container */
#define MSCRD_NFILES     5u
#define MSCRD_MAX_BLOCKS 161u
#define MSCRD_LOADER_BLOCKS 11u        /* the loader save SuperSmashBros0110290334 */
#define MSCRD_MELEE_BLOCKS  11u        /* the standard Melee save MultishineMelee (the fifth file) */
#define MSCRD_GCI_HDR    0x40u
#define MSCRD_MAX_PAYLOAD (32u + 5u * 4u + 5u * 0x40u + MSCRD_MAX_BLOCKS * 0x2000u)
#define MSCRD_MAX_FILE   ((MSCRD_MAX_PAYLOAD + 0x2000u * 3u) & ~0x1FFFu)

struct mscrd_file {
    const mw2_u8 *gci;                 /* the whole .gci: 0x40-byte directory entry + blocks * 0x2000 (points into the checked buffer) */
    mw2_u32 len;
    mw2_u32 blocks;
    char name[33];
};
struct mscrd_set {
    struct mscrd_file f[MSCRD_NFILES]; /* in the order of mscrd_names */
    mw2_u32 blocks;                    /* total, 150 */
    mw2_u32 seq;
};
const char *const *mscrd_names(void);  /* the five file names, loader save first, MultishineMelee last */

/* the payload of a card container (b[0..n)): 1 = ok and *out filled (out->seq = 0), 0 = refused.  The same rules as tools/card/crd_data.py parse_payload. */
int mscrd_parse(const mw2_u8 *pay, mw2_u32 n, struct mscrd_set *out);
/* a served crd-N.mscf, same order of checks as msota_check, payload 'MCRD', every file well formed (MSOTA_E_PAYLOAD / MSOTA_E_SHELL otherwise) */
int msota_check_crd(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct mscrd_set *out);

/* ---- a raw card image in memory ------------------------------------------------------------------------------------------------------------------------------------------------ */
#define MSCARD_BLOCK  0x2000u
#define MSCARD_SYS    5u
#define MSCARD_SIZE   (2u * 1024u * 1024u)       /* the virtual card we write: 2 MiB = 256 blocks, 251 usable */
enum { MSCARD_OK = 0, MSCARD_E_SIZE, MSCARD_E_HDR, MSCARD_E_DIR, MSCARD_E_BAT, MSCARD_E_ARG, MSCARD_E_EXISTS, MSCARD_E_FULL, MSCARD_E_NOSPACE, MSCARD_E_CHAIN, MSCARD_E_MISSING, MSCARD_E_DIFF };
/* format an empty card of `size` bytes (a power of two, 512 KiB..16 MiB; img must hold size bytes), USA/ANSI like the project's other tooling format_card */
int mscard_format(mw2_u8 *img, mw2_u32 size);
/* the same with only the first head_cap bytes in memory (>= 5 blocks): see ms_card_core.c; add / check / has_set then work on the head with the full `size` */
int mscard_format_head(mw2_u8 *img, mw2_u32 head_cap, mw2_u32 size);
/* add one .gci (entry + whole blocks) first-fit; the other files are not touched */
int mscard_add(mw2_u8 *img, mw2_u32 size, const mw2_u8 *gci, mw2_u32 len);
/* all checksums good and directory, chains and map agree (the same cross-check as Card.consistency()) */
int mscard_check(const mw2_u8 *img, mw2_u32 size);
/* every file of the set is on the card, with identical bytes (entry apart from its first-block field) */
int mscard_has_set(const mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set);
/* one more file (a whole .gci: entry + blocks) is on the card with identical bytes (the card checks first); the same comparison as has_set */
int mscard_has_gci(const mw2_u8 *img, mw2_u32 size, const mw2_u8 *gci, mw2_u32 len);
/* the file `name` of the card: its entry (first-block field 0) + blocks into out (cap bytes) -> the length, 0 = not there / chain broken / does not fit; the card image must be whole in memory */
mw2_u32 mscard_extract(const mw2_u8 *img, mw2_u32 size, const char *name, mw2_u8 *out, mw2_u32 cap);
mw2_u32 mscard_free_blocks(const mw2_u8 *img, mw2_u32 size);          /* from the block map, 0 if the card does not check */
const char *mscard_err(int e);

/* ---- the in-place refresh of MultishineCode / MultishineOvl on a card that already exists (the loader's boot check) -------------------------------------------------------------------- */
/* Only the two build files change; every other file of the card (the loader save, MultishineState, the Melee save, identity, options, ...) is left exactly as it is, and the previous-build copies
   (MultishineCodeB / OvlB) stay too.  The caller works on a copy of the card in memory (the WHOLE card) and writes it only when the result is MSUPD_DONE.
   When the new Code claims a DIFFERENT hole than the card's (the card layout changes: generic vs stock) the loader save SuperSmashBros0110290334 is replaced in the same step, because the exploit save and the engine must agree on the layout; the report says so.
   The build sequence of a card file is read from its MSCF header (block 0, offset 0x1000); the container set was verified as a whole by msota_check_crd. */
#define MSUPD_CODE_ABI 1u
#define MSUPD_OVL_ABI  2u
enum { MSUPD_DONE = 0,       /* replaced: write the card */
       MSUPD_CURRENT,        /* the card has these builds (or newer): nothing to do */
       MSUPD_DEV,            /* the card's file is dev-signed (the watermark): never replaced */
       MSUPD_NOFILES,        /* the card has no MultishineCode / MultishineOvl: this is not a card to refresh */
       MSUPD_BADCARD,        /* the card does not check */
       MSUPD_BADSET,         /* the container's Code / Overlay are not what the loader knows */
       MSUPD_FAIL };         /* removing or adding failed (the image is spoiled: discard it) */
struct msupd_report { mw2_u32 code_old, code_new, ovl_old, ovl_new; mw2_u32 hole_old, hole_new; int newer_on_card; int layout_changed; int err; };
/* free the directory entry and the blocks of the file `name` (GALE); MSCARD_OK, MSCARD_E_MISSING, or a table error */
int mscard_remove(mw2_u8 *img, mw2_u32 size, const char *name);
/* the build sequence in the MSCF header of the card's file `name`: 1 = *seq set (0 when the header is not an MSCF one), 0 = no such file */
int mscard_file_seq(const mw2_u8 *img, mw2_u32 size, const char *name, mw2_u32 *seq, unsigned *abi, int *dev);
/* the RAM hole an MSCF build file claims (header +0x10 base): the card layout (stock / generic / low) is part of the exploit save, the engine is built for it, and the hole base tells them apart; 1 = *base set, 0 = no such file */
int mscard_file_hole(const mw2_u8 *img, mw2_u32 size, const char *name, mw2_u32 *base);
int mscard_update_code(mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set, struct msupd_report *rep);
const char *msupd_text(int r);

/* ---- the loader save (the exploit save SuperSmashBros0110290334) on a card that already exists: ONE rule for the Wii loader and the GameCube shell (52) ------------------------------
   The loader lives only in the exploit save; a card keeps its loader until that file is replaced.  The exploit save is ALSO the file Melee itself saves to:
   on every card in use the game rewrites its directory entry time (0x28..0x2B), block 0's second comment line (the date, 0x20..0x3F) and the 16 bytes at 0x1E40, and blocks 1 and 10 (its own data);
   blocks 2..9 (MSLDR_FIX0..MSLDR_FIX1) never change after the card is made (checked on our cards of 18-20 Nov 2026) and hold the loader.  mscard_update_loader therefore writes ONLY blocks
   2..9 of the container's copy over the blocks the card's copy already owns, in place: the directory entry, the block map, every block position and the game's own bytes stay as they are.
   It does so only when ALL of these hold:
     * the card checks and its loader save has a well formed chain of MSCRD_LOADER_BLOCKS blocks; blocks 2..9 differ from the container's (equal = MSLDR_SAME, nothing written);
     * the card's copy is a RELEASED loader: the CRC-32 of its blocks 2..9 is in `known` (a dev / lab / foreign / damaged loader = MSLDR_UNKNOWN, never touched);
     * the container's copy is of the same kind (our art): directory entry equal apart from the time and the first-block field, block 0 equal apart from the date line and 0x1E40..0x1E5F
       (MSLDR_OTHERART otherwise);
     * the card layout does not change: the card's MultishineCode claims the same RAM hole as the container's (MSLDR_LAYOUT otherwise: a layout migration is mscard_update_code's, which replaces the
       whole exploit save itself together with the engine);
     * the container is newer than the card's loader: set->seq > the first container seq that carried the card's loader, and the container's loader is not a known OLDER one (MSLDR_OLDER).
   The player's Melee save, MultishineState, options and every other file are never touched.  The caller works on a copy of the whole card in memory and writes it (new file, read back, swap,
   old copy kept) only on MSLDR_DONE.  Virtual card FILES only: never a physical card. */
#define MSLDR_FIX0 2u
#define MSLDR_FIX1 9u
struct msldr_known { mw2_u32 crc; mw2_u32 first_seq; };      /* a released loader: CRC-32 of blocks 2..9 of its loader save, the first card container seq that carried it */
#define MSLDR_KNOWN_INIT { { 0x887C30A9u, 1137u },  /* stock layout (crd-1137 .. crd-1281; whole-file SHA-256 228efffc5245dffe...) */ \
                           { 0xA42778CBu, 1283u },  /* generic layout (crd-1283 .. crd-1293; 92b05afb71d87d41...) */ \
                           { 0x0F0A2624u, 1294u } } /* generic + loader mute (crd-1294 ..; 2757e8f0dc2f6f49...) */
#define MSLDR_NKNOWN 3u
enum { MSLDR_DONE = 0, MSLDR_SAME, MSLDR_NOLOADER, MSLDR_UNKNOWN, MSLDR_OTHERART, MSLDR_LAYOUT, MSLDR_OLDER, MSLDR_BADCARD, MSLDR_FAIL };
struct msldr_report { mw2_u32 crc_old, crc_new, first_old, set_seq; int err; };
int mscard_update_loader(mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set, const struct msldr_known *known, unsigned nknown, struct msldr_report *rep);
const char *msldr_text(int r);
/* the whole boot refresh of a virtual card: mscard_update_code, then (when that answered DONE or CURRENT) mscard_update_loader on the same image.  -> MSUPD_DONE when either step changed the
   image (write it), MSUPD_FAIL when either failed (discard the image), otherwise what mscard_update_code answered (ldr_r = -1 when the loader step did not run). */
struct msref_report { struct msupd_report code; struct msldr_report ldr; int code_r, ldr_r; };
int mscard_refresh(mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set, const struct msldr_known *known, unsigned nknown, struct msref_report *rep);

/* ---- rollback of a card LAYOUT migration (the loader's boot refresh, ms_ota_loader.c) -------------------------------------------------------------------------------------------------- */
/* After a layout migration the loader keeps GALE.raw.prev and writes SD:/multishine/crd.pend ("pend <code seq> <starts>").  The engine's own boot record in MultishineState ('MSBT', candidate seq, tries,
   good; written when the build reaches the Main Menu) tells whether the migrated card ever worked.  At each start the loader counts only the starts that LAUNCHED the game (the kernel's RUNID line),
   so opening the app does not count.  A card whose exploit save does not work never reaches the engine, so its record never names the new build: after MSUPD_PEND_MAX such launches the previous card file
   is put back. */
#define MSUPD_PEND_MAX 3u
enum { MSUPD_PEND_WAIT = 0, MSUPD_PEND_CONFIRMED, MSUPD_PEND_ROLLBACK, MSUPD_PEND_REARM };
/* the boot record of MultishineState (block 0 offset 0x1000 + 16): 1 = valid record in *cand / *tries / *good, 0 = no file / no record / bad CRC */
int mscard_boot_record(const mw2_u8 *img, mw2_u32 size, mw2_u32 *cand, unsigned *tries, unsigned *good);
/* "pend <seq> <starts>\n": 1 and the numbers, 0 if the text is not that */
int msupd_pend_parse(const char *txt, mw2_u32 n, mw2_u32 *seq, unsigned *starts);
mw2_u32 msupd_pend_fmt(char *out, mw2_u32 cap, mw2_u32 seq, unsigned starts);
/* seq = the migrated Code build_seq; starts = game launches counted so far (this one included when launched); rec_ok / cand / tries / good = the boot record */
int msupd_pend_decide(mw2_u32 seq, unsigned starts, int rec_ok, mw2_u32 cand, unsigned tries, unsigned good);
/* the updater's bootrec_arm on the card IMAGE: MultishineState +0x1010 = 'MSBT', candidate seq, tries 0, good 0, CRC-32.
   The engine's bootrec_good then marks the build good at its Main Menu; its bootrec_start counts nothing on a card without MultishineCodeB (the crd card has none), so a loader that does not arm
   leaves a record for an older build that never names the migrated one (shells up to 166: every migration was rolled back at the 3rd launch).  1 = armed, 0 = no MultishineState / card does not check */
int mscard_boot_arm(mw2_u8 *img, mw2_u32 size, mw2_u32 seq);
/* the pending rule.  code_seq = the MultishineCode build on the card now (0 = unknown).  The target is code_seq (or seq when unknown): CONFIRMED when the record names the target as
   good; REARM (*target set) when the record does not name the target at all (a pend written by a shell <= 166 that never armed, or a same-layout refresh by such a shell that moved the card on):
   the loader arms the record for the target and starts the count again at 0; otherwise msupd_pend_decide for the target (an armed build that never reaches its Main Menu is still rolled back
   after MSUPD_PEND_MAX launches, or after MSUPD_PEND_MAX engine tries on a card with MultishineCodeB). */
int msupd_pend_step(mw2_u32 seq, unsigned starts, int rec_ok, mw2_u32 cand, unsigned tries, unsigned good, mw2_u32 code_seq, mw2_u32 *target);

#endif
