/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Nintendont OTA, the pure part: see ms_ota_core.h.  C89, no libc. */
#include "ms_ota_core.h"
#include "mscf_verify.h"

static mw2_u32 be32(const mw2_u8 *p) { return ((mw2_u32)p[0] << 24) | ((mw2_u32)p[1] << 16) | ((mw2_u32)p[2] << 8) | (mw2_u32)p[3]; }
static unsigned be16(const mw2_u8 *p) { return ((unsigned)p[0] << 8) | (unsigned)p[1]; }
static void put32(mw2_u8 *p, mw2_u32 v) { p[0] = (mw2_u8)(v >> 24); p[1] = (mw2_u8)(v >> 16); p[2] = (mw2_u8)(v >> 8); p[3] = (mw2_u8)v; }

mw2_u32 msota_crc32(const mw2_u8 *b, mw2_u32 n)
{
    mw2_u32 c = 0xFFFFFFFFu, i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= b[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (mw2_u32)(0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static mw2_u32 crc32_cont(const mw2_u8 *b, mw2_u32 n, mw2_u32 crc)      /* zlib-style continuation: crc32(tabs, crc32(header)) */
{
    mw2_u32 c = crc ^ 0xFFFFFFFFu, i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= b[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (mw2_u32)(0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

const char *msota_err(int e)
{
    switch (e) {
    case MSOTA_OK: return "ok";
    case MSOTA_E_SIZE: return "size";
    case MSOTA_E_HDR: return "header";
    case MSOTA_E_ABI: return "ABI";
    case MSOTA_E_SEQ: return "older than installed";
    case MSOTA_E_CRC: return "CRC";
    case MSOTA_E_SIG: return "signature";
    case MSOTA_E_PAYLOAD: return "payload";
    case MSOTA_E_SHELL: return "needs a newer shell";
    case MSOTA_E_DOL: return "shell image";
    default: return "?";
    }
}

/* the container checks the kinds share: size, header, ABI, seq, header CRC, payload CRC, signature, nothing after the payload.  -> MSOTA_OK with *img / *ilen / *seq */
int msota_check_container(const mw2_u8 *file, mw2_u32 len, mw2_u32 max_file, unsigned abi, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq,
                           const mw2_u8 **img_out, mw2_u32 *ilen_out, mw2_u32 *seq_out)
{
    const mw2_u8 *h, *img;
    mw2_u32 seq, flags, ilen, icrc, nseg, npatch, k;
    if (len < 0x4000u || (len & 0x1FFFu) || len > max_file) return MSOTA_E_SIZE;
    h = file + 0x1000u;
    if (be32(h) != 0x4D534346u || be16(h + 4) != 1u) return MSOTA_E_HDR;          /* 'MSCF', plain version 1 */
    if (be16(h + 6) != abi) return MSOTA_E_ABI;
    seq = be32(h + 8); flags = be32(h + 0x0C); ilen = be32(h + 0x18); icrc = be32(h + 0x1C); nseg = be32(h + 0x20); npatch = be32(h + 0x24);
    if (flags || nseg || npatch || be32(h + 0x28) || ilen == 0 || ilen > len - 0x2000u) return MSOTA_E_HDR;
    if (seq < min_seq || seq == 0) return MSOTA_E_SEQ;
    if (crc32_cont(h + 0x2C, 0, msota_crc32(h, 0x2Cu)) != be32(h + 0x2C)) return MSOTA_E_HDR;     /* header CRC over bytes 0..0x2B (no tables) */
    img = file + 0x2000u;
    if (msota_crc32(img, ilen) != icrc) return MSOTA_E_CRC;
    if (!mscf_verify_sig(h, h + 0x80, 0, img, ilen, pubs, npub, 0, 0)) return MSOTA_E_SIG;
    for (k = 0x2000u + ilen; k < len; k++)                                       /* nothing after the payload but the block padding */
        if (file[k]) return MSOTA_E_PAYLOAD;
    *img_out = img; *ilen_out = ilen; *seq_out = seq;
    return MSOTA_OK;
}

int msota_check(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct msota_kernel *out)
{
    const mw2_u8 *img;
    mw2_u32 seq, ilen, total, klen, kcrc;
    int r = msota_check_container(file, len, MSOTA_MAX_FILE, MSOTA_ABI, pubs, npub, min_seq, &img, &ilen, &seq);
    if (r != MSOTA_OK) return r;
    if (ilen < 33u || img[0] != 'M' || img[1] != 'K' || img[2] != 'R' || img[3] != 'N' || img[4] != 1 || img[5] != 0 || img[6] || img[7]) return MSOTA_E_PAYLOAD;
    total = be32(img + 8); klen = be32(img + 12); kcrc = be32(img + 16);
    if (total != ilen || klen != ilen - 32u || klen == 0 || klen > MSOTA_MAX_KERNEL || be32(img + 24) || be32(img + 28)) return MSOTA_E_PAYLOAD;
    if (msota_crc32(img + 32, klen) != kcrc) return MSOTA_E_CRC;
    if (be32(img + 20) > MSOTA_SHELL_ABI) return MSOTA_E_SHELL;
    if (out) { out->zip = img + 32; out->zip_len = klen; out->seq = seq; }
    return MSOTA_OK;
}

int msota_check_shell(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct msota_shell *out)
{
    const mw2_u8 *img;
    mw2_u32 seq, ilen, total, dlen, dcrc;
    int r = msota_check_container(file, len, MSOTA_MAX_SHELL_FILE, MSOTA_SHL_ABI, pubs, npub, min_seq, &img, &ilen, &seq);
    if (r != MSOTA_OK) return r;
    if (ilen < 33u || img[0] != 'M' || img[1] != 'S' || img[2] != 'H' || img[3] != 'L' || img[4] != 1 || img[5] != 0 || img[6] || img[7]) return MSOTA_E_PAYLOAD;
    total = be32(img + 8); dlen = be32(img + 12); dcrc = be32(img + 16);
    if (total != ilen || dlen != ilen - 32u || dlen == 0 || dlen > MSOTA_MAX_DOL || be32(img + 24) || be32(img + 28)) return MSOTA_E_PAYLOAD;
    if (msota_crc32(img + 32, dlen) != dcrc) return MSOTA_E_CRC;
    if (be32(img + 20) > MSOTA_STUB_ABI) return MSOTA_E_SHELL;
    if (msdol_check(img + 32, dlen, 0) != MSDOL_OK) return MSOTA_E_DOL;
    if (out) { out->dol = img + 32; out->dol_len = dlen; out->seq = seq; }
    return MSOTA_OK;
}

const char *msdol_err(int e)
{
    switch (e) {
    case MSDOL_OK: return "ok";
    case MSDOL_E_SIZE: return "size";
    case MSDOL_E_SECTION: return "section";
    case MSDOL_E_ADDR: return "address";
    case MSDOL_E_BSS: return "bss";
    case MSDOL_E_ENTRY: return "entry";
    case MSDOL_E_OVERLAP: return "overlap";
    default: return "?";
    }
}

/* the DOL header: 18 u32 file offsets (7 text, 11 data) at 0x00, 18 load addresses at 0x48, 18 sizes at 0x90, bss address / size at 0xD8 / 0xDC, entry at 0xE0 */
int msdol_check(const mw2_u8 *dol, mw2_u32 len, struct msdol_plan *plan)
{
    struct msdol_plan p;
    mw2_u32 lo[MSDOL_NSEC + 1], hi[MSDOL_NSEC + 1], t;
    unsigned i, j, n = 0, nr = 0;
    int text = 0, entry_ok = 0;
    if (len < 0x100u || len > MSOTA_MAX_DOL) return MSDOL_E_SIZE;
    p.n = 0;
    p.bss = be32(dol + 0xD8); p.bss_len = be32(dol + 0xDC); p.entry = be32(dol + 0xE0);
    for (i = 0; i < MSDOL_NSEC; i++) {
        mw2_u32 off = be32(dol + 4u * i), addr = be32(dol + 0x48u + 4u * i), size = be32(dol + 0x90u + 4u * i);
        if (size == 0) continue;
        if (off < 0x100u || (off & 3u) || (addr & 3u) || (size & 3u) || size > len || off > len - size) return MSDOL_E_SECTION;
        if (addr < MSDOL_LO || addr > MSDOL_HI || size > MSDOL_HI - addr) return MSDOL_E_ADDR;
        p.sec[n].src = off; p.sec[n].dst = addr; p.sec[n].len = size; n++;
        lo[nr] = addr; hi[nr] = addr + size; nr++;
        if (i < 7u) {
            text = 1;
            if (p.entry >= addr && p.entry < addr + size) entry_ok = 1;
        }
    }
    if (p.bss_len) {
        if ((p.bss & 3u) || (p.bss_len & 3u) || p.bss < MSDOL_LO || p.bss > MSDOL_HI || p.bss_len > MSDOL_HI - p.bss) return MSDOL_E_BSS;
        lo[nr] = p.bss; hi[nr] = p.bss + p.bss_len; nr++;
    }
    if (!text || !entry_ok) return MSDOL_E_ENTRY;
    for (i = 0; i < nr; i++)                                    /* no two destination ranges overlap */
        for (j = i + 1; j < nr; j++)
            if (lo[i] < hi[j] && lo[j] < hi[i]) return MSDOL_E_OVERLAP;
    (void)t;
    p.n = n;
    if (plan) *plan = p;
    return MSDOL_OK;
}

void msota_state_init(struct msota_state *st) { st->floor = st->active = st->bad = 0; st->pending = st->fail = 0; }

int msota_state_parse(struct msota_state *st, const mw2_u8 *b, mw2_u32 n)
{
    msota_state_init(st);
    if (n < MSOTA_ST_LEN || b[0] != 'M' || b[1] != 'K' || b[2] != 'O' || b[3] != 'S' || b[4] != 1) return 0;
    if (msota_crc32(b, 20u) != be32(b + 20) || b[5] > 1u || b[6] > 100u || b[7]) return 0;
    st->pending = b[5]; st->fail = b[6]; st->floor = be32(b + 8); st->active = be32(b + 12); st->bad = be32(b + 16);
    return 1;
}

void msota_state_pack(const struct msota_state *st, mw2_u8 out[MSOTA_ST_LEN])
{
    out[0] = 'M'; out[1] = 'K'; out[2] = 'O'; out[3] = 'S'; out[4] = 1; out[5] = st->pending; out[6] = st->fail; out[7] = 0;
    put32(out + 8, st->floor); put32(out + 12, st->active); put32(out + 16, st->bad);
    put32(out + 20, msota_crc32(out, 20u));
}

int msota_decide(struct msota_state *st, int file_ok, mw2_u32 file_seq, int prev_confirmed, int *action)
{
    int a = 0;
    if (st->pending) {                                  /* the last start used the downloaded kernel */
        if (prev_confirmed) { st->pending = 0; st->fail = 0; }
        else if (st->fail < 100u) st->fail++;
    }
    if (!file_ok) { st->pending = 0; if (action) *action = 0; return MSOTA_PICK_EMBEDDED; }
    if (file_seq < st->floor) { st->pending = 0; if (action) *action = 2; return MSOTA_PICK_EMBEDDED; }
    if (st->fail >= MSOTA_MAX_FAIL) {                   /* two starts in a row never reached the game: drop it and remember which one */
        st->bad = file_seq; st->pending = 0; st->fail = 0; st->active = 0;
        if (action) *action = 1;
        return MSOTA_PICK_EMBEDDED;
    }
    if (file_seq == st->bad) { st->pending = 0; if (action) *action = 2; return MSOTA_PICK_EMBEDDED; }
    st->active = file_seq;
    if (file_seq > st->floor) st->floor = file_seq;
    if (!st->pending) st->fail = 0;
    st->pending = 1;
    if (action) *action = a;
    return MSOTA_PICK_DOWNLOADED;
}

int msota_alive_due(int pads_up, int sd_ok, int screen_drawn, int already)
{
    return !already && pads_up && sd_ok && screen_drawn;
}

int msota_has_runid(const mw2_u8 *b, mw2_u32 n)
{
    static const char t[] = "RUNID: game";
    mw2_u32 i;
    unsigned k;
    if (n < 11u) return 0;
    for (i = 0; i + 11u <= n; i++) {
        for (k = 0; k < 11u && b[i + k] == (mw2_u8)t[k]; k++) {}
        if (k == 11u) return 1;
    }
    return 0;
}

int msota_http_head(const mw2_u8 *b, mw2_u32 n, mw2_u32 *status, mw2_u32 *clen, mw2_u32 *hdr_end)
{
    mw2_u32 k, j, e = 0, v;
    *status = 0; *clen = 0xFFFFFFFFu; *hdr_end = 0;
    for (k = 0; k + 3u < n; k++)
        if (b[k] == '\r' && b[k + 1] == '\n' && b[k + 2] == '\r' && b[k + 3] == '\n') { e = k + 4u; break; }
    if (!e) return -1;
    if (e > 12u && b[0] == 'H' && b[1] == 'T' && b[2] == 'T' && b[3] == 'P' && b[4] == '/' && b[8] == ' ' && b[9] >= '0' && b[9] <= '9') {
        v = 0;
        for (j = 9u; j < 12u && b[j] >= '0' && b[j] <= '9'; j++) v = v * 10u + (mw2_u32)(b[j] - '0');
        *status = v;
    }
    for (j = 0; j + 15u < e; j++) {                                   /* "Content-Length:" at the start of a line */
        static const char t[] = "content-length:";
        unsigned i;
        if (j && b[j - 1] != '\n') continue;
        for (i = 0; i < 15u; i++) {
            mw2_u8 ch = b[j + i];
            if (ch >= 'A' && ch <= 'Z') ch = (mw2_u8)(ch + 32);
            if (ch != (mw2_u8)t[i]) break;
        }
        if (i == 15u) {
            mw2_u32 p = j + 15u;
            while (p < e && b[p] == ' ') p++;
            if (p < e && b[p] >= '0' && b[p] <= '9') {
                v = 0;
                while (p < e && b[p] >= '0' && b[p] <= '9') { if (v > 0x19999999u) return -1; v = v * 10u + (mw2_u32)(b[p] - '0'); p++; }
                *clen = v;
            }
            break;
        }
    }
    *hdr_end = e;
    return 0;
}

int msota_parse_ip(const char *s, mw2_u32 *ip, unsigned *port)
{
    mw2_u32 a[4], v;
    unsigned k;
    const char *p = s;
    *port = 0;
    for (k = 0; k < 4u; k++) {
        v = 0;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') { v = v * 10u + (mw2_u32)(*p - '0'); if (v > 255u) return -1; p++; }
        a[k] = v;
        if (k < 3u) { if (*p != '.') return -1; p++; }
    }
    if (*p == ':') {
        v = 0; p++;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') { v = v * 10u + (mw2_u32)(*p - '0'); if (v > 65535u) return -1; p++; }
        *port = (unsigned)v;
    }
    if (*p != 0 && *p != '/') return -1;
    *ip = (a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
    return 0;
}

mw2_u32 msota_json_uint(const char *s, const char *key)
{
    unsigned k;
    mw2_u32 v = 0;
    for (; *s; s++) {
        for (k = 0; key[k] && s[k] == key[k]; k++) {}
        if (!key[k]) {
            s += k;
            while (*s == ' ') s++;
            while (*s >= '0' && *s <= '9') { if (v > 0x19999999u) return 0; v = v * 10u + (mw2_u32)(*s - '0'); s++; }
            return v;
        }
    }
    return 0;
}

/* the first line of the Update screen */
void msota_fmt_header(char *out, unsigned long n, unsigned long kernel, unsigned long shell)
{
    unsigned long i = 0, k;
    const char *p;
    char num[16];
    static const char a[] = "Multishine update (kernel ", b[] = ", shell ", e[] = ")";
    for (p = a; *p && i + 1 < n; p++) out[i++] = *p;
    for (k = 0; k < 2; k++) {
        unsigned long v = k ? shell : kernel;
        int m = 0, d;
        if (!v) num[m++] = '0';
        while (v) { num[m++] = (char)('0' + v % 10u); v /= 10u; }
        for (d = m - 1; d >= 0 && i + 1 < n; d--) out[i++] = num[d];
        for (p = k ? e : b; *p && i + 1 < n; p++) out[i++] = *p;
    }
    if (n) out[i] = 0;
}

/* the back-off of the boot check (SD:/multishine/ota.chk 'next <epoch>'): see ms_ota_core.h */
mw2_u32 msota_backoff(int event, mw2_u32 now, mw2_u32 cur)
{
    switch (event) {
    case MSOTA_BO_NET_FAIL: return now + MSOTA_BO_NET_S;
    case MSOTA_BO_SERVER_FAIL: return now + MSOTA_BO_SERVER_S;
    case MSOTA_BO_SUCCESS: case MSOTA_BO_INSTALLED: return 0;
    default: return cur;                                  /* a skip by the user, anything unknown: nothing changes */
    }
}

/* swapping a verified file into place: bin <- knew, the old bin kept as bak.  The loader's f_rename (and f_close) can report an error although the data is written (its disk layer has no
   sync call), so the outcome is decided by what is on the card afterwards, never by the return code alone. */
int msota_swap_in(const struct msota_fs *fs, const char *bin, const char *knew, const char *bak)
{
    int r;
    fs->unlink(fs->ctx, bak);                                           /* nothing to do when there is no old backup */
    if (fs->exists(fs->ctx, bin)) {                                     /* a first install has no old file: nothing to move aside */
        r = fs->rename(fs->ctx, bin, bak);
        if (r && (fs->exists(fs->ctx, bin) || !fs->exists(fs->ctx, bak))) return MSOTA_SW_BAK;        /* it really did not move: the new file stays as it is, nothing is changed */
    }
    r = fs->rename(fs->ctx, knew, bin);
    if (r && (fs->exists(fs->ctx, knew) || !fs->exists(fs->ctx, bin))) {                              /* it really did not move */
        if (fs->exists(fs->ctx, bak) && !fs->exists(fs->ctx, bin)) fs->rename(fs->ctx, bak, bin);   /* put the old file back */
        return MSOTA_SW_FAIL;
    }
    return MSOTA_SW_OK;
}

/* ---- the DNS fallback: a raw UDP A query and a defensive parser; the socket work is in the loader --------------------------------------------- */
mw2_u32 msdns_query(mw2_u8 *out, mw2_u32 cap, unsigned id, const char *name)
{
    mw2_u32 n = 12, lab, k;
    const char *p = name;
    if (cap < 18u) return 0;
    for (k = 0; k < 12u; k++) out[k] = 0;
    out[0] = (mw2_u8)(id >> 8); out[1] = (mw2_u8)id; out[2] = 0x01;          /* a standard query, recursion desired */
    out[5] = 1;                                                              /* one question */
    while (*p) {
        const char *e = p;
        while (*e && *e != '.') e++;
        lab = (mw2_u32)(e - p);
        if (lab == 0 || lab > 63u || n + lab + 1u + 5u > cap) return 0;
        out[n++] = (mw2_u8)lab;
        for (k = 0; k < lab; k++) out[n++] = (mw2_u8)p[k];
        p = *e ? e + 1 : e;
    }
    out[n++] = 0;
    out[n++] = 0; out[n++] = 1;                                              /* type A */
    out[n++] = 0; out[n++] = 1;                                              /* class IN */
    return n;
}

/* addresses a name server must not point us at: 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.168/16, 224/4 and 240/4 (so a poisoned answer cannot aim at the LAN) */
int msdns_ip_ok(mw2_u32 ip)
{
    mw2_u32 a = ip >> 24, b = (ip >> 16) & 255u;
    if (a == 0 || a == 10 || a == 127 || a >= 224) return 0;
    if (a == 100 && b >= 64 && b <= 127) return 0;
    if (a == 169 && b == 254) return 0;
    if (a == 172 && b >= 16 && b <= 31) return 0;
    if (a == 192 && b == 168) return 0;
    return 1;
}

/* expand the (possibly compressed) name at *off into out as 'a.b.c' (lower case); *off is left after the name as it stands in the packet (after the first pointer).  0 = ok.  Bounds, label
   lengths, pointer hops and the total length are all checked. */
static int dns_name(const mw2_u8 *p, mw2_u32 n, mw2_u32 *off, char *out, mw2_u32 cap)
{
    mw2_u32 pos = *off, outn = 0, hops = 0, end = 0, lab, k;
    int jumped = 0;
    for (;;) {
        if (pos >= n) return -1;
        lab = p[pos];
        if ((lab & 0xC0u) == 0xC0u) {
            mw2_u32 to;
            if (pos + 1u >= n) return -1;
            to = ((lab & 0x3Fu) << 8) | p[pos + 1u];
            if (!jumped) { end = pos + 2u; jumped = 1; }
            if (++hops > 8u || to >= n) return -1;
            pos = to;
            continue;
        }
        if (lab & 0xC0u) return -1;                                          /* the reserved label types */
        pos++;
        if (lab == 0) break;
        if (pos + lab > n || outn + lab + 2u > cap) return -1;
        if (outn) out[outn++] = '.';
        for (k = 0; k < lab; k++) { mw2_u8 ch = p[pos + k]; out[outn++] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); }
        pos += lab;
    }
    out[outn] = 0;
    *off = jumped ? end : pos;
    return 0;
}

static int dns_same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

const char *msdns_err(int e)
{
    switch (e) {
    case MSDNS_OK: return "ok";
    case MSDNS_E_SHORT: return "short packet";
    case MSDNS_E_ID: return "wrong id";
    case MSDNS_E_FLAGS: return "not a clean answer";
    case MSDNS_E_QUESTION: return "question differs";
    case MSDNS_E_NOA: return "no usable A record";
    default: return "malformed";
    }
}

int msdns_parse(const mw2_u8 *p, mw2_u32 n, unsigned id, const char *name, mw2_u32 *ip)
{
    mw2_u32 off = 12, an, i, rd, ty, cl;
    char qn[256], on[256], cn[256];
    unsigned flags;
    if (n < 12u) return MSDNS_E_SHORT;
    if ((((unsigned)p[0] << 8) | p[1]) != (id & 0xFFFFu)) return MSDNS_E_ID;
    flags = ((unsigned)p[2] << 8) | p[3];
    if (!(flags & 0x8000u) || (flags & 0x7800u) || (flags & 0x000Fu)) return MSDNS_E_FLAGS;          /* a response, a standard query's, no error code (a truncated one is read as far as it goes) */
    if ((((unsigned)p[4] << 8) | p[5]) != 1u) return MSDNS_E_QUESTION;
    an = ((mw2_u32)p[6] << 8) | p[7];
    if (an > 40u) an = 40u;
    if (dns_name(p, n, &off, qn, sizeof qn)) return MSDNS_E_FORMAT;
    if (off + 4u > n) return MSDNS_E_SHORT;
    if (!dns_same(qn, name) || p[off] != 0 || p[off + 1] != 1 || p[off + 2] != 0 || p[off + 3] != 1) return MSDNS_E_QUESTION;
    off += 4u;
    { mw2_u32 k = 0; while (qn[k] && k + 1 < sizeof cn) { cn[k] = qn[k]; k++; } cn[k] = 0; }          /* the name the answers must be for: the question, then each CNAME target */
    for (i = 0; i < an; i++) {
        if (dns_name(p, n, &off, on, sizeof on)) return i ? MSDNS_E_NOA : MSDNS_E_FORMAT;
        if (off + 10u > n) return MSDNS_E_NOA;                                                         /* truncated: nothing usable before this point */
        ty = ((mw2_u32)p[off] << 8) | p[off + 1];
        cl = ((mw2_u32)p[off + 2] << 8) | p[off + 3];
        rd = ((mw2_u32)p[off + 8] << 8) | p[off + 9];
        off += 10u;
        if (off + rd > n) return MSDNS_E_NOA;
        if (cl == 1u && dns_same(on, cn)) {
            if (ty == 5u) {                                                                            /* CNAME: follow it */
                mw2_u32 o2 = off, k = 0;
                char tg[256];
                if (dns_name(p, n, &o2, tg, sizeof tg) || o2 > off + rd) return MSDNS_E_FORMAT;
                while (tg[k] && k + 1 < sizeof cn) { cn[k] = tg[k]; k++; }
                cn[k] = 0;
            } else if (ty == 1u && rd == 4u) {
                mw2_u32 a = ((mw2_u32)p[off] << 24) | ((mw2_u32)p[off + 1] << 16) | ((mw2_u32)p[off + 2] << 8) | p[off + 3];
                if (msdns_ip_ok(a)) { *ip = a; return MSDNS_OK; }                                      /* the first public A of the chain */
            }
        }
        off += rd;
    }
    return MSDNS_E_NOA;
}

/* the resolver loop on top of the parser, with the socket, the clock and the waiting injected (the loader backs them with a non-blocking UDP socket, the host test with a script) */
int msdns_resolve(const struct msdns_io *io, unsigned id, const char *name, mw2_u32 timeout_ms, unsigned tries, mw2_u32 budget_ms, mw2_u32 *ip, int *last_parse)
{
    mw2_u8 q[300], r[512];
    mw2_u32 qn = msdns_query(q, sizeof q, id, name), t_all = io->now(io->ctx), t1;
    unsigned t;
    int pr = MSDNS_E_SHORT;
    unsigned errs = 0;
    if (!qn) return -2;
    for (t = 0; t < tries; t++) {
        long n;
        if (io->send(io->ctx, q, qn) < 0) return -2;
        t1 = io->now(io->ctx);
        while ((mw2_u32)(io->now(io->ctx) - t1) < timeout_ms && (mw2_u32)(io->now(io->ctx) - t_all) < budget_ms) {
            n = io->recv(io->ctx, r, sizeof r);
            if (n < 0) { if (++errs > 20u) return -2; io->wait(io->ctx, 10u); continue; }              /* an odd error from the socket layer is waited out (20 in a row = the socket is broken) */
            errs = 0;
            if (n == 0) { io->wait(io->ctx, 10u); continue; }
            pr = msdns_parse(r, (mw2_u32)n, id, name, ip);
            if (last_parse) *last_parse = pr;
            if (pr == MSDNS_OK) return 0;
            if (pr == MSDNS_E_FLAGS || pr == MSDNS_E_NOA) return -3;                          /* the resolver said no (NXDOMAIN, SERVFAIL, nothing usable): trying again will not help */
            /* a wrong id, a short or malformed packet: not the answer to our question, keep waiting */
        }
        if ((mw2_u32)(io->now(io->ctx) - t_all) >= budget_ms) break;
    }
    if (last_parse) *last_parse = pr;
    return -1;
}

/* SD:/multishine/ota.ip: "A.B.C.D EPOCH" = the last address the name resolved to that worked.  Used as a second choice before the baked address; ignored when older than 30 days or not public. */
int msota_ipcache_parse(const char *txt, mw2_u32 now, mw2_u32 *ip)
{
    mw2_u32 a = 0, ep = 0;
    unsigned port = 0;
    const char *p = txt;
    {   /* the first token is the address (parse_ip wants the text to end after it) */
        char tok[24];
        unsigned k = 0;
        while (p[k] && p[k] != ' ' && p[k] != '\n' && k + 1 < sizeof tok) { tok[k] = p[k]; k++; }
        tok[k] = 0;
        if (msota_parse_ip(tok, &a, &port) != 0 || port) return 0;
    }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') { if (ep > 0x19999999u) return 0; ep = ep * 10u + (mw2_u32)(*p - '0'); p++; }
    if (!msdns_ip_ok(a)) return 0;
    if (now && ep && (now < ep || now - ep > 30u * 86400u)) return 0;                              /* from the future (clock) or older than 30 days */
    *ip = a;
    return 1;
}

int msota_ipcache_fmt(char *out, unsigned n, mw2_u32 ip, mw2_u32 now)
{
    unsigned i = 0, k;
    mw2_u32 v;
    char d[12];
    for (k = 0; k < 5u; k++) {
        int m = 0;
        v = k < 4u ? (ip >> (24 - 8 * k)) & 255u : now;
        if (!v) d[m++] = '0';
        while (v) { d[m++] = (char)('0' + v % 10u); v /= 10u; }
        while (m && i + 2 < n) out[i++] = d[--m];
        if (i + 2 < n) out[i++] = k < 3u ? '.' : (k == 3u ? ' ' : '\n');
    }
    if (n) out[i] = 0;
    return (int)i;
}

/* ---- writing a downloaded file to the SD in checked chunks, and the 'SD card full' message ---- */
int msota_write_chunks(const struct msota_wio *io, const mw2_u8 *data, mw2_u32 len, mw2_u32 chunk, mw2_u32 sync_every, mw2_u32 *at, int *rc, mw2_u32 *sync_fail)
{
    mw2_u32 off = 0, since = 0, put, n;
    int r;
    *at = 0; *rc = 0; *sync_fail = 0;
    if (!chunk) chunk = 0x8000u;
    while (off < len) {
        n = len - off < chunk ? len - off : chunk;
        put = 0;
        r = io->write(io->ctx, data + off, n, &put);
        if (r) { *at = off; *rc = r; return MSOTA_W_WRITE; }
        if (put != n) { *at = off + put; *rc = 0; return MSOTA_W_SHORT; }
        off += n; since += n;
        if (sync_every && since >= sync_every && off < len) { if (io->sync(io->ctx)) (*sync_fail)++; since = 0; }       /* a sync that fails is counted, not fatal: this loader's disk layer has no sync call */
    }
    if (io->sync(io->ctx)) (*sync_fail)++;
    *at = len;
    return MSOTA_W_OK;
}

static unsigned put_num(char *out, unsigned i, unsigned n, mw2_u32 v)
{
    char d[12];
    int m = 0;
    if (!v) d[m++] = '0';
    while (v) { d[m++] = (char)('0' + v % 10u); v /= 10u; }
    while (m && i + 1 < n) out[i++] = d[--m];
    return i;
}

/* "SD card full: 12 MB free, 3 MB needed" (sizes rounded up to whole MB, at least 1) */
void msota_fmt_full(char *out, unsigned n, mw2_u32 free_bytes, mw2_u32 need_bytes)
{
    unsigned i = 0;
    const char *p;
    mw2_u32 f = (free_bytes + 0xFFFFFu) >> 20, d = (need_bytes + 0xFFFFFu) >> 20;
    for (p = "SD card full: "; *p && i + 1 < n; p++) out[i++] = *p;
    i = put_num(out, i, n, f);
    for (p = " MB free, "; *p && i + 1 < n; p++) out[i++] = *p;
    i = put_num(out, i, n, d);
    for (p = " MB needed"; *p && i + 1 < n; p++) out[i++] = *p;
    if (n) out[i] = 0;
}

/* 0 = enough room (the file plus a little slack for the directory and the backup swap), MSOTA_E_FULL otherwise */
int msota_space_ok(mw2_u32 free_bytes, mw2_u32 need_bytes)
{
    mw2_u32 want = need_bytes + 0x40000u;
    return (want < need_bytes || free_bytes >= want) ? 0 : 1;
}

/* ---- the in-place hand-over to a freshly installed shell: the decision and the Homebrew Channel argument block are pure ---- */
int msota_chain_decide(const struct msota_chain_in *in)
{
    if (!in->installed) return MSOTA_CH_NOINST;
    if (!in->early && !in->reload_ok) return MSOTA_CH_LATE; /* after the IOS58 reload the hand-over goes through a clean IOS58 reload first (the state the stub hands over); without that route: no */
    if (in->marker) return MSOTA_CH_GUARD;                 /* this start already is a hand-over: never twice in one power-on */
    if (in->disabled) return MSOTA_CH_DISABLED;            /* the no_chain marker, or an earlier hand-over that never confirmed */
    if (!in->verified) return MSOTA_CH_NOVERIFY;           /* signature / sequence checked again from the card */
    if (!in->dol_ok) return MSOTA_CH_DOL;                  /* msdol_check refused the DOL */
    if (!in->mem2_ok) return MSOTA_CH_MEM;                 /* no MEM2 buffer for it */
    return MSOTA_CH_GO;
}

int msota_restart_decide(const struct msota_restart_in *in)
{
    if (!in->installed) return MSOTA_RS_NONE;
    if (!in->verified) return MSOTA_RS_BAD;
    if (in->reboot_marker && in->reboot_ok) return MSOTA_RS_REBOOT;
    if (in->inplace_ok) return MSOTA_RS_INPLACE;
    return MSOTA_RS_TEXT;
}

int msota_chain_is_late(mw2_u32 guard, mw2_u32 flavour) { return guard == MSOTA_CHAIN_GUARD && flavour == MSOTA_CHAIN_LATE; }

void msota_chain_take(volatile mw2_u32 *m, int *marker, int *late)
{
    mw2_u32 g = m[0], f = m[1];
    *marker = g == MSOTA_CHAIN_GUARD;
    *late = msota_chain_is_late(g, f);
    m[0] = 0; m[1] = 0;
}

int msota_kernel_probe(volatile mw2_u32 *rs, volatile mw2_u32 *hid_change, volatile mw2_u32 *hid_size, unsigned polls, void (*wait)(void *ctx), void *ctx)
{
    unsigned i;
    if (*hid_change != 0) { *hid_size = 0; *hid_change = 0; }
    *rs = MSOTA_KPROBE;
    for (i = 0; i < polls; i++) {
        if (*rs == 0) return 1;
        if (*hid_change != 0) { *hid_size = 0; *hid_change = 0; }
        if (wait) wait(ctx);
    }
    if (*rs == 0) return 1;
    *rs = 0;
    return 0;
}

const char *msota_chain_name(int d)
{
    switch (d) {
    case MSOTA_CH_GO: return "go";
    case MSOTA_CH_NOINST: return "no shell was installed";
    case MSOTA_CH_LATE: return "late, and no IOS58 reload route";
    case MSOTA_CH_GUARD: return "already a hand-over in this power-on";
    case MSOTA_CH_DISABLED: return "switched off (no_chain)";
    case MSOTA_CH_NOVERIFY: return "the installed file did not verify again";
    case MSOTA_CH_DOL: return "the DOL rules refused it";
    case MSOTA_CH_MEM: return "no MEM2 buffer";
    default: return "?";
    }
}

/* the Homebrew Channel's command line: every argument NUL terminated, one more NUL at the end; stops before the block would exceed cap.  -> the length, 0 when there is no argv[0] */
mw2_u32 msota_args_build(char *dst, mw2_u32 cap, int argc, const char *const *argv)
{
    mw2_u32 len = 0, l, k;
    int i;
    if (argc < 1 || !argv || !argv[0]) return 0;
    for (i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (!s) break;
        for (l = 0; s[l]; l++) {}
        l++;
        if (len + l + 1u > cap) break;
        for (k = 0; k < l; k++) dst[len + k] = s[k];
        len += l;
    }
    if (!len || len + 1u > cap) return 0;                   /* not even argv[0] fitted */
    dst[len++] = 0;
    return len;
}
