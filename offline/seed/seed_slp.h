/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* seed_slp: the system-files builder builds the STOCK MultishinePack ON THE CONSOLE, from (a) the player's own retail disc files (MnExtAll.usd MnMaAll.usd SdMenu.usd SdSlChr.usd)
 * and (b) Slippi's public GPL files that the modified image carries unmodified (/ms/slp/: the four *.usd.diff VCDIFF deltas against retail and slpCSS.dat whole).  The result is byte-identical to what
 * the project's other tooling makes on the PC for the same inputs: the same VCDIFF decode, the same SdMenu item-name fix,
 * the same mdelta op stream, the same optimal-parse MSLZ (tools/payload_pack/mslz_opt.c), the same entry table and header.
 * Pure C, no libc, no globals except two constant tables built on first use; the host provides disc reads and memory (struct slp_io).  The work is sliced: slp_step does a bounded amount of
 * work per call (a frame), so the system-files builder's screen keeps moving.  Nothing Nintendo- or Slippi-derived is stored by this code: the buffers live in the heap and end up on the player's own card. */
#ifndef SEED_SLP_H
#define SEED_SLP_H

typedef unsigned char slp_u8;
typedef unsigned int slp_u32;
#ifdef __GNUC__
__extension__ typedef long long slp_s64;     /* C89 -pedantic hosts: the one 64-bit type */
#else
typedef long long slp_s64;
#endif

#define SLP_NENT 5u                   /* the stock pack's entries (table rows 0..4) */
#define SLP_NENT_MAX 6u               /* + the optional 6th, MnSlMap.usd (row 5): SLP_RETAIL_SLMAP / SLP_DIFF_SLMAP below; the lean builder takes it when both are on the disc */
#define SLP_PACK_BLOCKS 29u           /* the stock pack: what the room check plans for */

/* disc file ids of struct slp_io */
enum { SLP_RETAIL = 0 /* 0..3: MnExtAll.usd MnMaAll.usd SdMenu.usd SdSlChr.usd on the player's disc */, SLP_DIFF = 4 /* 4..7: /ms/slp/<name>.diff */, SLP_WHOLE = 8 /* slpCSS.dat */, SLP_HEAD = 9 /* head.bin: GCI entry + header block of the pack */,
       SLP_RETAIL_SLMAP = 10 /* MnSlMap.usd on the player's disc (optional entry) */, SLP_DIFF_SLMAP = 11 /* /ms/slp/MnSlMap.usd.diff */ };

struct slp_io {
    void *ctx;
    int (*size)(void *ctx, int id);                                              /* file size in bytes, -1 = missing */
    int (*read)(void *ctx, int id, slp_u8 *dst, slp_u32 off, slp_u32 len);       /* off and dst multiples of 32, len up to the next multiple of 32 past the end is allowed (dst has room); 0 = ok */
    void *(*alloc)(void *ctx, slp_u32 n);                                        /* 32-byte aligned block, 0 = none */
    void (*release)(void *ctx, void *p);
};

/* results of slp_step */
enum { SLP_RUN = 0, SLP_DONE = 1, SLP_E_MEM = -1, SLP_E_READ = -2, SLP_E_INPUT = -3 /* a size or CRC that is not the stock one: another disc, a changed Slippi file */, SLP_E_VCDIFF = -4,
       SLP_E_DELTA = -5, SLP_E_MSLZ = -6, SLP_E_PACK = -7 };

/* the mdelta encoder in slices */
struct slp_md {
    const slp_u8 *old, *nw;
    int olen, nlen, W, STEP, MINC;
    int *start, *spos;                    /* counting-sorted index: bucket b holds spos[start[b + 1] .. start[b + 2]) in ascending order */
    slp_u8 *occ;                          /* bitmap of the non-empty buckets */
    int nb, bits, ne, bi;                 /* buckets (a power of two), their bits, indexed positions, index build position */
    slp_u32 mpow, hv;                     /* HM^(W-1), the rolling hash of the window at hi */
    int hi;
    slp_u8 *out;
    int oc, ocap;
    int i, pend, prev_end, expo;
    int phase;                            /* 0 count, 1 fill, 2 scan, 3 done */
    slp_u32 used;                         /* work units the last slp_md_step did */
};

/* the optimal-parse MSLZ encoder (tools/payload_pack/mslz_opt.c) in slices */
struct slp_z {
    const slp_u8 *d;
    int n, depth, ph;
    slp_s64 cap;
    int *A;                               /* phase 1: head[1<<20] + prev[n+1]; phase 2: S Sm M ML */
    int *head, *prev, *Lmax, *S, *Sm, *M, *ML;
    unsigned short *Off;
    slp_u8 *Uf;
    int i, j;
    slp_s64 rbest[255];
    int rk[255];
    slp_u8 *out;
    int outlen;
    slp_u32 used;                         /* work units the last slp_z_step did */
};

struct slp_ent {                          /* one pack entry, in TOC order */
    slp_u8 *stored;                       /* the stored bytes (MSLZ stream) */
    slp_u32 mode, new_size, old_size, stored_len, data_off, gap, crc_new, crc_old, p_pos, p_old;
};

struct slp {
    const struct slp_io *io;
    int ph;                               /* phase inside the entry */
    unsigned k;                           /* entry being built */
    slp_u32 pos;                          /* bytes read so far (disc reads) */
    slp_u8 *old, *nw, *dl, *dout;         /* retail file, new file, VCDIFF delta, mdelta stream */
    slp_u32 olen, nlen, dllen, doutlen;
    struct slp_md md;
    struct slp_z z;
    slp_u8 *zbuf;                         /* the work areas of the MSLZ encoder */
    int *zA;
    slp_u8 *zout;
    struct slp_ent e[SLP_NENT];
    slp_u8 *gci;                          /* 0x40 dir entry + the pack file (32-byte aligned part starts at gci_data) */
    slp_u8 *gci_raw, *gci_data;
    slp_u32 gci_blocks;
    int err;                              /* SLP_E_* of the failure, 0 none */
    unsigned detail;                      /* k << 8 | phase of the failure */
    slp_u32 peak, cur;                    /* heap accounting (bytes) */
    slp_u8 *head;                         /* head.bin */
    slp_u32 work;                         /* progress units done */
};

void slp_init(struct slp *s, const struct slp_io *io);
/* does at most `budget` units of work (about 100 CPU cycles each; 100000 = a few tens of ms on the console).  SLP_RUN, SLP_DONE or a negative SLP_E_*; after SLP_DONE slp_gci is valid */
int slp_step(struct slp *s, slp_u32 budget);
unsigned slp_percent(const struct slp *s);           /* 0..100 */
/* the finished card file: hdr = the 0x40 byte GCI directory entry, data = blocks * 0x2000 bytes (32-byte aligned) */
const slp_u8 *slp_gci_hdr(const struct slp *s);
const slp_u8 *slp_gci_data(const struct slp *s);
slp_u32 slp_gci_blocks(const struct slp *s);
/* gives back everything the builder holds (including the finished file) */
void slp_free(struct slp *s);
/* 1 when the disc carries every Slippi file (sizes only, no reads) */
int slp_disc_has(const struct slp_io *io);

/* building blocks, also used by the tests */
slp_u32 slp_crc32(slp_u32 crc, const slp_u8 *p, slp_u32 n);
int slp_vcdiff(const slp_u8 *src, slp_u32 srclen, const slp_u8 *dl, slp_u32 dllen, slp_u8 *out, slp_u32 outcap, slp_u32 *outlen);
int slp_fix_item_names(const slp_u8 *src, slp_u32 srclen, slp_u8 *res, slp_u32 reslen);     /* 1 = the one byte was restored */
int slp_mslz_gap(const slp_u8 *z, slp_u32 zn, slp_u32 out_len);                              /* mslz.inplace_gap, -1 on a malformed stream */
int slp_delta_apply(const slp_u8 *old, slp_u32 olen, const slp_u8 *d, slp_u32 dn, slp_u8 *out, slp_u32 nsize);   /* 0 ok */
int slp_delta_crc(const slp_u8 *old, slp_u32 olen, const slp_u8 *d, slp_u32 dn, slp_u32 nsize, slp_u32 *crc);   /* CRC-32 of what the op stream produces, without building it */
int slp_delta_inplace_ok(const slp_u8 *d, slp_u32 dn, slp_u32 oldlen, slp_u32 new_size);
slp_u32 slp_md_mem(int olen, int W, int STEP);                                               /* bytes of index memory slp_md_init needs */
void slp_md_init(struct slp_md *m, const slp_u8 *old, int olen, const slp_u8 *nw, int nlen, int W, int STEP, int MINC, void *mem, slp_u8 *out, int ocap);
int slp_md_count(int olen, int W, int STEP);                                                 /* indexed positions */
int slp_md_step(struct slp_md *m, slp_u32 budget);                                           /* 1 done, 0 more, -1 output full */
slp_u32 slp_z_mem(int n);                                                                    /* bytes of work area slp_z_init needs */
void slp_z_init(struct slp_z *z, const slp_u8 *d, int n, slp_s64 cap, void *mem, slp_u8 *out);
int slp_z_step(struct slp_z *z, slp_u32 budget);                                             /* 1 done (z->outlen), 0 more, 3 no stream within cap */

#endif
