/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for the card migration proof (tests/test_migration167.py).  Links the loader's ms_card_core.c AND the engine's own boot record code
 * over a raw card image, so a simulated launch writes the record the
 * way the engine does.
 *   migr_test arm RAW SEQ                          mscard_boot_arm in place; prints "ARM <1|0>"
 *   migr_test engine RAW SEQ good|crash|none       one game launch of engine build SEQ: bootrec_start (counts the start; skipped without MultishineCodeB), then for 'good' bootrec_good
 *                                                  (the Main Menu was reached); 'crash' dies after the count; 'none' = the card's exploit save never reached the engine.  In place;
 *                                                  prints "ENGINE start <rc> good <rc>"
 *   migr_test step SEQ STARTS OK CAND TRIES GOOD CODE   msupd_pend_step; prints "STEP <0 wait|1 confirmed|2 rollback|3 rearm> target <n>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_card_core.h"
#include "../../../cardloader/ota/ota_core.h"

static mw2_u8 *img;
static size_t isz;

static unsigned be16(const mw2_u8 *p) { return ((unsigned)p[0] << 8) | p[1]; }
/* the first block of a file on the image (directory and block map: the copy with the higher update counter); -1 = no such file */
static long first_block(const char *name)
{
    const mw2_u8 *d1 = img + 1 * MSCARD_BLOCK, *d2 = img + 2 * MSCARD_BLOCK, *d = be16(d2 + 0x1FFA) > be16(d1 + 0x1FFA) ? d2 : d1;
    int i;
    for (i = 0; i < 127; i++) {
        const mw2_u8 *e = d + i * 0x40;
        if (e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF) continue;
        if (!strncmp((const char *)e + 8, name, 32)) return (long)be16(e + 0x36);
    }
    return -1;
}
static int op_exists(void *c, const char *n) { (void)c; return first_block(n) >= 0; }
static int op_read(void *c, const char *n, ota_u32 off, mw2_u8 *b, ota_u32 len)
{
    long f = first_block(n); (void)c;
    if (f < 0 || off || len != OTA_BLOCK) return 3;                     /* the state file is one block: that is all the boot record needs */
    memcpy(b, img + (size_t)f * MSCARD_BLOCK, OTA_BLOCK);
    return 0;
}
static int op_write(void *c, const char *n, ota_u32 off, const mw2_u8 *b, ota_u32 len)
{
    long f = first_block(n); (void)c;
    if (f < 0 || off || len != OTA_BLOCK) return 3;
    memcpy(img + (size_t)f * MSCARD_BLOCK, b, OTA_BLOCK);
    return 0;
}
static int op_create(void *c, const char *n, ota_u32 nb) { (void)c; (void)n; (void)nb; return 1; }      /* no room: the engine's 'no state file' case (BR_NOROOM) */
static int op_del(void *c, const char *n) { (void)c; (void)n; return 3; }
static int op_rename(void *c, const char *a, const char *b) { (void)c; (void)a; (void)b; return 3; }

static int load(const char *p)
{
    FILE *f = fopen(p, "rb");
    long l;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    img = (mw2_u8 *)malloc((size_t)l); isz = (size_t)l;
    if (fread(img, 1, isz, f) != isz) { fclose(f); return 0; }
    fclose(f);
    return 1;
}
static int save(const char *p) { FILE *f = fopen(p, "wb"); if (!f) return 0; fwrite(img, 1, isz, f); fclose(f); return 1; }

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "arm")) {
        int r;
        if (!load(argv[2])) { printf("ARM -1 input\n"); return 2; }
        r = mscard_boot_arm(img, (mw2_u32)isz, (mw2_u32)strtoul(argv[3], 0, 10));
        if (r) save(argv[2]);
        printf("ARM %d\n", r);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "engine")) {
        static mw2_u8 blk[OTA_BLOCK];
        struct ota_card_ops ops;
        ota_u32 seq = (ota_u32)strtoul(argv[3], 0, 10);
        int rs = -1, rg = -1;
        if (!load(argv[2])) { printf("ENGINE -1 input\n"); return 2; }
        memset(&ops, 0, sizeof ops);
        ops.exists = op_exists; ops.read = op_read; ops.write = op_write; ops.create = op_create; ops.del = op_del; ops.rename = op_rename;
        if (strcmp(argv[4], "none")) {
            rs = bootrec_start(&ops, "MultishineState", "MultishineCodeB", blk, seq);
            if (!strcmp(argv[4], "good")) rg = bootrec_good(&ops, "MultishineState", blk, seq);
        }
        save(argv[2]);
        printf("ENGINE start %d good %d\n", rs, rg);
        return 0;
    }
    if (argc >= 9 && !strcmp(argv[1], "step")) {
        mw2_u32 t = 0;
        int d = msupd_pend_step((mw2_u32)strtoul(argv[2], 0, 10), (unsigned)strtoul(argv[3], 0, 10), atoi(argv[4]), (mw2_u32)strtoul(argv[5], 0, 10), (unsigned)strtoul(argv[6], 0, 10),
                                (unsigned)strtoul(argv[7], 0, 10), (mw2_u32)strtoul(argv[8], 0, 10), &t);
        printf("STEP %d target %lu\n", d, (unsigned long)t);
        return 0;
    }
    printf("usage: migr_test arm|engine|step ...\n");
    return 2;
}
