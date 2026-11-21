/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Nintendont shell, Memory card creation, the pure part: see ms_pack_core.h.  C89. */
#include <stdio.h>
#include <string.h>
#include "ms_pack_core.h"

static mw2_u32 be32(const mw2_u8 *p) { return ((mw2_u32)p[0] << 24) | ((mw2_u32)p[1] << 16) | ((mw2_u32)p[2] << 8) | (mw2_u32)p[3]; }

/* ---- the container --------------------------------------------------------------------------------------------------------------------------------------------------------- */
int mspk_parse(const mw2_u8 *pl, mw2_u32 n, struct mspk_data *out)
{
    struct mspk_data d;
    mw2_u32 total, k, nent, seen = 0;
    if (n < 32u || pl[0] != 'M' || pl[1] != 'S' || pl[2] != 'P' || pl[3] != 'D' || pl[4] != 1 || (pl[5] != 6 && pl[5] != 7) || pl[6] || pl[7]) return 0;       /* 7 = the optional MnSlMap.usd.diff (id 11) is in it too */
    nent = pl[5];
    total = be32(pl + 8);
    if (total != n || (total & 31u) || 16u + 16u * nent > n) return 0;
    if (msota_crc32(pl + 16, 16u * nent) != be32(pl + 12)) return 0;
    memset(&d, 0, sizeof d);
    for (k = 0; k < nent; k++) {
        const mw2_u8 *e = pl + 16 + 16u * k;
        mw2_u32 id = e[0], off = be32(e + 4), ln = be32(e + 8);
        if (e[1] || e[2] || e[3] || (id < 4u || (id > 9u && id != 11u)) || (seen & (1u << id)) || (off & 31u) || off < 16u + 16u * nent || !ln || off > total || ln > total - off) return 0;
        if (msota_crc32(pl + off, ln) != be32(e + 12)) return 0;
        seen |= 1u << id; d.off[id] = off; d.len[id] = ln;
    }
    if (seen != (nent == 7u ? 0xBF0u : 0x3F0u)) return 0;
    d.pl = pl; d.plen = n;
    *out = d;
    return 1;
}

int msota_check_pack(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct mspk_data *out)
{
    const mw2_u8 *img;
    mw2_u32 seq, ilen;
    struct mspk_data d;
    int r = msota_check_container(file, len, MSPK_MAX_FILE, MSPK_ABI, pubs, npub, min_seq, &img, &ilen, &seq);
    if (r != MSOTA_OK) return r;
    if (!mspk_parse(img, (ilen + 31u) & ~31u, &d)) return MSOTA_E_PAYLOAD;           /* the payload is padded to 32 bytes inside its length */
    d.seq = seq;
    if (out) *out = d;
    return MSOTA_OK;
}

/* ---- the disc ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
int mspk_disc_header_ok(const mw2_u8 *h)
{
    return h[0] == 'G' && h[1] == 'A' && h[2] == 'L' && h[3] == 'E' && h[4] == '0' && h[5] == '1' && h[6] == 0 && h[7] == 2
        && h[0x1C] == 0xC2 && h[0x1D] == 0x33 && h[0x1E] == 0x9F && h[0x1F] == 0x3D;
}

static const char *const WANT[5] = { "MnExtAll.usd", "MnMaAll.usd", "SdMenu.usd", "SdSlChr.usd", "MnSlMap.usd" };      /* the fifth is optional */

int mspk_find_files(const struct mspk_host *h, struct mspk_loc loc[5])
{
    mw2_u8 *fst;
    mw2_u32 fo, fl, count, strs, i, found = 0, k;
    mw2_u8 *a = (mw2_u8 *)h->alloc(h->ctx, 0x440u);
    int rc;
    if (!a) return MSPK_E_MEM;
    if (h->rd(h->ctx, 0, a, 0x440u)) { h->release(h->ctx, a); return MSPK_E_READ; }
    fo = be32(a + 0x424); fl = be32(a + 0x428);
    h->release(h->ctx, a);
    if (fo < 0x440u || fl < 12u || fl > MSPK_FST_MAX || fo > h->disc_size || fl > h->disc_size - fo) return MSPK_E_FST;
    fst = (mw2_u8 *)h->alloc(h->ctx, (fl + 31u) & ~31u);
    if (!fst) return MSPK_E_MEM;
    if (h->rd(h->ctx, fo, fst, fl)) { h->release(h->ctx, fst); return MSPK_E_READ; }
    rc = MSPK_E_FST;
    count = be32(fst + 8);                                                     /* the root entry: the number of entries */
    if (fst[0] != 1 || count < 1u || count > fl / 12u) goto out;
    strs = count * 12u;
    for (k = 0; k < 5; k++) loc[k].off = loc[k].len = 0;
    for (i = 1; i < count;) {
        const mw2_u8 *e = fst + 12u * i;
        if (e[0] == 1) {                                                        /* a directory: its whole subtree is skipped (the four files are in the root) */
            mw2_u32 nx = be32(e + 8);
            if (nx <= i || nx > count) goto out;
            i = nx;
            continue;
        }
        {
            mw2_u32 no = ((mw2_u32)e[1] << 16) | ((mw2_u32)e[2] << 8) | e[3], j;
            if (strs + no < strs || strs + no >= fl) goto out;
            for (k = 0; k < 5; k++) {
                const char *w = WANT[k];
                for (j = 0; w[j] && strs + no + j < fl && fst[strs + no + j] == (mw2_u8)w[j]; j++) {}
                if (!w[j] && strs + no + j < fl && fst[strs + no + j] == 0) {
                    mw2_u32 o = be32(e + 4), l = be32(e + 8);
                    if (!l || o > h->disc_size || l > h->disc_size - o) goto out;
                    loc[k].off = o; loc[k].len = l; found |= 1u << k;
                }
            }
        }
        i++;
    }
    rc = (found & 0xFu) == 0xFu ? MSPK_OK : MSPK_E_NOFILES;
out:
    h->release(h->ctx, fst);
    return rc;
}

/* ---- the build --------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static const slp_u8 *io_map(void *ctx, int id)
{
    struct mspk_build *b = (struct mspk_build *)ctx;
    return ((id >= 4 && id < 10) || id == 11) && b->pk->len[id] ? b->pk->pl + b->pk->off[id] : 0;
}

static int io_size(void *ctx, int id)
{
    struct mspk_build *b = (struct mspk_build *)ctx;
    if (id >= 0 && id < 4) return b->loc[id].len ? (int)b->loc[id].len : -1;
    if (id == 10) return b->loc[4].len ? (int)b->loc[4].len : -1;
    if (((id >= 4 && id < 10) || id == 11) && b->pk->len[id]) return (int)b->pk->len[id];
    return -1;
}

static int io_read(void *ctx, int id, slp_u8 *dst, slp_u32 off, slp_u32 len)
{
    struct mspk_build *b = (struct mspk_build *)ctx;
    slp_u32 n, i, rl;
    if ((((unsigned long)dst) & 31u) || (off & 31u)) return -1;
    rl = (len + 31u) & ~31u;
    if ((id >= 0 && id < 4) || id == 10) {                                      /* the player's disc (10 = MnSlMap.usd, loc[4]) */
        const struct mspk_loc *lc = &b->loc[id == 10 ? 4 : id];
        if (off > lc->len) return -1;
        n = lc->len - off;
        if (n > len) n = len;
        if (n && b->h->rd(b->h->ctx, (mw2_u32)(lc->off + off), dst, n)) return -1;
    } else {
        if (id < 4 || !((id < 10) || id == 11) || !b->pk->len[id] || off > b->pk->len[id]) return -1;
        n = (slp_u32)b->pk->len[id] - off;
        if (n > len) n = len;
        memcpy(dst, b->pk->pl + b->pk->off[id] + off, n);
    }
    for (i = n; i < rl; i++) dst[i] = 0;
    return 0;
}

static void *io_alloc(void *ctx, slp_u32 n) { struct mspk_build *b = (struct mspk_build *)ctx; return b->h->alloc(b->h->ctx, n); }
static void io_release(void *ctx, void *p) { struct mspk_build *b = (struct mspk_build *)ctx; b->h->release(b->h->ctx, p); }

int mspk_begin(struct mspk_build *b, const struct mspk_host *h, const struct mspk_data *pk)
{
    int r;
    memset(b, 0, sizeof *b);
    b->h = h; b->pk = pk;
    r = mspk_find_files(h, b->loc);
    if (r == MSPK_E_NOFILES) return MSPK_E_NODISC;
    if (r != MSPK_OK) return r;
    b->io.io.ctx = b; b->io.io.size = io_size; b->io.io.read = io_read; b->io.io.alloc = io_alloc; b->io.io.release = io_release; b->io.map = io_map;
    if (!slpl_disc_has(&b->io.io)) return MSPK_E_NODISC;                        /* a size is not the stock one (another version of the disc) */
    slpl_init(&b->sl, &b->io);
    b->begun = 1;
    return MSPK_OK;
}

int mspk_step(struct mspk_build *b, mw2_u32 budget, unsigned *pct)
{
    int r;
    if (!b->begun) return MSPK_E_BUILD;
    if (b->done) { *pct = 100; return 1; }
    r = slpl_step(&b->sl, (slp_u32)budget);
    *pct = slpl_percent(&b->sl);
    if (r == SLP_RUN) return 0;
    if (r == SLP_DONE) {
        const mw2_u8 *g = slpl_gci_hdr(&b->sl);
        mw2_u32 n = 0x40u + (mw2_u32)slpl_gci_blocks(&b->sl) * 0x2000u;
        if (slpl_gci_data(&b->sl) != g + 0x40 || !mspk_pack_valid(g, n)) return MSPK_E_BUILD;
        b->done = 1; *pct = 100;
        return 1;
    }
    return r == SLP_E_MEM ? MSPK_E_MEM : r == SLP_E_READ ? MSPK_E_READ : r == SLP_E_INPUT ? MSPK_E_INPUT : MSPK_E_BUILD;
}

const mw2_u8 *mspk_gci(const struct mspk_build *b, mw2_u32 *len)
{
    if (!b->done) return 0;
    *len = 0x40u + (mw2_u32)slpl_gci_blocks(&b->sl) * 0x2000u;
    return slpl_gci_hdr(&b->sl);
}

void mspk_end(struct mspk_build *b)
{
    if (b->begun) slpl_free(&b->sl);
    b->begun = b->done = 0;
}

/* ---- the card ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
int mspk_pack_valid(const mw2_u8 *g, mw2_u32 len)
{
    const mw2_u8 *b0, *toc;
    mw2_u32 blocks, cnt, total, k, off[10], ln[10], crc[10];
    char name[33];
    if (len < 0x40u + 0x2000u || ((len - 0x40u) & 0x1FFFu)) return 0;
    if (memcmp(g, "GALE", 4) != 0 || memcmp(g + 4, "01", 2) != 0) return 0;
    memcpy(name, g + 8, 32); name[32] = 0;
    if (strcmp(name, MSPK_NAME) != 0) return 0;
    blocks = ((mw2_u32)g[0x38] << 8) | g[0x39];
    if (blocks < 2u || blocks > MSPK_MAX_PACK_BLOCKS || blocks * 0x2000u != len - 0x40u) return 0;
    b0 = g + 0x40; toc = b0 + 0x1020; total = blocks * 0x2000u;
    if (be32(b0 + 0x1000) != 0x4D53504Bu || be32(b0 + 0x1004) != 1u) return 0;            /* 'MSPK', version 1 */
    cnt = be32(b0 + 0x1008);
    if (cnt < 1u || cnt > 10u || be32(b0 + 0x100C) != total) return 0;
    if ((mw2_u32)slp_crc32(0, toc, (slp_u32)(cnt * 64u)) != be32(b0 + 0x1010)) return 0;
    for (k = 0; k < cnt; k++) {
        const mw2_u8 *e = toc + 64u * k;
        off[k] = be32(e + 40); ln[k] = be32(e + 36); crc[k] = be32(e + 60);
        if (off[k] < 0x2000u || (off[k] & 0x1FFFu) || off[k] > total || ln[k] > total - off[k]) return 0;
    }
    for (k = 0; k < cnt; k++)
        if (crc[k] && (mw2_u32)slp_crc32(0, b0 + off[k], (slp_u32)ln[k]) != crc[k]) return 0;
    return 1;
}

int mspk_card_add(mw2_u8 *raw, mw2_u32 size, const mw2_u8 *gci, mw2_u32 len)
{
    int r;
    if (!mspk_pack_valid(gci, len)) return MSPK_ADD_BAD;
    if (mscard_free_blocks(raw, size) < (len - 0x40u) / 0x2000u) return MSPK_ADD_NOSPACE;
    r = mscard_add(raw, size, gci, len);
    if (r == MSCARD_E_NOSPACE || r == MSCARD_E_FULL) return MSPK_ADD_NOSPACE;
    if (r != MSCARD_OK) return MSPK_ADD_FAILED;
    if (mscard_has_gci(raw, size, gci, len) != MSCARD_OK) return MSPK_ADD_FAILED;
    return MSPK_ADD_OK;
}

int mspk_card_extract(const mw2_u8 *raw, mw2_u32 size, mw2_u8 *out, mw2_u32 cap, mw2_u32 *len)
{
    mw2_u32 n;
    if (mscard_check(raw, size) != MSCARD_OK) return 0;
    n = mscard_extract(raw, size, MSPK_NAME, out, cap);
    if (!n || !mspk_pack_valid(out, n)) return 0;
    *len = n;
    return 1;
}

/* ---- which image is the disc ---------------------------------------------------------------------------------------------------------------------------------------------------- */
int mspk_pick_discs(mspk_list_fn list, mspk_check_fn check, void *ctx, char *path, unsigned plen, unsigned max, unsigned *seen)
{
    char tmp[300];
    unsigned i, n = 0, total = 0;
    unsigned char taken[MSPK_MAX_CAND];
    int pass;
    memset(taken, 0, sizeof taken);
    while (total < MSPK_MAX_CAND && list(ctx, total, tmp, sizeof tmp)) total++;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < total && n < max; i++) {
            if (taken[i] || !list(ctx, i, tmp, sizeof tmp)) continue;
            if (check(ctx, tmp, pass == 0) == 1 && strlen(tmp) < plen) {
                strcpy(path + (size_t)n * plen, tmp);
                taken[i] = 1;
                n++;
            }
        }
    }
    if (seen) *seen = total;
    return (int)n;
}

void mspk_none_text(char *out, unsigned n, unsigned seen)
{
    if (seen == 1) snprintf(out, n, "Found 1 image: it is not stock Melee v1.02.");
    else snprintf(out, n, "Found %u images: none is stock Melee v1.02.", seen);
}

/* ---- the words ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static const char *const T1[MSPK_O_COUNT] = {
    "The Multishine system files are on the card.",
    "The Multishine system files of your old card are kept.",
    "No Melee disc image found in SD:/games.",
    "The disc in SD:/games is not the original Melee v1.02.",
    "Could not download the Multishine system files.",
    "Could not build the Multishine system files.",
    "No room for the Multishine system files on this card.",
    "The Melee disc image could not be read.",
};
static const char *const T2 = "They are built when you first start online play.";
const char *mspk_text(int o) { return o >= 0 && o < MSPK_O_COUNT ? T1[o] : T1[MSPK_O_BUILDFAIL]; }
const char *mspk_text2(int o) { return o == MSPK_O_BUILT || o == MSPK_O_KEPT ? "" : T2; }
int mspk_outcome_ok(int o) { return o == MSPK_O_BUILT || o == MSPK_O_KEPT; }
int mspk_outcome_of_build(int rc)
{
    switch (rc) {
    case MSPK_E_NODISC: case MSPK_E_INPUT: return MSPK_O_BADDISC;
    case MSPK_E_READ: case MSPK_E_FST: case MSPK_E_DISCHDR: return MSPK_O_DISCREAD;
    default: return MSPK_O_BUILDFAIL;
    }
}
