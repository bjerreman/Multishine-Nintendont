/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* seed_lean: the MEMORY-LEAN builder of the stock MultishinePack.  Same inputs and the same pack format as seed_slp.c / the project's other tooling, but the heap
 * peak is about 0.5 MB instead of 8.6 MiB, so it runs at the Main Menu where the HSD heap has no big block.  It is NOT byte-identical to the project's other tooling (a greedy MSLZ with a 64 KiB window instead of the
 * optimal parse; the delta streams come straight from Slippi's VCDIFF instructions instead of a match search); it decodes (df_produce) to exactly the same files (CRC-32 checked on the console by the
 * runtime, SHA-1 compared in the host tests).  Two paths:
 *   MnExtAll.usd (in-place delta) and MnMaAll.usd (delta): the retail file is only CRC-checked (read in chunks, never held); Slippi's VCDIFF instructions (COPY from the source / ADD / RUN / COPY from the target)
 *     are turned into the pack's own op stream; a COPY from the target is resolved to the retail ranges / literals it was made of.
 *   SdMenu.usd, SdSlChr.usd (small): decoded whole (VCDIFF, the item-name fix), the op stream from the system-files builder's match search (slp_md_*), as before.
 *   slpCSS.dat: the whole file, MSLZ.
 * Every op stream / file is compressed by the lean MSLZ below (hash chains, lazy matching, window 64 KiB - 1) and checked by a streaming decode against its source.
 * Pure C89, no libc, sliced like slp_step (one call = a bounded amount of work). */
#ifndef SEED_LEAN_H
#define SEED_LEAN_H

#include "seed_slp.h"

struct slpl_io {
    struct slp_io io;                                                           /* ids 0..3 the retail files (read in chunks), 4..9 as for slp_io */
    const slp_u8 *(*map)(void *ctx, int id);                                    /* ids 4..9 and 11 (MnSlMap.usd.diff): the whole file already in memory (the staging buffer), 0 = not available -> the build fails with SLP_E_READ */
};

struct slpl_piece { slp_u32 pos, a, lk; };         /* an output range: pos = start in the new file, a = retail offset (S) / diff offset of the literal bytes (L) / byte (R), lk = len | kind << 28 */

struct slpl_lz {
    const slp_u8 *d;                    /* the input */
    slp_u32 *head;                      /* 1 << 13 entries: position + 1 */
    unsigned short *prev;               /* 32 Ki ring: distance to the previous position of the same hash (0 = none) */
    slp_u8 *out;
    slp_u32 n, ocap, o, i, anchor, used;
};

struct slpl {
    const struct slpl_io *io;
    int ph;
    unsigned k, ord;                    /* entry being built, its place in the build order */
    slp_u32 pos, crc;
    slp_u8 *chunk, *old, *nw, *dout;
    const slp_u8 *dl, *src;             /* the diff (in memory), the bytes the MSLZ compresses (the op stream, or the whole slpCSS.dat) */
    slp_u32 olen, nlen, dllen, doutlen, doutcap, inst_len;
    struct slpl_piece *pc;
    slp_u32 npc, cpc;
    struct slp_md md;
    struct slpl_lz z;
    slp_u8 *zbuf, *zout;
    struct slp_ent e[SLP_NENT_MAX];
    unsigned n;                         /* entries of the pack: SLP_NENT (5), or SLP_NENT_MAX (6) with the optional MnSlMap.usd */
    slp_u8 *gci_raw, *gci_data, *head;
    slp_u32 gci_blocks;
    int err;
    unsigned detail;
    slp_u32 peak, cur, work;
};

void slpl_init(struct slpl *s, const struct slpl_io *io);
int slpl_step(struct slpl *s, slp_u32 budget);                  /* SLP_RUN, SLP_DONE or a negative SLP_E_* */
unsigned slpl_percent(const struct slpl *s);
const slp_u8 *slpl_gci_hdr(const struct slpl *s);
const slp_u8 *slpl_gci_data(const struct slpl *s);
slp_u32 slpl_gci_blocks(const struct slpl *s);
void slpl_free(struct slpl *s);
int slpl_disc_has(const struct slp_io *io);                     /* as slp_disc_has */
int slpl_has_slmap(const struct slp_io *io);                    /* 1 = the optional 6th entry (MnSlMap.usd) can be built: retail file of the stock size + Slippi's diff, ids 10 / 11 */

/* the lean MSLZ alone (host tests) */
slp_u32 slpl_lz_mem(void);                                      /* bytes of work area */
void slpl_lz_init(struct slpl_lz *z, const slp_u8 *d, slp_u32 n, void *mem, slp_u8 *out, slp_u32 ocap);
int slpl_lz_step(struct slpl_lz *z, slp_u32 budget);            /* 1 done (z->o = stream length), 0 more, -1 output full */
int slpl_lz_check(const slp_u8 *z, slp_u32 zn, const slp_u8 *d, slp_u32 n);       /* 0 = the stream decodes to exactly d (streaming compare, no buffer) */

#endif
