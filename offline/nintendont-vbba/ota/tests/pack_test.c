/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_pack_core.c (tests/test_pack.py builds the containers, discs and cards and runs this).  The loader compiles the same file.
 *   pack_test pack PUBHEX MINSEQ FILE         msota_check_pack on a served pack-N.mscf; prints "RESULT <code> <name> seq <n> len4 <n> ... "
 *   pack_test fst DISC                        mspk_find_files over a disc image file; prints "FST <code> off len x4"
 *   pack_test build PUBHEX PACK DISC OUT [budget]   check PACK, build the system files from DISC (the file-backed disc image), write the card file; prints "OK blocks steps heap_peak biggest leak"
 *   pack_test fault PUBHEX PACK DISC          every allocation failing in turn ends in MSPK_E_MEM without a leak
 *   pack_test valid GCI                       mspk_pack_valid; prints "VALID <0|1>"
 *   pack_test cardadd RAW GCI                 mspk_card_add into RAW (in place, 2 MiB); prints "ADD <code>"
 *   pack_test extract RAW OUT                 mspk_card_extract; prints "EXTRACT <0|1> <len>"
 *   pack_test text                            the texts of every outcome, one per line
 *   pack_test header HEXFILE                  mspk_disc_header_ok on the first 32 bytes of a file */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_pack_core.h"

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static mw2_u8 *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    mw2_u8 *b;
    long l;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = (mw2_u8 *)calloc((size_t)l + 64, 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return 0; }
    fclose(f);
    *n = (size_t)l;
    return b;
}
static void key_of(const char *pub, mw2_u8 key[1][32]) { int i; for (i = 0; i < 32; i++) key[0][i] = (mw2_u8)(hexval(pub[2 * i]) * 16 + hexval(pub[2 * i + 1])); }

static FILE *g_disc;
static mw2_u32 g_disc_size;
static int fault_alloc = -1, nalloc;
static unsigned long live, peak_live, biggest;
static int h_rd(void *c, mw2_u32 off, mw2_u8 *dst, mw2_u32 len)
{
    (void)c;
    if (off > g_disc_size || len > g_disc_size - off) return -1;
    if (fseek(g_disc, (long)off, SEEK_SET) || fread(dst, 1, len, g_disc) != len) return -1;
    return 0;
}
static void *h_alloc(void *c, mw2_u32 n)
{
    unsigned char *p;
    (void)c;
    if (nalloc++ == fault_alloc) return 0;
    p = (unsigned char *)aligned_alloc(32, (((size_t)n + 32) + 31) & ~(size_t)31);
    memcpy(p, &n, 4);
    live += n; if (live > peak_live) peak_live = live; if (n > biggest) biggest = n;
    return p + 32;
}
static void h_rel(void *c, void *q)
{
    unsigned char *p = (unsigned char *)q - 32; unsigned n;
    (void)c;
    memcpy(&n, p, 4);
    live -= n;
    free(p);
}
static int open_disc(const char *path)
{
    g_disc = fopen(path, "rb");
    if (!g_disc) return 0;
    fseek(g_disc, 0, SEEK_END); g_disc_size = (mw2_u32)ftell(g_disc);
    return 1;
}
static struct mspk_host HOST = { 0, h_rd, 0, h_alloc, h_rel };

static int build_once(const mw2_u8 *pk, size_t pn, const mw2_u8 key[1][32], unsigned budget, mw2_u8 **out, mw2_u32 *outlen, unsigned *steps, int *rc_begin)
{
    static struct mspk_data d;
    struct mspk_build b;
    unsigned pct = 0;
    int r;
    *steps = 0; *out = 0;
    r = msota_check_pack(pk, (mw2_u32)pn, (const mw2_u8 (*)[32])key, 1, 1, &d);
    if (r != MSOTA_OK) return 100 + r;
    HOST.disc_size = g_disc_size;
    *rc_begin = mspk_begin(&b, &HOST, &d);
    if (*rc_begin != MSPK_OK) return *rc_begin;
    while ((r = mspk_step(&b, budget, &pct)) == 0) (*steps)++;
    if (r == 1) {
        const mw2_u8 *g = mspk_gci(&b, outlen);
        *out = (mw2_u8 *)malloc(*outlen);
        memcpy(*out, g, *outlen);
        r = 0;
    }
    mspk_end(&b);
    return r;
}

/* "pick": a fake games tree - the candidates are the argument paths (in the order given = name order), a file is "stock" when it holds STOCK at offset 0x40; the second argument lists the paths
   the verdict cache accepts (comma separated, "-" = none) */
static char **g_cand; static int g_ncand; static const char *g_cache; static int g_full_checks;
static int pk_list(void *c, unsigned i, char *p, unsigned n) { (void)c; if ((int)i >= g_ncand || strlen(g_cand[i]) >= n) return 0; strcpy(p, g_cand[i]); return 1; }
static int pk_check(void *c, const char *p, int cache_only)
{
    char tag[8] = { 0 };
    FILE *f;
    size_t l = strlen(p);
    const char *q = g_cache;
    (void)c;
    while (q && *q && *q != '-') {
        const char *e = strchr(q, ',');
        size_t ql = e ? (size_t)(e - q) : strlen(q);
        if (ql == l && !memcmp(q, p, l)) return 1;
        q = e ? e + 1 : 0;
    }
    if (cache_only) return -1;
    g_full_checks++;
    f = fopen(p, "rb");
    if (!f) return 0;
    if (fseek(f, 0x40, SEEK_SET) == 0 && fread(tag, 1, 5, f) == 5) { fclose(f); return !memcmp(tag, "STOCK", 5); }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    mw2_u8 key[1][32];
    size_t n = 0;
    mw2_u8 *buf;
    int i;
    if (argc >= 5 && !strcmp(argv[1], "pack")) {
        struct mspk_data d;
        int r;
        key_of(argv[2], key);
        buf = slurp(argv[4], &n);
        if (!buf) return 2;
        r = msota_check_pack(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, (mw2_u32)strtoul(argv[3], 0, 10), &d);
        printf("RESULT %d %s", r, msota_err(r));
        if (r == MSOTA_OK) { printf(" seq %lu", (unsigned long)d.seq); for (i = 4; i < 10; i++) printf(" %lu", (unsigned long)d.len[i]); }
        printf("\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "fst")) {
        struct mspk_loc loc[5];
        int r;
        if (!open_disc(argv[2])) return 2;
        HOST.disc_size = g_disc_size;
        r = mspk_find_files(&HOST, loc);
        printf("FST %d", r);
        if (r == MSPK_OK) for (i = 0; i < 4; i++) printf(" %lu %lu", (unsigned long)loc[i].off, (unsigned long)loc[i].len);
        printf(" leak %lu\n", live);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "build")) {
        mw2_u8 *pk, *g; size_t pn = 0; mw2_u32 gl = 0; unsigned steps; int r, rb = 0; FILE *f;
        key_of(argv[2], key);
        pk = slurp(argv[3], &pn);
        if (!pk || !open_disc(argv[4])) return 2;
        r = build_once(pk, pn, key, argc > 6 ? (unsigned)atoi(argv[6]) : 200000u, &g, &gl, &steps, &rb);
        if (r) { printf("FAIL %d leak %lu\n", r, live); return 1; }
        f = fopen(argv[5], "wb"); fwrite(g, 1, gl, f); fclose(f);
        printf("OK blocks %lu steps %u heap_peak %lu biggest %lu leak %lu\n", (unsigned long)((gl - 0x40) / 0x2000), steps, peak_live, biggest, live);
        return live ? 1 : 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "fault")) {
        mw2_u8 *pk, *g; size_t pn = 0; mw2_u32 gl = 0; unsigned steps; int r, rb = 0, k, total;
        key_of(argv[2], key);
        pk = slurp(argv[3], &pn);
        if (!pk || !open_disc(argv[4])) return 2;
        r = build_once(pk, pn, key, 200000u, &g, &gl, &steps, &rb);
        if (r) { printf("baseline failed %d\n", r); return 1; }
        free(g); total = nalloc;
        for (k = 0; k < total; k++) {
            fault_alloc = k; nalloc = 0;
            r = build_once(pk, pn, key, 200000u, &g, &gl, &steps, &rb);
            if (r != MSPK_E_MEM) { printf("alloc %d ended in %d, not MSPK_E_MEM\n", k, r); return 1; }
            if (live) { printf("alloc %d leaked %lu\n", k, live); return 1; }
        }
        printf("OK %d allocation faults\n", total);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "valid")) {
        buf = slurp(argv[2], &n);
        if (!buf) return 2;
        printf("VALID %d\n", mspk_pack_valid(buf, (mw2_u32)n));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "cardadd")) {
        size_t rn = 0, gn = 0; mw2_u8 *raw = slurp(argv[2], &rn), *g = slurp(argv[3], &gn); FILE *f;
        int r;
        if (!raw || !g) return 2;
        r = mspk_card_add(raw, (mw2_u32)rn, g, (mw2_u32)gn);
        printf("ADD %d free %lu\n", r, (unsigned long)mscard_free_blocks(raw, (mw2_u32)rn));
        if (r == 0) { f = fopen(argv[2], "wb"); fwrite(raw, 1, rn, f); fclose(f); }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "extract")) {
        size_t rn = 0; mw2_u8 *raw = slurp(argv[2], &rn), *out = (mw2_u8 *)malloc(64 * 0x2000); mw2_u32 len = 0; FILE *f;
        int r;
        if (!raw) return 2;
        r = mspk_card_extract(raw, (mw2_u32)rn, out, 64 * 0x2000, &len);
        printf("EXTRACT %d %lu\n", r, (unsigned long)len);
        if (r) { f = fopen(argv[3], "wb"); fwrite(out, 1, len, f); fclose(f); }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "pick")) {
        static char out[MSPK_MAX_PICK][300];
        unsigned seen = 0;
        int k, np;
        char t[80];
        g_cache = argv[2]; g_cand = argv + 3; g_ncand = argc - 3;
        np = mspk_pick_discs(pk_list, pk_check, 0, &out[0][0], 300, MSPK_MAX_PICK, &seen);
        printf("PICK %d seen %u full %d\n", np, seen, g_full_checks);
        for (k = 0; k < np; k++) printf("%s\n", out[k]);
        mspk_none_text(t, sizeof t, seen);
        printf("%s\n", t);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "text")) {
        for (i = 0; i < MSPK_O_COUNT; i++) printf("%d|%d|%s|%s\n", i, mspk_outcome_ok(i), mspk_text(i), mspk_text2(i));
        printf("map %d %d %d %d %d %d\n", mspk_outcome_of_build(MSPK_E_NODISC), mspk_outcome_of_build(MSPK_E_INPUT), mspk_outcome_of_build(MSPK_E_READ), mspk_outcome_of_build(MSPK_E_MEM), mspk_outcome_of_build(MSPK_E_BUILD), mspk_outcome_of_build(MSPK_E_FST));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "header")) {
        buf = slurp(argv[2], &n);
        if (!buf || n < 32) return 2;
        printf("HEADER %d\n", mspk_disc_header_ok(buf));
        return 0;
    }
    fprintf(stderr, "usage: see the top of the file\n");
    return 2;
}
