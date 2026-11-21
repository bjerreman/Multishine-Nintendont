/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Nintendont shell, Memory card option, the pure part: see ms_card_core.h.  C89. */
#include <string.h>
#include "ms_card_core.h"

static mw2_u32 be32(const mw2_u8 *p) { return ((mw2_u32)p[0] << 24) | ((mw2_u32)p[1] << 16) | ((mw2_u32)p[2] << 8) | (mw2_u32)p[3]; }
static unsigned be16(const mw2_u8 *p) { return ((unsigned)p[0] << 8) | (unsigned)p[1]; }
static void put16(mw2_u8 *p, unsigned v) { p[0] = (mw2_u8)(v >> 8); p[1] = (mw2_u8)v; }

static const char *const NAMES[MSCRD_NFILES] = { "SuperSmashBros0110290334", "MultishineCode", "MultishineOvl", "MultishineState", "MultishineMelee" };
const char *const *mscrd_names(void) { return NAMES; }

/* ---- the container -------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static int file_info(const mw2_u8 *g, mw2_u32 len, struct mscrd_file *f)
{
    unsigned i, k, blocks;
    char name[33];
    if (len < MSCRD_GCI_HDR + MSCARD_BLOCK || ((len - MSCRD_GCI_HDR) & (MSCARD_BLOCK - 1))) return 0;
    if (memcmp(g, "GALE", 4) != 0 || memcmp(g + 4, "01", 2) != 0) return 0;
    for (k = 0; k < 32 && g[8 + k]; k++) name[k] = (char)g[8 + k];
    name[k] = 0;
    for (i = k; i < 32; i++) if (g[8 + i]) return 0;                         /* the name field is NUL padded */
    blocks = be16(g + 0x38);
    if (blocks == 0 || (mw2_u32)blocks * MSCARD_BLOCK != len - MSCRD_GCI_HDR) return 0;
    for (i = 0; i < MSCRD_NFILES; i++) if (strcmp(name, NAMES[i]) == 0) break;
    if (i == MSCRD_NFILES) return 0;
    if (i == 0 && blocks != MSCRD_LOADER_BLOCKS) return 0;
    if (i == 4 && blocks != MSCRD_MELEE_BLOCKS) return 0;
    f->gci = g; f->len = len; f->blocks = blocks;
    memcpy(f->name, name, k + 1);
    return 1;
}

int mscrd_parse(const mw2_u8 *p, mw2_u32 n, struct mscrd_set *out)
{
    struct mscrd_set s;
    mw2_u32 off = 32, i, j, cnt;
    if (n < 33u || n > MSCRD_MAX_PAYLOAD || memcmp(p, "MCRD", 4) != 0 || p[4] != 1 || p[5] != 0) return 0;
    cnt = (mw2_u32)be16(p + 6);
    if (cnt != MSCRD_NFILES || be32(p + 8) != n || be32(p + 12) || be32(p + 20) > MSCRD_SHELL_ABI || be32(p + 24) || be32(p + 28)) return 0;
    if (msota_crc32(p + 32, n - 32u) != be32(p + 16)) return 0;
    memset(&s, 0, sizeof s);
    for (i = 0; i < cnt; i++) {
        mw2_u32 l;
        struct mscrd_file f;
        if (off + 4u > n) return 0;
        l = be32(p + off); off += 4u;
        if (l > n - off) return 0;
        memset(&f, 0, sizeof f);
        if (!file_info(p + off, l, &f)) return 0;
        for (j = 0; j < i; j++) if (strcmp(s.f[j].name, f.name) == 0) return 0;      /* each name once */
        s.f[i] = f; s.blocks += f.blocks;
        off += l;
    }
    if (off != n || s.blocks > MSCRD_MAX_BLOCKS) return 0;
    for (i = 0; i < MSCRD_NFILES; i++) {                                              /* the set is in the order of NAMES */
        struct mscrd_file t = s.f[i];
        for (j = i; j < MSCRD_NFILES; j++) if (strcmp(s.f[j].name, NAMES[i]) == 0) break;
        if (j == MSCRD_NFILES) return 0;
        s.f[i] = s.f[j]; s.f[j] = t;
    }
    *out = s;
    return 1;
}

int msota_check_crd(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct mscrd_set *out)
{
    const mw2_u8 *img;
    mw2_u32 seq, ilen;
    struct mscrd_set s;
    int r = msota_check_container(file, len, MSCRD_MAX_FILE, MSCRD_ABI, pubs, npub, min_seq, &img, &ilen, &seq);
    if (r != MSOTA_OK) return r;
    if (ilen < 33u || memcmp(img, "MCRD", 4) != 0) return MSOTA_E_PAYLOAD;
    if (!mscrd_parse(img, ilen, &s)) return MSOTA_E_PAYLOAD;
    s.seq = seq;
    if (be32(img + 20) > MSCRD_SHELL_ABI) return MSOTA_E_SHELL;
    if (out) *out = s;
    return MSOTA_OK;
}

/* ---- the raw card --------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static void csum(const mw2_u8 *d, mw2_u32 n, unsigned *c1, unsigned *c2)
{
    mw2_u32 i;
    unsigned a = 0, b = 0, w;
    for (i = 0; i + 1 < n; i += 2) {
        w = ((unsigned)d[i] << 8) | d[i + 1];
        a = (a + w) & 0xFFFFu;
        b = (b + (w ^ 0xFFFFu)) & 0xFFFFu;
    }
    *c1 = a == 0xFFFFu ? 0 : a;
    *c2 = b == 0xFFFFu ? 0 : b;
}
static int sum_ok(const mw2_u8 *d, mw2_u32 at)          /* the checksum pair stored at `at` covers d[0..at) (header, directory) */
{
    unsigned c1, c2;
    csum(d, at, &c1, &c2);
    return be16(d + at) == c1 && be16(d + at + 2) == c2;
}
static int bat_ok(const mw2_u8 *b)
{
    unsigned c1, c2;
    csum(b + 4, MSCARD_BLOCK - 4u, &c1, &c2);
    return be16(b) == c1 && be16(b + 2) == c2;
}
static void seal_dir(mw2_u8 *d) { unsigned c1, c2; csum(d, 0x1FFCu, &c1, &c2); put16(d + 0x1FFC, c1); put16(d + 0x1FFE, c2); }
static void seal_bat(mw2_u8 *b) { unsigned c1, c2; csum(b + 4, MSCARD_BLOCK - 4u, &c1, &c2); put16(b, c1); put16(b + 2, c2); }

static int size_ok(mw2_u32 n) { return n >= 0x80000u && n <= 0x1000000u && (n & (n - 1u)) == 0; }

/* format an empty card of `size` bytes, holding only the first head_cap bytes in memory: the tables are in blocks 0..4 and the files are added first-fit from block 5, so everything the card contains
   below `size` that is not zero lives in the first head_cap bytes; the caller writes those and then (size - head_cap) zero bytes.  head_cap = size gives the whole card. */
int mscard_format_head(mw2_u8 *img, mw2_u32 head_cap, mw2_u32 size)
{
    mw2_u32 total;
    unsigned c1, c2;
    mw2_u8 *h, *d, *b;
    if (!size_ok(size)) return MSCARD_E_SIZE;
    if (head_cap > size || head_cap < MSCARD_SYS * MSCARD_BLOCK) return MSCARD_E_ARG;
    total = size / MSCARD_BLOCK;
    memset(img, 0, head_cap);
    h = img; d = img + MSCARD_BLOCK; b = img + 3u * MSCARD_BLOCK;
    memset(h, 0xFF, MSCARD_BLOCK);
    memset(h, 0, 12); memset(h + 0x0C, 0, 8); memset(h + 0x14, 0, 12);       /* serial 0, format time 0, sram bias / language / unknown 0, like vmc_card.format_card */
    put16(h + 0x20, 0); put16(h + 0x22, (unsigned)(size / 131072u)); put16(h + 0x24, 0);   /* device id 0, size in Mbit, ANSI */
    csum(h, 0x1FCu, &c1, &c2); put16(h + 0x1FC, c1); put16(h + 0x1FE, c2);
    memset(d, 0xFF, MSCARD_BLOCK); put16(d + 0x1FFA, 0); seal_dir(d);
    memcpy(d + MSCARD_BLOCK, d, MSCARD_BLOCK);
    memset(b, 0, MSCARD_BLOCK);
    put16(b + 4, 0); put16(b + 6, (unsigned)(total - MSCARD_SYS)); put16(b + 8, MSCARD_SYS - 1u);
    seal_bat(b);
    memcpy(b + MSCARD_BLOCK, b, MSCARD_BLOCK);
    return MSCARD_OK;
}

int mscard_format(mw2_u8 *img, mw2_u32 size) { return mscard_format_head(img, size, size); }

/* the newer valid copy of a table: first = block of copy 1; -1 = none valid */
static int pick(const mw2_u8 *img, unsigned first, int is_bat)
{
    int best = -1;
    unsigned bc = 0, i, c;
    for (i = 0; i < 2; i++) {
        const mw2_u8 *t = img + (size_t)(first + i) * MSCARD_BLOCK;
        if (is_bat ? !bat_ok(t) : !sum_ok(t, 0x1FFCu)) continue;
        c = is_bat ? be16(t + 4) : be16(t + 0x1FFA);
        if (best < 0 || c >= bc) { best = (int)(first + i); bc = c; }
    }
    return best;
}

static int geometry(const mw2_u8 *img, mw2_u32 size, int *dir, int *bat)
{
    if (!size_ok(size)) return MSCARD_E_SIZE;
    if (!sum_ok(img, 0x1FCu)) return MSCARD_E_HDR;
    if ((mw2_u32)be16(img + 0x22) * 131072u != size) return MSCARD_E_HDR;
    *dir = pick(img, 1, 0); if (*dir < 0) return MSCARD_E_DIR;
    *bat = pick(img, 3, 1); if (*bat < 0) return MSCARD_E_BAT;
    return MSCARD_OK;
}

static unsigned bat_get(const mw2_u8 *bat, mw2_u32 blk) { return be16(bat + 10 + 2u * (blk - MSCARD_SYS)); }
static int entry_free(const mw2_u8 *e) { return e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF; }

int mscard_add(mw2_u8 *img, mw2_u32 size, const mw2_u8 *gci, mw2_u32 len)
{
    int di, bi, r, slot = -1;
    mw2_u8 *d, *b, *e;
    mw2_u32 total = size / MSCARD_BLOCK, n, blk, found = 0, chain[MSCRD_MAX_BLOCKS], i, free_n = 0;
    unsigned c;
    r = geometry(img, size, &di, &bi);
    if (r != MSCARD_OK) return r;
    if (len < MSCRD_GCI_HDR + MSCARD_BLOCK || ((len - MSCRD_GCI_HDR) & (MSCARD_BLOCK - 1))) return MSCARD_E_ARG;
    n = (len - MSCRD_GCI_HDR) / MSCARD_BLOCK;
    if (n > MSCRD_MAX_BLOCKS) return MSCARD_E_ARG;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    for (i = 0; i < 127; i++) {
        e = d + i * 0x40u;
        if (entry_free(e)) { if (slot < 0) slot = (int)i; }
        else if (memcmp(e, gci, 6) == 0 && memcmp(e + 8, gci + 8, 32) == 0) return MSCARD_E_EXISTS;
    }
    if (slot < 0) return MSCARD_E_FULL;
    for (blk = MSCARD_SYS; blk < total; blk++)
        if (bat_get(b, blk) == 0) { free_n++; if (found < n) chain[found++] = blk; }
    if (found < n) return MSCARD_E_NOSPACE;
    for (i = 0; i < n; i++) {
        put16(b + 10 + 2u * (chain[i] - MSCARD_SYS), i + 1 < n ? (unsigned)chain[i + 1] : 0xFFFFu);
        memcpy(img + (size_t)chain[i] * MSCARD_BLOCK, gci + MSCRD_GCI_HDR + (size_t)i * MSCARD_BLOCK, MSCARD_BLOCK);
    }
    put16(b + 6, be16(b + 6) - (unsigned)n);
    put16(b + 8, (unsigned)chain[n - 1]);
    e = d + (unsigned)slot * 0x40u;
    memcpy(e, gci, MSCRD_GCI_HDR);
    put16(e + 0x36, (unsigned)chain[0]); put16(e + 0x38, (unsigned)n);
    c = (be16(d + 0x1FFA) + 1u) & 0xFFFFu; put16(d + 0x1FFA, c); seal_dir(d);
    c = (be16(b + 4) + 1u) & 0xFFFFu; put16(b + 4, c); seal_bat(b);
    memcpy(img + 1u * MSCARD_BLOCK, d, MSCARD_BLOCK); memcpy(img + 2u * MSCARD_BLOCK, d, MSCARD_BLOCK);
    memcpy(img + 3u * MSCARD_BLOCK, b, MSCARD_BLOCK); memcpy(img + 4u * MSCARD_BLOCK, b, MSCARD_BLOCK);
    (void)free_n;
    return MSCARD_OK;
}

/* the chain of directory entry e: -> block count walked (0 on a broken chain); blocks[] receives at most max */
static mw2_u32 walk(const mw2_u8 *b, mw2_u32 total, const mw2_u8 *e, mw2_u32 *blocks, mw2_u32 max)
{
    unsigned want = be16(e + 0x38), cur = be16(e + 0x36);
    mw2_u32 n = 0;
    while (cur != 0xFFFFu) {
        if (cur < MSCARD_SYS || cur >= total || n >= want || n >= max) return 0;
        blocks[n++] = cur;
        cur = bat_get(b, cur);
    }
    return n == want ? n : 0;
}

mw2_u32 mscard_free_blocks(const mw2_u8 *img, mw2_u32 size)
{
    int di, bi;
    if (geometry(img, size, &di, &bi) != MSCARD_OK) return 0;
    return be16(img + (size_t)bi * MSCARD_BLOCK + 6);
}

int mscard_check(const mw2_u8 *img, mw2_u32 size)
{
    int di, bi, r;
    const mw2_u8 *d, *b;
    mw2_u32 total = size / MSCARD_BLOCK, i, k, used = 0, owned = 0, freec = 0, blk;
    mw2_u8 seen[2048 + 8];
    mw2_u32 chain[2048];
    r = geometry(img, size, &di, &bi);
    if (r != MSCARD_OK) return r;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < 127; i++) {
        const mw2_u8 *e = d + i * 0x40u;
        mw2_u32 n;
        if (entry_free(e)) continue;
        n = walk(b, total, e, chain, 2048);
        if (n == 0) return MSCARD_E_CHAIN;
        for (k = 0; k < n; k++) { if (seen[chain[k]]) return MSCARD_E_CHAIN; seen[chain[k]] = 1; owned++; }
    }
    for (blk = MSCARD_SYS; blk < total; blk++) { if (bat_get(b, blk)) used++; else freec++; }
    if (used != owned || freec != be16(b + 6)) return MSCARD_E_BAT;
    return MSCARD_OK;
}

/* one file of ours is on the card with identical bytes: 0 = yes, MSCARD_E_MISSING / MSCARD_E_DIFF otherwise (d, b = the valid directory and block map) */
static int has_one(const mw2_u8 *img, const mw2_u8 *d, const mw2_u8 *b, mw2_u32 total, const mw2_u8 *gci, mw2_u32 blocks)
{
    mw2_u32 j, k, chain[MSCRD_MAX_BLOCKS];
    for (j = 0; j < 127; j++) {
        const mw2_u8 *e = d + j * 0x40u;
        mw2_u32 n;
        if (entry_free(e) || memcmp(e, gci, 6) != 0 || memcmp(e + 8, gci + 8, 32) != 0) continue;
        n = walk(b, total, e, chain, MSCRD_MAX_BLOCKS);
        if (n != blocks) return MSCARD_E_DIFF;
        if (memcmp(e, gci, 0x36) != 0 || memcmp(e + 0x38, gci + 0x38, 8) != 0) return MSCARD_E_DIFF;     /* the entry, apart from its first-block field */
        for (k = 0; k < n; k++)
            if (memcmp(img + (size_t)chain[k] * MSCARD_BLOCK, gci + MSCRD_GCI_HDR + (size_t)k * MSCARD_BLOCK, MSCARD_BLOCK) != 0) return MSCARD_E_DIFF;
        return MSCARD_OK;
    }
    return MSCARD_E_MISSING;
}

int mscard_has_set(const mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set)
{
    int di, bi, r;
    mw2_u32 i;
    r = mscard_check(img, size);
    if (r != MSCARD_OK) return r;
    geometry(img, size, &di, &bi);
    for (i = 0; i < MSCRD_NFILES; i++) {
        r = has_one(img, img + (size_t)di * MSCARD_BLOCK, img + (size_t)bi * MSCARD_BLOCK, size / MSCARD_BLOCK, set->f[i].gci, set->f[i].blocks);
        if (r != MSCARD_OK) return r;
    }
    return MSCARD_OK;
}

int mscard_has_gci(const mw2_u8 *img, mw2_u32 size, const mw2_u8 *gci, mw2_u32 len)
{
    int di, bi, r;
    if (len < MSCRD_GCI_HDR + MSCARD_BLOCK || ((len - MSCRD_GCI_HDR) & (MSCARD_BLOCK - 1))) return MSCARD_E_ARG;
    r = mscard_check(img, size);
    if (r != MSCARD_OK) return r;
    geometry(img, size, &di, &bi);
    return has_one(img, img + (size_t)di * MSCARD_BLOCK, img + (size_t)bi * MSCARD_BLOCK, size / MSCARD_BLOCK, gci, (len - MSCRD_GCI_HDR) / MSCARD_BLOCK);
}

mw2_u32 mscard_extract(const mw2_u8 *img, mw2_u32 size, const char *name, mw2_u8 *out, mw2_u32 cap)
{
    int di, bi;
    const mw2_u8 *d, *b;
    mw2_u32 total = size / MSCARD_BLOCK, j, k, n, chain[MSCRD_MAX_BLOCKS], nl = (mw2_u32)strlen(name);
    if (nl == 0 || nl > 32 || geometry(img, size, &di, &bi) != MSCARD_OK) return 0;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    for (j = 0; j < 127; j++) {
        const mw2_u8 *e = d + j * 0x40u;
        mw2_u8 nm[32];
        if (entry_free(e)) continue;
        memset(nm, 0, 32); memcpy(nm, name, nl);
        if (memcmp(e, "GALE", 4) != 0 || memcmp(e + 8, nm, 32) != 0) continue;
        n = walk(b, total, e, chain, MSCRD_MAX_BLOCKS);
        if (n == 0 || MSCRD_GCI_HDR + n * MSCARD_BLOCK > cap) return 0;
        memcpy(out, e, MSCRD_GCI_HDR);
        out[0x36] = 0; out[0x37] = 0;                                           /* the first-block field is the card's own */
        for (k = 0; k < n; k++) memcpy(out + MSCRD_GCI_HDR + (size_t)k * MSCARD_BLOCK, img + (size_t)chain[k] * MSCARD_BLOCK, MSCARD_BLOCK);
        return MSCRD_GCI_HDR + n * MSCARD_BLOCK;
    }
    return 0;
}

/* ---- the in-place refresh --------------------------------------------------------------------------------------------------------------------------------------------------------- */
static const mw2_u8 *find_entry(const mw2_u8 *d, const char *name)
{
    mw2_u32 j, nl = (mw2_u32)strlen(name);
    mw2_u8 nm[32];
    if (nl == 0 || nl > 32) return 0;
    memset(nm, 0, 32); memcpy(nm, name, nl);
    for (j = 0; j < 127; j++) {
        const mw2_u8 *e = d + j * 0x40u;
        if (entry_free(e)) continue;
        if (memcmp(e, "GALE", 4) == 0 && memcmp(e + 8, nm, 32) == 0) return e;
    }
    return 0;
}

int mscard_remove(mw2_u8 *img, mw2_u32 size, const char *name)
{
    int di, bi, r;
    mw2_u8 *d, *b;
    const mw2_u8 *fe;
    mw2_u32 total = size / MSCARD_BLOCK, n, k, chain[MSCRD_MAX_BLOCKS];
    unsigned c;
    r = geometry(img, size, &di, &bi);
    if (r != MSCARD_OK) return r;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    fe = find_entry(d, name);
    if (!fe) return MSCARD_E_MISSING;
    n = walk(b, total, fe, chain, MSCRD_MAX_BLOCKS);
    if (n == 0) return MSCARD_E_CHAIN;
    for (k = 0; k < n; k++) put16(b + 10 + 2u * (chain[k] - MSCARD_SYS), 0);
    put16(b + 6, be16(b + 6) + (unsigned)n);
    memset(d + (fe - d), 0xFF, 0x40);
    c = (be16(d + 0x1FFA) + 1u) & 0xFFFFu; put16(d + 0x1FFA, c); seal_dir(d);
    c = (be16(b + 4) + 1u) & 0xFFFFu; put16(b + 4, c); seal_bat(b);
    memcpy(img + 1u * MSCARD_BLOCK, d, MSCARD_BLOCK); memcpy(img + 2u * MSCARD_BLOCK, d, MSCARD_BLOCK);
    memcpy(img + 3u * MSCARD_BLOCK, b, MSCARD_BLOCK); memcpy(img + 4u * MSCARD_BLOCK, b, MSCARD_BLOCK);
    return MSCARD_OK;
}

/* the MSCF header of block 0 of file data `blk0` (gci: entry + blocks, so the header is at 0x1000 of the DATA): seq / abi, 0 / 0 when it is not an MSCF header */
static void mscf_peek(const mw2_u8 *blk0, mw2_u32 *seq, unsigned *abi)
{
    const mw2_u8 *h = blk0 + 0x1000u;
    *seq = 0; *abi = 0;
    if (be32(h) != 0x4D534346u) return;
    *abi = be16(h + 6); *seq = be32(h + 8);
}
static int is_dev(const mw2_u8 *blk0) { return memcmp(blk0, "DEV-SIGNED CODE FILE", 20) == 0; }

int mscard_file_seq(const mw2_u8 *img, mw2_u32 size, const char *name, mw2_u32 *seq, unsigned *abi, int *dev)
{
    int di, bi;
    const mw2_u8 *d, *b, *fe;
    mw2_u32 chain[MSCRD_MAX_BLOCKS];
    *seq = 0; *abi = 0; *dev = 0;
    if (geometry(img, size, &di, &bi) != MSCARD_OK) return 0;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    fe = find_entry(d, name);
    if (!fe || walk(b, size / MSCARD_BLOCK, fe, chain, MSCRD_MAX_BLOCKS) == 0) return 0;
    mscf_peek(img + (size_t)chain[0] * MSCARD_BLOCK, seq, abi);
    *dev = is_dev(img + (size_t)chain[0] * MSCARD_BLOCK);
    return 1;
}

int mscard_file_hole(const mw2_u8 *img, mw2_u32 size, const char *name, mw2_u32 *base)
{
    int di, bi;
    const mw2_u8 *d, *b, *fe;
    mw2_u32 chain[MSCRD_MAX_BLOCKS];
    *base = 0;
    if (geometry(img, size, &di, &bi) != MSCARD_OK) return 0;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    fe = find_entry(d, name);
    if (!fe || walk(b, size / MSCARD_BLOCK, fe, chain, MSCRD_MAX_BLOCKS) == 0) return 0;
    *base = be32(img + (size_t)chain[0] * MSCARD_BLOCK + 0x1000u + 16u);
    return 1;
}

int mscard_update_code(mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set, struct msupd_report *rep)
{
    static const unsigned want_abi[2] = { MSUPD_CODE_ABI, MSUPD_OVL_ABI };
    mw2_u32 cur[2], nw[2];
    int i, dev, need[2], newer = 0, r;
    unsigned abi;
    memset(rep, 0, sizeof *rep);
    if (mscard_check(img, size) != MSCARD_OK) return MSUPD_BADCARD;
    for (i = 0; i < 2; i++) {                                                      /* the set's file i + 1 (MultishineCode, MultishineOvl): an MSCF file of the right kind */
        const struct mscrd_file *f = &set->f[i + 1];
        unsigned a;
        mscf_peek(f->gci + MSCRD_GCI_HDR, &nw[i], &a);
        if (nw[i] == 0 || a != want_abi[i]) return MSUPD_BADSET;
    }
    for (i = 0; i < 2; i++) {
        if (!mscard_file_seq(img, size, set->f[i + 1].name, &cur[i], &abi, &dev)) return MSUPD_NOFILES;
        if (dev) return MSUPD_DEV;
        if (abi != want_abi[i]) cur[i] = 0;                                        /* a file that is not a build file of this kind: replaced */
    }
    rep->code_old = cur[0]; rep->ovl_old = cur[1]; rep->code_new = nw[0]; rep->ovl_new = nw[1];
    for (i = 0; i < 2; i++) { need[i] = nw[i] > cur[i]; if (cur[i] > nw[i]) newer = 1; }
    rep->newer_on_card = newer;
    mscard_file_hole(img, size, set->f[1].name, &rep->hole_old);
    rep->hole_new = be32(set->f[1].gci + MSCRD_GCI_HDR + 0x1000u + 16u);
    rep->layout_changed = rep->hole_old != rep->hole_new;
    if (rep->layout_changed && need[0]) {                                          /* another card layout: the exploit save is replaced with the engine (never Code alone), and only for a NEWER Code */
        r = mscard_remove(img, size, set->f[0].name);
        if (r != MSCARD_OK && r != MSCARD_E_MISSING) { rep->err = r; return MSUPD_FAIL; }
        r = mscard_add(img, size, set->f[0].gci, set->f[0].len);
        if (r != MSCARD_OK) { rep->err = r; return MSUPD_FAIL; }
        need[1] = 1;                                                               /* the overlay is built for the layout too */
    } else if (rep->layout_changed) { rep->layout_changed = 0; return MSUPD_CURRENT; }       /* a different layout but not newer: never moved backwards by a stale container */
    if (!need[0] && !need[1]) return MSUPD_CURRENT;
    for (i = 0; i < 2; i++) {
        if (!need[i]) continue;
        r = mscard_remove(img, size, set->f[i + 1].name);
        if (r == MSCARD_OK) r = mscard_add(img, size, set->f[i + 1].gci, set->f[i + 1].len);
        if (r != MSCARD_OK) { rep->err = r; return MSUPD_FAIL; }
    }
    r = mscard_check(img, size);
    for (i = 0; r == MSCARD_OK && i < 2; i++) if (need[i]) r = mscard_has_gci(img, size, set->f[i + 1].gci, set->f[i + 1].len);
    if (r == MSCARD_OK && rep->layout_changed) r = mscard_has_gci(img, size, set->f[0].gci, set->f[0].len);
    if (r != MSCARD_OK) { rep->err = r; return MSUPD_FAIL; }
    return MSUPD_DONE;
}

static mw2_u32 crc_blocks(const mw2_u8 *img, const mw2_u32 *chain, mw2_u32 n)     /* CRC-32 (the msota_crc32 polynomial) over the blocks of a chain, in chain order */
{
    mw2_u32 c = 0xFFFFFFFFu, i, j;
    int k;
    for (j = 0; j < n; j++) {
        const mw2_u8 *p = img + (size_t)chain[j] * MSCARD_BLOCK;
        for (i = 0; i < MSCARD_BLOCK; i++) {
            c ^= p[i];
            for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (mw2_u32)(0u - (c & 1u)));
        }
    }
    return (c ^ 0xFFFFFFFFu) & 0xFFFFFFFFu;
}

static int same_but(const mw2_u8 *a, const mw2_u8 *b, mw2_u32 n, mw2_u32 skip0, mw2_u32 skip1, mw2_u32 skip2, mw2_u32 skip3)   /* equal outside [skip0,skip1) and [skip2,skip3) */
{
    mw2_u32 i;
    for (i = 0; i < n; i++) if (!(i >= skip0 && i < skip1) && !(i >= skip2 && i < skip3) && a[i] != b[i]) return 0;
    return 1;
}

int mscard_update_loader(mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set, const struct msldr_known *known, unsigned nknown, struct msldr_report *rep)
{
    const struct mscrd_file *nf = &set->f[0];
    const mw2_u8 *d, *b, *fe, *nd = nf->gci + MSCRD_GCI_HDR;
    mw2_u32 chain[MSCRD_MAX_BLOCKS], n, k, hole_card = 0, hole_new, same = 1;
    int di, bi, ko = -1, kn = -1;
    unsigned i;
    memset(rep, 0, sizeof *rep);
    rep->set_seq = set->seq;
    if (mscard_check(img, size) != MSCARD_OK || geometry(img, size, &di, &bi) != MSCARD_OK) return MSLDR_BADCARD;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    fe = find_entry(d, NAMES[0]);
    if (!fe) return MSLDR_NOLOADER;
    n = walk(b, size / MSCARD_BLOCK, fe, chain, MSCRD_MAX_BLOCKS);
    if (n != MSCRD_LOADER_BLOCKS || nf->blocks != MSCRD_LOADER_BLOCKS || nf->len != MSCRD_GCI_HDR + MSCRD_LOADER_BLOCKS * MSCARD_BLOCK || strcmp(nf->name, NAMES[0]) != 0) return MSLDR_OTHERART;
    rep->crc_old = crc_blocks(img, chain + MSLDR_FIX0, MSLDR_FIX1 - MSLDR_FIX0 + 1u);
    rep->crc_new = msota_crc32(nd + (size_t)MSLDR_FIX0 * MSCARD_BLOCK, (MSLDR_FIX1 - MSLDR_FIX0 + 1u) * MSCARD_BLOCK) & 0xFFFFFFFFu;
    for (k = MSLDR_FIX0; k <= MSLDR_FIX1; k++) if (memcmp(img + (size_t)chain[k] * MSCARD_BLOCK, nd + (size_t)k * MSCARD_BLOCK, MSCARD_BLOCK) != 0) same = 0;
    if (same) return MSLDR_SAME;
    for (i = 0; i < nknown; i++) { if (known[i].crc == rep->crc_old) ko = (int)i; if (known[i].crc == rep->crc_new) kn = (int)i; }
    if (ko < 0) return MSLDR_UNKNOWN;
    rep->first_old = known[ko].first_seq;
    if (!same_but(fe, nf->gci, MSCRD_GCI_HDR, 0x28u, 0x2Cu, 0x36u, 0x38u)) return MSLDR_OTHERART;                     /* entry: the game's time and the card's first block apart */
    if (!same_but(img + (size_t)chain[0] * MSCARD_BLOCK, nd, MSCARD_BLOCK, 0x20u, 0x40u, 0x1E40u, 0x1E60u)) return MSLDR_OTHERART;   /* block 0: banner, icon, title line (our art) */
    hole_new = be32(set->f[1].gci + MSCRD_GCI_HDR + 0x1000u + 16u);
    if (!mscard_file_hole(img, size, NAMES[1], &hole_card) || hole_card != hole_new) return MSLDR_LAYOUT;
    if (set->seq <= known[ko].first_seq || (kn >= 0 && known[kn].first_seq < known[ko].first_seq)) return MSLDR_OLDER;
    for (k = MSLDR_FIX0; k <= MSLDR_FIX1; k++) memcpy(img + (size_t)chain[k] * MSCARD_BLOCK, nd + (size_t)k * MSCARD_BLOCK, MSCARD_BLOCK);
    if (mscard_check(img, size) != MSCARD_OK) { rep->err = MSCARD_E_BAT; return MSLDR_FAIL; }
    for (k = MSLDR_FIX0; k <= MSLDR_FIX1; k++)                                                                     /* read back what was written */
        if (memcmp(img + (size_t)chain[k] * MSCARD_BLOCK, nd + (size_t)k * MSCARD_BLOCK, MSCARD_BLOCK) != 0) { rep->err = MSCARD_E_DIFF; return MSLDR_FAIL; }
    return MSLDR_DONE;
}

const char *msldr_text(int r)
{
    switch (r) {
    case MSLDR_DONE: return "loader save replaced";
    case MSLDR_SAME: return "loader save current";
    case MSLDR_NOLOADER: return "no loader save on the card";
    case MSLDR_UNKNOWN: return "loader save not a released one (dev, lab or foreign): kept";
    case MSLDR_OTHERART: return "loader save of another kind: kept";
    case MSLDR_LAYOUT: return "card layout differs: left to the layout rule";
    case MSLDR_OLDER: return "container not newer than the card's loader save: kept";
    case MSLDR_BADCARD: return "card does not check";
    case MSLDR_FAIL: return "loader save replacement failed";
    default: return "?";
    }
}

int mscard_refresh(mw2_u8 *img, mw2_u32 size, const struct mscrd_set *set, const struct msldr_known *known, unsigned nknown, struct msref_report *rep)
{
    memset(rep, 0, sizeof *rep);
    rep->ldr_r = -1;
    rep->code_r = mscard_update_code(img, size, set, &rep->code);
    if (rep->code_r != MSUPD_DONE && rep->code_r != MSUPD_CURRENT) return rep->code_r;
    rep->ldr_r = mscard_update_loader(img, size, set, known, nknown, &rep->ldr);
    if (rep->ldr_r == MSLDR_FAIL) return MSUPD_FAIL;
    return (rep->code_r == MSUPD_DONE || rep->ldr_r == MSLDR_DONE) ? MSUPD_DONE : MSUPD_CURRENT;
}

const char *msupd_text(int r)
{
    switch (r) {
    case MSUPD_DONE: return "updated";
    case MSUPD_CURRENT: return "current";
    case MSUPD_DEV: return "dev-signed file kept";
    case MSUPD_NOFILES: return "no build files on the card";
    case MSUPD_BADCARD: return "card does not check";
    case MSUPD_BADSET: return "container files not recognised";
    case MSUPD_FAIL: return "update failed";
    default: return "?";
    }
}

int mscard_boot_record(const mw2_u8 *img, mw2_u32 size, mw2_u32 *cand, unsigned *tries, unsigned *good)
{
    int di, bi;
    const mw2_u8 *d, *b, *fe, *r;
    mw2_u32 chain[MSCRD_MAX_BLOCKS];
    *cand = 0; *tries = 0; *good = 0;
    if (geometry(img, size, &di, &bi) != MSCARD_OK) return 0;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    fe = find_entry(d, "MultishineState");
    if (!fe || walk(b, size / MSCARD_BLOCK, fe, chain, MSCRD_MAX_BLOCKS) == 0) return 0;
    r = img + (size_t)chain[0] * MSCARD_BLOCK + 0x1000u + 16u;
    if (memcmp(r, "MSBT", 4) != 0 || msota_crc32(r, 12u) != be32(r + 12)) return 0;
    *cand = be32(r + 4); *tries = be16(r + 8); *good = be16(r + 10);
    return 1;
}

int msupd_pend_parse(const char *t, mw2_u32 n, mw2_u32 *seq, unsigned *starts)
{
    mw2_u32 i = 5, v = 0, w = 0;
    int d1 = 0, d2 = 0;
    if (n < 8u || memcmp(t, "pend ", 5) != 0) return 0;
    while (i < n && t[i] >= '0' && t[i] <= '9') { v = v * 10u + (mw2_u32)(t[i] - '0'); i++; d1++; }
    if (!d1 || i >= n || t[i] != ' ') return 0;
    i++;
    while (i < n && t[i] >= '0' && t[i] <= '9') { w = w * 10u + (mw2_u32)(t[i] - '0'); i++; d2++; }
    if (!d2 || w > 99u) return 0;
    *seq = v; *starts = (unsigned)w;
    return 1;
}

mw2_u32 msupd_pend_fmt(char *out, mw2_u32 cap, mw2_u32 seq, unsigned starts)
{
    char num[12];
    mw2_u32 n = 0, k = 0, v = seq;
    unsigned s = starts > 99u ? 99u : starts;
    if (cap < 28u) return 0;
    memcpy(out, "pend ", 5); n = 5;
    if (v == 0) num[k++] = '0';
    while (v) { num[k++] = (char)('0' + v % 10u); v /= 10u; }
    while (k) out[n++] = num[--k];
    out[n++] = ' ';
    if (s >= 10u) out[n++] = (char)('0' + s / 10u);
    out[n++] = (char)('0' + s % 10u);
    out[n++] = '\n';
    return n;
}

int msupd_pend_decide(mw2_u32 seq, unsigned starts, int rec_ok, mw2_u32 cand, unsigned tries, unsigned good)
{
    if (rec_ok && cand == seq && good) return MSUPD_PEND_CONFIRMED;
    if (rec_ok && cand == seq && tries >= MSUPD_PEND_MAX) return MSUPD_PEND_ROLLBACK;      /* the engine started and died three times: its own loader falls back, but a card with no previous build would stay broken */
    if (starts >= MSUPD_PEND_MAX) return MSUPD_PEND_ROLLBACK;
    return MSUPD_PEND_WAIT;
}

int mscard_boot_arm(mw2_u8 *img, mw2_u32 size, mw2_u32 seq)
{
    int di, bi;
    const mw2_u8 *d, *b, *fe;
    mw2_u8 *r;
    mw2_u32 chain[MSCRD_MAX_BLOCKS], c;
    if (geometry(img, size, &di, &bi) != MSCARD_OK) return 0;
    d = img + (size_t)di * MSCARD_BLOCK; b = img + (size_t)bi * MSCARD_BLOCK;
    fe = find_entry(d, "MultishineState");
    if (!fe || walk(b, size / MSCARD_BLOCK, fe, chain, MSCRD_MAX_BLOCKS) == 0) return 0;
    r = img + (size_t)chain[0] * MSCARD_BLOCK + 0x1000u + 16u;                     /* the floor record 'MSST' at +0x1000 is not touched */
    memcpy(r, "MSBT", 4);
    r[4] = (mw2_u8)(seq >> 24); r[5] = (mw2_u8)(seq >> 16); r[6] = (mw2_u8)(seq >> 8); r[7] = (mw2_u8)seq;
    r[8] = r[9] = r[10] = r[11] = 0;                                                   /* tries 0, good 0 */
    c = msota_crc32(r, 12u);
    r[12] = (mw2_u8)(c >> 24); r[13] = (mw2_u8)(c >> 16); r[14] = (mw2_u8)(c >> 8); r[15] = (mw2_u8)c;
    return 1;
}

int msupd_pend_step(mw2_u32 seq, unsigned starts, int rec_ok, mw2_u32 cand, unsigned tries, unsigned good, mw2_u32 code_seq, mw2_u32 *target)
{
    mw2_u32 t = code_seq ? code_seq : seq;
    *target = t;
    if (rec_ok && cand == t && good) return MSUPD_PEND_CONFIRMED;
    if (code_seq && (!rec_ok || cand != t)) return MSUPD_PEND_REARM;                   /* never armed for the build on the card: no proof either way yet */
    return msupd_pend_decide(t, starts, rec_ok, cand, tries, good);
}

const char *mscard_err(int e)
{
    switch (e) {
    case MSCARD_OK: return "ok";
    case MSCARD_E_SIZE: return "card size";
    case MSCARD_E_HDR: return "card header";
    case MSCARD_E_DIR: return "card directory";
    case MSCARD_E_BAT: return "card block map";
    case MSCARD_E_ARG: return "file";
    case MSCARD_E_EXISTS: return "file already on the card";
    case MSCARD_E_FULL: return "card directory full";
    case MSCARD_E_NOSPACE: return "not enough free blocks";
    case MSCARD_E_CHAIN: return "block chain";
    case MSCARD_E_MISSING: return "file missing";
    case MSCARD_E_DIFF: return "file differs";
    default: return "?";
    }
}
