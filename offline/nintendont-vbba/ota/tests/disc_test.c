/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_disc_lock.c (tests/test_disc.py builds the cases and runs this).
 *   disc_test cache                      the verdict cache format, asserts
 *   disc_test verdict FILE [MOD ...]     msdl_verdict on FILE read through the same callback the loader uses (a seek + read), with virtual modifications applied to what is read:
 *                                          flip=OFF        xor 0xFF into the byte at OFF         byte=OFF:VAL   set the byte at OFF to VAL (decimal or 0x..)
 *                                          ascii=OFF:TXT   write TXT at OFF                      zero=OFF:LEN   zero a range (a scrubbed padding area)
 *                                          size=N          report a file of N bytes (a truncated copy; reads past N fail)
 *                                        prints "VERDICT <code> <name>" and "READS <n windows> BYTES <n>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_disc_lock.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static FILE *img;
static unsigned long img_size, nreads, nbytes;
enum { M_FLIP, M_BYTE, M_ASCII, M_ZERO };
static struct { int k; unsigned long off, len; unsigned char val; char txt[16]; } mods[16];
static int nmods;

static int rd(void *ctx, unsigned long off, unsigned char *buf, unsigned long len)
{
    int i;
    (void)ctx;
    nreads++; nbytes += len;
    if (off + len > img_size) return -1;
    if (fseek(img, (long)off, SEEK_SET) || fread(buf, 1, len, img) != len) return -1;
    for (i = 0; i < nmods; i++) {
        unsigned long a = mods[i].off, b = mods[i].off + (mods[i].k == M_ASCII ? strlen(mods[i].txt) : mods[i].k == M_ZERO ? mods[i].len : 1ul), x;
        for (x = off > a ? off : a; x < off + len && x < b; x++) {
            switch (mods[i].k) {
            case M_FLIP: buf[x - off] ^= 0xFF; break;
            case M_BYTE: buf[x - off] = mods[i].val; break;
            case M_ASCII: buf[x - off] = (unsigned char)mods[i].txt[x - a]; break;
            default: buf[x - off] = 0; break;
            }
        }
    }
    return 0;
}

static void test_cache(void)
{
    char buf[MSDL_CACHE_MAX];
    unsigned long len = 0, i;
    int v = -1;
    CHECK(!msdl_cache_get(buf, 0, 1, 2, 3, &v));
    len = msdl_cache_put(buf, sizeof buf, len, 0x1234abcdul, 1459978240ul, 0x5a6b7c8dul, 1);
    CHECK(len == 29 && !memcmp(buf, "1234abcd 57058000 5a6b7c8d 1\n", 29));
    CHECK(msdl_cache_get(buf, len, 0x1234abcdul, 1459978240ul, 0x5a6b7c8dul, &v) && v == 1);
    CHECK(!msdl_cache_get(buf, len, 0x1234abcdul, 1459978241ul, 0x5a6b7c8dul, &v));      /* another size: not the same file */
    CHECK(!msdl_cache_get(buf, len, 0x1234abcdul, 1459978240ul, 0x5a6b7c8eul, &v));      /* another mtime */
    len = msdl_cache_put(buf, sizeof buf, len, 0x1234abcdul, 1459978240ul, 0x5a6b7c8dul, 0);   /* the newest line wins */
    CHECK(msdl_cache_get(buf, len, 0x1234abcdul, 1459978240ul, 0x5a6b7c8dul, &v) && v == 0);
    for (i = 0; i < 200; i++) len = msdl_cache_put(buf, sizeof buf, len, i, i, i, (int)(i % 3));   /* fills and rolls: never beyond the cap, the newest stay */
    CHECK(len <= MSDL_CACHE_MAX && len % 29 == 0);
    CHECK(msdl_cache_get(buf, len, 199, 199, 199, &v) && v == 199 % 3);
    CHECK(!msdl_cache_get(buf, len, 0, 0, 0, &v));
    memset(buf, 'x', 29);                                                                  /* a damaged file reads as empty, no crash */
    CHECK(!msdl_cache_get(buf, 29, 0, 0, 0, &v));
    CHECK(msdl_fnv("/games/Melee/game.iso") != msdl_fnv("/games/Melee/game2.iso"));
}

static void parse_mod(const char *s)
{
    char *e;
    if (!strncmp(s, "flip=", 5)) { mods[nmods].k = M_FLIP; mods[nmods].off = strtoul(s + 5, 0, 0); }
    else if (!strncmp(s, "byte=", 5)) { mods[nmods].k = M_BYTE; mods[nmods].off = strtoul(s + 5, &e, 0); mods[nmods].val = (unsigned char)strtoul(e + 1, 0, 0); }
    else if (!strncmp(s, "ascii=", 6)) { mods[nmods].k = M_ASCII; mods[nmods].off = strtoul(s + 6, &e, 0); strncpy(mods[nmods].txt, e + 1, 15); }
    else if (!strncmp(s, "zero=", 5)) { mods[nmods].k = M_ZERO; mods[nmods].off = strtoul(s + 5, &e, 0); mods[nmods].len = strtoul(e + 1, 0, 0); }
    else if (!strncmp(s, "size=", 5)) { img_size = strtoul(s + 5, 0, 0); return; }
    else { printf("bad modifier %s\n", s); exit(2); }
    nmods++;
}

int main(int argc, char **argv)
{
    int v, i;
    if (argc >= 2 && !strcmp(argv[1], "cache")) {
        test_cache();
        printf(fails ? "FAILED %d\n" : "disc cache ok\n", fails);
        return fails != 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "verdict")) {
        img = fopen(argv[2], "rb");
        if (!img) { printf("VERDICT -9 nofile\n"); return 2; }
        fseek(img, 0, SEEK_END); img_size = (unsigned long)ftell(img);
        for (i = 3; i < argc; i++) parse_mod(argv[i]);
        { struct msdl_info in; char det[80]; v = msdl_verdict_ex(rd, 0, img_size, &in); msdl_detail(v, &in, det, sizeof det);
          printf("VERDICT %d %s\nREADS %lu BYTES %lu\nFIRST %d MASK %lx ID %s REV %u DETAIL %s\n", v, msdl_reason(v), nreads, nbytes, in.first_bad, (unsigned long)in.bad_mask, in.id, (unsigned)in.rev, det); }
        return 0;
    }
    printf("usage: disc_test cache | verdict FILE [MOD ...]\n");
    return 2;
}
