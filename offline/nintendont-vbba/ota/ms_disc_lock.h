/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The disc lock of the Wii loader: only the stock NTSC-U Melee v1.02 disc image may be started.  Portable C89 (the loader compiles it, tests/disc_test.c
 * compiles it on the host): the caller supplies a read callback, this file never touches a file system.  Identity in two steps: (1) the cheap header check
 * (game ID GALE01, disc 0, revision 02, the DVD magic) that the game list uses; (2) the fingerprint: SHA-256 over sampled windows of the stock image
 * (ms_disc_fp.h: offsets and digests only, no disc bytes). */
#ifndef MS_DISC_LOCK_H
#define MS_DISC_LOCK_H

typedef unsigned char mdl_u8;
typedef unsigned long mdl_u32;

enum { MSDL_OK_STOCK = 1, MSDL_NOT_MELEE = -1, MSDL_OTHER_VERSION = -2, MSDL_MODIFIED = -3, MSDL_READ_ERROR = -4, MSDL_TOO_SMALL = -5 };

/* read len bytes at byte offset off of the image into buf; 0 = ok */
typedef int (*msdl_read_fn)(void *ctx, mdl_u32 off, mdl_u8 *buf, mdl_u32 len);

/* the cheap check on the first 0x20 bytes of an image: 1 = GALE01, disc 0, revision 2, DVD magic 0xC2339F3D at 0x1C.  id6 (7 bytes) gets the printable game ID, *rev the revision. */
int msdl_header_ok(const mdl_u8 *hdr, char *id6, mdl_u8 *rev);
/* what a verdict was based on (for the log and the refusal text) */
struct msdl_info {
    char id[7];                 /* the game ID of the header, printable */
    mdl_u8 rev;                 /* its revision byte */
    mdl_u32 bad_mask;           /* bit i set = common window i (0-based) differs from the stock disc */
    int first_bad;              /* 1-based number of the first differing common window, 0 = none */
};
/* The lock: the header is GALE01 revision 2, EVERY common window matches the stock disc and the digest over the two title/patch-area windows is the stock one.
   MSDL_OK_STOCK = accepted; negative = refused (MSDL_MODIFIED names the first differing window, NCOMMON + 1 for the title/patch-area windows). */
int msdl_verdict_ex(msdl_read_fn rd, void *ctx, mdl_u32 size, struct msdl_info *info);
int msdl_verdict(msdl_read_fn rd, void *ctx, mdl_u32 size);
int msdl_ncommon(void);        /* how many common windows there are (the length of the bad_mask map) */
/* a short plain-language reason class for a refusal ("header does not match", "window 5 differs", ...) */
void msdl_detail(int verdict, const struct msdl_info *info, char *out, mdl_u32 n);
const char *msdl_reason(int verdict);

/* the verdict cache SD:/multishine/disc.cache: text lines "PPPPPPPP SSSSSSSS MMMMMMMM V\n" (FNV-1a of the path, size, mtime, verdict digit 2 accepted / 0 refused; an entry with the older digit 1 is checked again),
 * newest last; msdl_cache_get returns 1 and *v when the exact key is present, msdl_cache_put returns the new length (it drops the oldest lines to stay under cap). */
mdl_u32 msdl_fnv(const char *s);
int msdl_cache_get(const char *buf, mdl_u32 len, mdl_u32 path_hash, mdl_u32 size, mdl_u32 mtime, int *v);
mdl_u32 msdl_cache_put(char *buf, mdl_u32 cap, mdl_u32 len, mdl_u32 path_hash, mdl_u32 size, mdl_u32 mtime, int v);
#define MSDL_CACHE_MAX 1024u

#endif
