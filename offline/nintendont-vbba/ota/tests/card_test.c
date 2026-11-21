/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_card_core.c (tests/test_card.py builds the containers and cards and runs this).
 *   card_test crd PUBHEX MINSEQ FILE       msota_check_crd on a served crd-N.mscf; prints "RESULT <code> <name> seq <n> blocks <n>"
 *   card_test build PUBHEX FILE OUT [SIZE] check FILE, format a card (default 2 MiB), add the four files, check it, verify the set, write OUT; prints "BUILT" lines
 *   card_test check RAW                    mscard_check; prints "CARD <code> <name> free <n>"
 *   card_test has PUBHEX FILE RAW          mscard_has_set of the container's files on RAW; prints "HAS <code> <name>"
 *   card_test addgci RAW GCI               mscard_add one .gci into RAW (in place); prints "ADD <code> <name>"
 *   card_test format SIZE OUT              an empty formatted card
 *   card_test refresh PUBHEX FILE RAW OUT  mscard_update_code of the container's Code / Overlay on RAW; prints "REFRESH <code> <text> code a b ovl c d newer n"; OUT written only on MSUPD_DONE
 *   card_test refresh2 PUBHEX FILE RAW OUT [CRC:SEQ ...]   mscard_refresh (Code / Overlay, then the loader save) with the given released-loader table (hex CRC-32 : first seq; none = MSLDR_KNOWN_INIT);
 *                                          prints "REFRESH2 <r> code <code_r> ldr <ldr_r> <ldr text> crc <old> <new> first <n>"; OUT written only on MSUPD_DONE
 *   card_test remove RAW NAME OUT          mscard_remove; prints "REMOVE <code> <name>"
 *   card_test seqof RAW NAME               mscard_file_seq; prints "SEQ <found> seq <n> abi <n> dev <n>"
 *   card_test bootrec RAW                  mscard_boot_record of MultishineState; prints "BOOTREC <ok> cand <n> tries <n> good <n>"
 *   card_test pendparse TEXT               msupd_pend_parse; prints "PEND <ok> seq <n> starts <n>"
 *   card_test pendfmt SEQ STARTS           msupd_pend_fmt; prints the text (quoted)
 *   card_test penddecide SEQ STARTS OK CAND TRIES GOOD   msupd_pend_decide; prints "DECIDE <0 wait|1 confirmed|2 rollback>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_card_core.h"

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static mw2_u8 *slurp(const char *path, size_t *n, size_t extra)
{
    FILE *f = fopen(path, "rb");
    mw2_u8 *b;
    long l;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = (mw2_u8 *)calloc((size_t)l + extra + 1, 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return 0; }
    fclose(f);
    *n = (size_t)l;
    return b;
}
static void key_of(const char *pub, mw2_u8 key[1][32]) { int i; for (i = 0; i < 32; i++) key[0][i] = (mw2_u8)(hexval(pub[2 * i]) * 16 + hexval(pub[2 * i + 1])); }
static int spit(const char *path, const mw2_u8 *b, size_t n) { FILE *f = fopen(path, "wb"); if (!f) return 0; fwrite(b, 1, n, f); fclose(f); return 1; }

int main(int argc, char **argv)
{
    mw2_u8 key[1][32];
    size_t n = 0, m = 0;
    mw2_u8 *buf, *raw;
    struct mscrd_set set;
    int r, i;
    if (argc >= 5 && !strcmp(argv[1], "crd")) {
        key_of(argv[2], key);
        buf = slurp(argv[4], &n, 0);
        if (!buf) { printf("RESULT -1 nofile\n"); return 2; }
        r = msota_check_crd(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, (mw2_u32)strtoul(argv[3], 0, 10), &set);
        if (r == MSOTA_OK) printf("RESULT 0 ok seq %lu blocks %lu\n", (unsigned long)set.seq, (unsigned long)set.blocks);
        else printf("RESULT %d %s\n", r, msota_err(r));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "build")) {
        mw2_u32 size = argc > 5 ? (mw2_u32)strtoul(argv[5], 0, 0) : MSCARD_SIZE;
        key_of(argv[2], key);
        buf = slurp(argv[3], &n, 0);
        if (!buf || msota_check_crd(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, 1, &set) != MSOTA_OK) { printf("BUILT refused container\n"); return 3; }
        raw = (mw2_u8 *)malloc(size);
        r = mscard_format(raw, size);
        if (r) { printf("BUILT format %s\n", mscard_err(r)); return 3; }
        for (i = 0; i < (int)MSCRD_NFILES; i++) {
            r = mscard_add(raw, size, set.f[i].gci, set.f[i].len);
            if (r) { printf("BUILT add %s %s\n", set.f[i].name, mscard_err(r)); return 3; }
        }
        r = mscard_check(raw, size); if (r) { printf("BUILT check %s\n", mscard_err(r)); return 3; }
        r = mscard_has_set(raw, size, &set); if (r) { printf("BUILT set %s\n", mscard_err(r)); return 3; }
        printf("BUILT ok free %lu\n", (unsigned long)mscard_free_blocks(raw, size));
        if (!spit(argv[4], raw, size)) return 4;
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "sized")) {                /* sized PUBHEX SIZE HEADCAP KIND [FILE.crd] OUT : head only in memory, written as head + zeros; KIND blank | files */
        mw2_u32 size = (mw2_u32)strtoul(argv[3], 0, 0), head = (mw2_u32)strtoul(argv[4], 0, 0);
        const char *kind = argv[5], *outp = argc > 7 ? argv[7] : argv[6];
        mw2_u32 z;
        FILE *o;
        key_of(argv[2], key);
        raw = (mw2_u8 *)malloc(head);
        r = mscard_format_head(raw, head, size);
        if (r) { printf("SIZED format %s\n", mscard_err(r)); return 3; }
        if (!strcmp(kind, "files")) {
            buf = slurp(argv[6], &n, 0);
            if (!buf || msota_check_crd(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, 1, &set) != MSOTA_OK) { printf("SIZED refused container\n"); return 3; }
            for (i = 0; i < (int)MSCRD_NFILES; i++) { r = mscard_add(raw, size, set.f[i].gci, set.f[i].len); if (r) { printf("SIZED add %s\n", mscard_err(r)); return 3; } }
        }
        r = mscard_check(raw, size);
        printf("SIZED ok check %d free %lu\n", r, (unsigned long)mscard_free_blocks(raw, size));
        o = fopen(outp, "wb"); fwrite(raw, 1, head, o);
        { static mw2_u8 zero[65536]; for (z = head; z < size; z += sizeof zero) fwrite(zero, 1, size - z < sizeof zero ? size - z : sizeof zero, o); }
        fclose(o);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "check")) {
        raw = slurp(argv[2], &n, 0);
        if (!raw) { printf("CARD -1 nofile\n"); return 2; }
        r = mscard_check(raw, (mw2_u32)n);
        printf("CARD %d %s free %lu\n", r, mscard_err(r), (unsigned long)mscard_free_blocks(raw, (mw2_u32)n));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "has")) {
        key_of(argv[2], key);
        buf = slurp(argv[3], &n, 0); raw = slurp(argv[4], &m, 0);
        if (!buf || !raw || msota_check_crd(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, 1, &set) != MSOTA_OK) { printf("HAS -1 input\n"); return 2; }
        r = mscard_has_set(raw, (mw2_u32)m, &set);
        printf("HAS %d %s\n", r, mscard_err(r));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "addgci")) {
        raw = slurp(argv[2], &m, 0); buf = slurp(argv[3], &n, 0);
        if (!raw || !buf) { printf("ADD -1 input\n"); return 2; }
        r = mscard_add(raw, (mw2_u32)m, buf, (mw2_u32)n);
        printf("ADD %d %s\n", r, mscard_err(r));
        if (r == 0) spit(argv[2], raw, m);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "format")) {
        mw2_u32 size = (mw2_u32)strtoul(argv[2], 0, 0);
        raw = (mw2_u8 *)malloc(size ? size : 1);
        r = mscard_format(raw, size);
        printf("FORMAT %d %s\n", r, mscard_err(r));
        if (r == 0) spit(argv[3], raw, size);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "refresh")) {
        struct msupd_report rep;
        key_of(argv[2], key);
        buf = slurp(argv[3], &n, 0); raw = slurp(argv[4], &m, 0);
        if (!buf || !raw || msota_check_crd(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, 1, &set) != MSOTA_OK) { printf("REFRESH -1 input\n"); return 2; }
        r = mscard_update_code(raw, (mw2_u32)m, &set, &rep);
        printf("REFRESH %d %s code %lu %lu ovl %lu %lu newer %d layout %d\n", r, msupd_text(r), (unsigned long)rep.code_old, (unsigned long)rep.code_new, (unsigned long)rep.ovl_old, (unsigned long)rep.ovl_new, rep.newer_on_card, rep.layout_changed);
        if (r == MSUPD_DONE) spit(argv[5], raw, m);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "refresh2")) {
        static const struct msldr_known def[MSLDR_NKNOWN] = MSLDR_KNOWN_INIT;
        struct msldr_known tab[16];
        const struct msldr_known *kt = def;
        unsigned nk = MSLDR_NKNOWN;
        struct msref_report rr;
        int i;
        if (argc > 6) {
            nk = 0;
            for (i = 6; i < argc && nk < 16; i++) { unsigned long c = 0, q = 0; if (sscanf(argv[i], "%lx:%lu", &c, &q) == 2) { tab[nk].crc = (mw2_u32)c; tab[nk].first_seq = (mw2_u32)q; nk++; } }
            kt = tab;
        }
        key_of(argv[2], key);
        buf = slurp(argv[3], &n, 0); raw = slurp(argv[4], &m, 0);
        if (!buf || !raw || msota_check_crd(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, 1, &set) != MSOTA_OK) { printf("REFRESH2 -1 input\n"); return 2; }
        r = mscard_refresh(raw, (mw2_u32)m, &set, kt, nk, &rr);
        printf("REFRESH2 %d code %d ldr %d %s crc %08lX %08lX first %lu\n", r, rr.code_r, rr.ldr_r, rr.ldr_r >= 0 ? msldr_text(rr.ldr_r) : "-", (unsigned long)rr.ldr.crc_old, (unsigned long)rr.ldr.crc_new, (unsigned long)rr.ldr.first_old);
        if (r == MSUPD_DONE) spit(argv[5], raw, m);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "remove")) {
        raw = slurp(argv[2], &m, 0);
        if (!raw) { printf("REMOVE -1 input\n"); return 2; }
        r = mscard_remove(raw, (mw2_u32)m, argv[3]);
        printf("REMOVE %d %s\n", r, mscard_err(r));
        if (r == 0) spit(argv[4], raw, m);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "seqof")) {
        mw2_u32 seq; unsigned abi; int dev;
        raw = slurp(argv[2], &m, 0);
        if (!raw) { printf("SEQ -1 input\n"); return 2; }
        r = mscard_file_seq(raw, (mw2_u32)m, argv[3], &seq, &abi, &dev);
        printf("SEQ %d seq %lu abi %u dev %d\n", r, (unsigned long)seq, abi, dev);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "bootrec")) {
        mw2_u32 cand; unsigned tries, good;
        raw = slurp(argv[2], &m, 0);
        if (!raw) { printf("BOOTREC -1 input\n"); return 2; }
        r = mscard_boot_record(raw, (mw2_u32)m, &cand, &tries, &good);
        printf("BOOTREC %d cand %lu tries %u good %u\n", r, (unsigned long)cand, tries, good);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "pendparse")) {
        mw2_u32 seq = 0; unsigned st = 0;
        r = msupd_pend_parse(argv[2], (mw2_u32)strlen(argv[2]), &seq, &st);
        printf("PEND %d seq %lu starts %u\n", r, (unsigned long)seq, st);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "pendfmt")) {
        char o[64]; mw2_u32 k = msupd_pend_fmt(o, sizeof o, (mw2_u32)strtoul(argv[2], 0, 10), (unsigned)strtoul(argv[3], 0, 10));
        o[k] = 0;
        printf("%u:", (unsigned)k); for (i = 0; i < (int)k; i++) printf(o[i] == '\n' ? "\\n" : "%c", o[i]); printf("\n");
        return 0;
    }
    if (argc >= 8 && !strcmp(argv[1], "penddecide")) {
        printf("DECIDE %d\n", msupd_pend_decide((mw2_u32)strtoul(argv[2], 0, 10), (unsigned)strtoul(argv[3], 0, 10), atoi(argv[4]), (mw2_u32)strtoul(argv[5], 0, 10), (unsigned)strtoul(argv[6], 0, 10), (unsigned)strtoul(argv[7], 0, 10)));
        return 0;
    }
    fprintf(stderr, "usage: see the header comment\n");
    return 2;
}
