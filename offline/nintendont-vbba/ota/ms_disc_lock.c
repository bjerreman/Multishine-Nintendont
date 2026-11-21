/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* See ms_disc_lock.h.  Compiled into the Nintendont loader (loader/source) and into the host test. */
#include <string.h>
#include "ms_disc_lock.h"
#include "ms_disc_fp.h"
#include "ms_sha256.h"

int msdl_header_ok(const mdl_u8 *h, char *id6, mdl_u8 *rev)
{
    int i, ok;
    for (i = 0; i < 6; i++) { mdl_u8 c = h[i]; id6[i] = (char)(c >= 0x20 && c < 0x7F ? c : '?'); }
    id6[6] = 0;
    *rev = h[7];
    ok = h[0] == 'G' && h[1] == 'A' && h[2] == 'L' && h[3] == 'E' && h[4] == '0' && h[5] == '1' && h[6] == 0 && h[7] == 2
         && h[0x1C] == 0xC2 && h[0x1D] == 0x33 && h[0x1E] == 0x9F && h[0x1F] == 0x3D;
    return ok;
}

static int digest_windows(msdl_read_fn rd, void *ctx, const mdl_u32 (*w)[2], int n, mdl_u8 out[32])
{
    static mdl_u8 buf[0x1000];
    ms_sha256_ctx c;
    int i;
    ms_sha256_init(&c);
    for (i = 0; i < n; i++) {
        if (w[i][1] > sizeof buf || rd(ctx, w[i][0], buf, w[i][1])) return -1;
        ms_sha256_update(&c, buf, w[i][1]);
    }
    ms_sha256_final(&c, out);
    return 0;
}

static int window_ok(msdl_read_fn rd, void *ctx, mdl_u32 off, mdl_u32 len, const mdl_u8 *want, int *rderr)
{
    static mdl_u8 buf[0x1000];
    mdl_u8 dg[32];
    if (len > sizeof buf || rd(ctx, off, buf, len)) { *rderr = 1; return 0; }
    ms_sha256(buf, len, dg);
    return memcmp(dg, want, 32) == 0;
}

int msdl_verdict_ex(msdl_read_fn rd, void *ctx, mdl_u32 size, struct msdl_info *info)
{
    mdl_u8 hdr[0x20], dg[32];
    struct msdl_info local;
    int i, rderr = 0;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    strcpy(info->id, "??????");
    if (size < 0x440u) return MSDL_NOT_MELEE;
    if (rd(ctx, 0, hdr, 0x20)) return MSDL_READ_ERROR;
    if (!msdl_header_ok(hdr, info->id, &info->rev)) {
        if (hdr[0] == 'G' && hdr[1] == 'A' && hdr[2] == 'L' && hdr[3] == 'E' && hdr[4] == '0' && hdr[5] == '1') return MSDL_OTHER_VERSION;   /* Melee NTSC-U, another revision */
        return MSDL_NOT_MELEE;
    }
    if (size < MSDL_MIN_SIZE) return MSDL_TOO_SMALL;
    for (i = 0; i < MSDL_NCOMMON; i++)
        if (!window_ok(rd, ctx, msdl_common[i][0], msdl_common[i][1], msdl_wdg[i], &rderr)) {
            if (rderr) return MSDL_READ_ERROR;
            info->bad_mask |= 1ul << i;
            if (!info->first_bad) info->first_bad = i + 1;
        }
    if (info->bad_mask) return MSDL_MODIFIED;                                     /* a different build of v1.02 (a modded image) or damaged */
    if (digest_windows(rd, ctx, msdl_variant, MSDL_NVARIANT, dg)) return MSDL_READ_ERROR;
    if (memcmp(dg, msdl_dg_stock, 32)) { info->first_bad = MSDL_NCOMMON + 1; return MSDL_MODIFIED; }   /* the title / patch area differs: not the stock image */
    return MSDL_OK_STOCK;
}

int msdl_verdict(msdl_read_fn rd, void *ctx, mdl_u32 size) { return msdl_verdict_ex(rd, ctx, size, 0); }
int msdl_ncommon(void) { return MSDL_NCOMMON; }

void msdl_detail(int v, const struct msdl_info *info, char *out, mdl_u32 n)
{
    const char *t;
    char num[8];
    mdl_u32 i = 0;
    switch (v) {
    case MSDL_NOT_MELEE: case MSDL_OTHER_VERSION: t = "header does not match (not GALE01 revision 2)"; break;
    case MSDL_TOO_SMALL: t = "the image is cut short"; break;
    case MSDL_READ_ERROR: t = "the image could not be read"; break;
    case MSDL_MODIFIED: t = "data window "; break;
    default: t = "";
    }
    while (*t && i + 1 < n) out[i++] = *t++;
    if (v == MSDL_MODIFIED && info) {
        int d = 0, m = info->first_bad, k;
        if (!m) num[d++] = '0';
        while (m) { num[d++] = (char)('0' + m % 10); m /= 10; }
        for (k = d - 1; k >= 0 && i + 1 < n; k--) out[i++] = num[k];
        for (t = " differs"; *t && i + 1 < n; t++) out[i++] = *t;
    }
    if (n) out[i] = 0;
}

const char *msdl_reason(int v)
{
    switch (v) {
    case MSDL_OK_STOCK: return "stock Melee v1.02";
    case MSDL_NOT_MELEE: return "not Melee NTSC-U";
    case MSDL_OTHER_VERSION: return "another Melee revision";
    case MSDL_MODIFIED: return "a modified Melee image";
    case MSDL_TOO_SMALL: return "a truncated image";
    default: return "unreadable";
    }
}

mdl_u32 msdl_fnv(const char *s)
{
    mdl_u32 h = 2166136261ul;
    while (*s) { h ^= (mdl_u8)*s++; h = (h * 16777619ul) & 0xFFFFFFFFul; }
    return h;
}

static const char HEXD[] = "0123456789abcdef";
static void put_hex(char *p, mdl_u32 v) { int i; for (i = 7; i >= 0; i--) { p[i] = HEXD[v & 15u]; v >>= 4; } }
static mdl_u32 get_hex(const char *p, int *ok)
{
    mdl_u32 v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) { *ok = 0; return 0; }
        v = (v << 4) | (mdl_u32)d;
    }
    return v;
}

int msdl_cache_get(const char *buf, mdl_u32 len, mdl_u32 ph, mdl_u32 size, mdl_u32 mt, int *v)
{
    mdl_u32 i = 0;
    int found = 0;
    while (i + 29u <= len) {                         /* "PPPPPPPP SSSSSSSS MMMMMMMM V\n" = 29 bytes */
        int ok = 1;
        mdl_u32 p = get_hex(buf + i, &ok), s = get_hex(buf + i + 9, &ok), m = get_hex(buf + i + 18, &ok);
        char d = buf[i + 27];
        if (ok && buf[i + 8] == ' ' && buf[i + 17] == ' ' && buf[i + 26] == ' ' && (d == '0' || d == '1' || d == '2') && buf[i + 28] == '\n') {
            if (p == ph && s == size && m == mt) { *v = d - '0'; found = 1; }      /* the last matching line wins */
        }
        i += 29u;
    }
    return found;
}

mdl_u32 msdl_cache_put(char *buf, mdl_u32 cap, mdl_u32 len, mdl_u32 ph, mdl_u32 size, mdl_u32 mt, int v)
{
    mdl_u32 drop = 0;
    len -= len % 29u;
    if (len + 29u > cap) { mdl_u32 need = len + 29u - cap; drop = ((need + 28u) / 29u) * 29u; }
    if (drop > len) drop = len;
    if (drop) { memmove(buf, buf + drop, len - drop); len -= drop; }
    put_hex(buf + len, ph); buf[len + 8] = ' ';
    put_hex(buf + len + 9, size); buf[len + 17] = ' ';
    put_hex(buf + len + 18, mt); buf[len + 26] = ' ';
    buf[len + 27] = (char)('0' + v); buf[len + 28] = '\n';
    return len + 29u;
}
