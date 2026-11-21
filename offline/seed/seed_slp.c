/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* seed_slp: see seed_slp.h.  C89 plus long long; no libc.  The algorithms are ports (same decisions, same tie-breaks) of the project's other tooling, tools/payload_pack/mslz_opt.c, the project's other tooling,
 * the project's other tooling (fix_item_names) and the project's other tooling (make_pack): the output must stay byte-identical to the PC tool. */
#include "seed_slp.h"
#include "../payload_pack/ms_lz.h"

typedef slp_u8 u8;
typedef slp_u32 u32;
typedef slp_s64 s64;

#define BLOCK 0x2000u
#define HB (1 << 20)
#define MINM 4
#define MAXOFF 65535
#define INF 0x3fffffff

static void mcopy(u8 *d, const u8 *s, u32 n) { u32 i; for (i = 0; i < n; i++) d[i] = s[i]; }
static void mzero(void *p, u32 n) { u8 *c = (u8 *)p; u32 i; for (i = 0; i < n; i++) c[i] = 0; }
static u32 rup(u32 x, u32 a) { return (x + a - 1u) / a * a; }

/* ---- CRC-32 (zlib) ---- */
u32 slp_crc32(u32 crc, const u8 *p, u32 n)
{
    static const u32 T[16] = { 0, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
                               0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };
    u32 c = ~crc, i;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}

static u32 adler32(const u8 *p, u32 n)
{
    u32 a = 1, b = 0, i;
    for (i = 0; i < n; i++) { a += p[i]; if (a >= 65521u) a -= 65521u; b += a; if (b >= 65521u) b -= 65521u; }
    return (b << 16) | a;
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* ================================================================== VCDIFF (RFC 3284, default code table, no secondary compression) ================================================================== */
enum { V_NOOP = 0, V_ADD, V_RUN, V_COPY };
static u8 CT[256][6];                    /* type1 size1 mode1 type2 size2 mode2 */
static int ct_ok;
static void ct_set(int *n, int t1, int s1, int m1, int t2, int s2, int m2)
{
    u8 *e = CT[(*n)++];
    e[0] = (u8)t1; e[1] = (u8)s1; e[2] = (u8)m1; e[3] = (u8)t2; e[4] = (u8)s2; e[5] = (u8)m2;
}
static void ct_init(void)
{
    int n = 0, size, mode, a, c;
    if (ct_ok) return;
    ct_set(&n, V_RUN, 0, 0, V_NOOP, 0, 0);
    for (size = 0; size < 18; size++) ct_set(&n, V_ADD, size, 0, V_NOOP, 0, 0);
    for (mode = 0; mode < 9; mode++) {
        ct_set(&n, V_COPY, 0, mode, V_NOOP, 0, 0);
        for (size = 4; size < 19; size++) ct_set(&n, V_COPY, size, mode, V_NOOP, 0, 0);
    }
    for (mode = 0; mode < 6; mode++) for (a = 1; a < 5; a++) for (c = 4; c < 7; c++) ct_set(&n, V_ADD, a, 0, V_COPY, c, mode);
    for (mode = 6; mode < 9; mode++) for (a = 1; a < 5; a++) ct_set(&n, V_ADD, a, 0, V_COPY, 4, mode);
    for (mode = 0; mode < 9; mode++) ct_set(&n, V_COPY, 4, mode, V_ADD, 1, 0);
    ct_ok = 1;
}

struct vr { const u8 *b; u32 p, n; int bad; };
static u32 vr_byte(struct vr *r) { if (r->p >= r->n) { r->bad = 1; return 0; } return r->b[r->p++]; }
static u32 vr_var(struct vr *r)
{
    u32 v = 0;
    int i;
    for (i = 0; i < 5; i++) {
        u32 c = vr_byte(r);
        if (r->bad) return 0;
        v = (v << 7) | (c & 0x7Fu);
        if (!(c & 0x80u)) return v;
    }
    r->bad = 1;
    return 0;
}
static const u8 *vr_take(struct vr *r, u32 n) { const u8 *p = r->b + r->p; if (n > r->n - r->p) { r->bad = 1; return 0; } r->p += n; return p; }

int slp_vcdiff(const u8 *src, u32 srclen, const u8 *dl, u32 dllen, u8 *out, u32 outcap, u32 *outlen)
{
    static u32 same[3 * 256];
    struct vr r;
    u32 o = 0, hdr;
    ct_init();
    r.b = dl; r.p = 0; r.n = dllen; r.bad = 0;
    {
        const u8 *m = vr_take(&r, 4);
        if (!m || m[0] != 0xD6 || m[1] != 0xC3 || m[2] != 0xC4 || m[3] != 0) return -1;
    }
    hdr = vr_byte(&r);
    if (hdr & 3u) return -1;
    if (hdr & 4u) { u32 l = vr_var(&r); vr_take(&r, l); }
    while (!r.bad && r.p < r.n) {
        u32 win = vr_byte(&r), seglen = 0, segpos = 0, delta_len, delta_end, tgtlen, data_len, inst_len, addr_len, chk_n = 0;
        const u8 *seg = 0, *dp, *ip, *ap, *chk = 0;
        struct vr data, inst, addr;
        u32 near[4], near_i = 0, tl = 0, s_len;
        u8 *t;
        int i;
        if ((win & 3u) == 3u) return -1;
        if (win & 3u) {
            seglen = vr_var(&r); segpos = vr_var(&r);
            if (r.bad) return -1;
            if (win & 1u) { if (segpos > srclen || seglen > srclen - segpos) return -1; seg = src + segpos; }
            else { if (segpos > o || seglen > o - segpos) return -1; seg = out + segpos; }
        }
        delta_len = vr_var(&r);
        if (r.bad || delta_len > r.n - r.p) return -1;
        delta_end = r.p + delta_len;
        tgtlen = vr_var(&r);
        if (vr_byte(&r)) return -1;
        data_len = vr_var(&r); inst_len = vr_var(&r); addr_len = vr_var(&r);
        if (r.bad || tgtlen > outcap - o) return -1;
        if (win & 4u) {
            u32 used = r.p + data_len + inst_len + addr_len;
            if (used > delta_end) return -1;
            chk_n = delta_end - used;
            if (chk_n < 1 || chk_n > 5) return -1;
            chk = vr_take(&r, chk_n);
        }
        dp = vr_take(&r, data_len); ip = vr_take(&r, inst_len); ap = vr_take(&r, addr_len);
        if (r.bad) return -1;
        data.b = dp; data.p = 0; data.n = data_len; data.bad = 0;
        inst.b = ip; inst.p = 0; inst.n = inst_len; inst.bad = 0;
        addr.b = ap; addr.p = 0; addr.n = addr_len; addr.bad = 0;
        for (i = 0; i < 4; i++) near[i] = 0;
        for (i = 0; i < 3 * 256; i++) same[i] = 0;
        t = out + o;
        s_len = seglen;
        while (inst.p < inst.n) {
            const u8 *e = CT[inst.b[inst.p++]];
            for (i = 0; i < 2; i++) {
                u32 typ = e[3 * i], size = e[3 * i + 1], mode = e[3 * i + 2];
                if (typ == V_NOOP) continue;
                if (size == 0) size = vr_var(&inst);
                if (inst.bad || size > tgtlen - tl) return -1;
                if (typ == V_ADD) {
                    const u8 *q = vr_take(&data, size);
                    if (!q) return -1;
                    mcopy(t + tl, q, size); tl += size;
                } else if (typ == V_RUN) {
                    u32 b = vr_byte(&data), k;
                    if (data.bad) return -1;
                    for (k = 0; k < size; k++) t[tl + k] = (u8)b;
                    tl += size;
                } else {
                    u32 here = s_len + tl, a, k;
                    if (mode == 0) a = vr_var(&addr);
                    else if (mode == 1) { u32 v = vr_var(&addr); if (v > here) return -1; a = here - v; }
                    else if (mode < 6) a = near[mode - 2] + vr_var(&addr);
                    else a = same[(mode - 6) * 256 + vr_byte(&addr)];
                    if (addr.bad || a >= here) return -1;
                    near[near_i] = a; near_i = (near_i + 1) & 3u;
                    same[a % (3u * 256u)] = a;
                    for (k = 0; k < size; k++) {
                        u32 x = a + k;
                        t[tl + k] = x < s_len ? seg[x] : t[x - s_len];
                    }
                    tl += size;
                }
            }
        }
        if (tl != tgtlen) return -1;
        if (chk) {
            u32 ad = adler32(t, tl), v1 = 0, v2 = 0xFFFFFFFFu;
            int ok = 0;
            if (chk_n == 4) { v1 = be32(chk); ok = ad == v1; }
            if (!ok) {                              /* open-vcdiff: the Adler-32 as a varint */
                struct vr cr;
                cr.b = chk; cr.p = 0; cr.n = chk_n; cr.bad = 0;
                if (!(chk[chk_n - 1] & 0x80u)) { v2 = vr_var(&cr); ok = !cr.bad && v2 == ad; }
            }
            if (!ok) return -1;
        }
        o += tl;
    }
    if (r.bad) return -1;
    *outlen = o;
    return 0;
}

/* Slippi's SdMenu overlay differs from retail in exactly ONE byte that makes the item names run under the icons (apply_overlays.fix_item_names): restore it when that is the only difference */
#define SIS_N 1604
static int sis_starts(const u8 *d, u32 len, u32 *out)
{
    u32 ds, nr, root, i;
    if (len < 0x40) return 0;
    ds = be32(d + 4); nr = be32(d + 8);
    if (ds > len || nr > len || (u32)0x20 + ds + nr * 4u + 4u > len) return 0;
    root = be32(d + 0x20 + ds + nr * 4u);
    if ((u32)0x20 + root + 4u * SIS_N > len) return 0;
    for (i = 0; i < SIS_N; i++) out[i] = 0x20u + be32(d + 0x20 + root + 4u * i);
    return 1;
}
int slp_fix_item_names(const u8 *src, u32 srclen, u8 *res, u32 reslen)
{
    static u32 ra[SIS_N], rb[SIS_N];
    u32 limit, i, j;
    if (!sis_starts(src, srclen, ra) || !sis_starts(res, reslen, rb)) return 0;
    limit = 0x20u + be32(src + 4);
    for (i = 0; i < SIS_N; i++) {
        u32 a = ra[i], b = rb[i], nxt = 0xFFFFFFFFu, n;
        if (limit > a && limit < nxt) nxt = limit;
        for (j = 0; j < SIS_N; j++) if (ra[j] > a && ra[j] < nxt) nxt = ra[j];
        if (nxt == 0xFFFFFFFFu) return 0;                    /* Python: KeyError -> unchanged */
        n = nxt - a;
        if (a + n > srclen || b + n > reslen) return 0;
        if (i == 2) {
            if (src[a] != 0x14 || res[b] != 0x15) return 0;
            for (j = 1; j < n; j++) if (src[a + j] != res[b + j]) return 0;
        } else {
            for (j = 0; j < n; j++) if (src[a + j] != res[b + j]) return 0;
        }
    }
    res[rb[2]] = src[ra[2]];
    return 1;
}

/* ================================================================== mdelta ================================================================== */
static int varint(u8 *o, u32 n)
{
    int k = 0;
    for (;;) {
        u8 b = (u8)(n & 0x7Fu);
        n >>= 7;
        if (n) o[k++] = (u8)(b | 0x80u); else { o[k++] = b; return k; }
    }
}
static u32 zz(int n) { return n >= 0 ? (u32)n << 1 : (((u32)(-n)) << 1) - 1u; }

int slp_md_count(int olen, int W, int STEP) { return olen >= W ? (olen - W + STEP) / STEP : 0; }

/* The match index.  Python keeps, per distinct 12..16-byte window of the old file, the ascending list of its indexed positions and takes the 6 positions nearest to the expected one (stable: the lower
 * position first on a tie).  Here the indexed positions are counting-sorted by a bucket of a polynomial hash of the window (ascending inside a bucket); a lookup binary-searches the bucket for the
 * expected position and walks outwards on both sides, skipping hash collisions (the window bytes are compared), until 6 real candidates are collected: the same 6, in the same order, without ever
 * walking a long list of equal windows (zero runs).  The hash rolls along the new file. */
#define HM 0x01000193u
static u32 hash_direct(const u8 *p, int W)
{
    u32 h = 0;
    int k;
    for (k = 0; k < W; k++) h = h * HM + p[k];
    return h;
}
static u32 bucket_of(const struct slp_md *m, u32 h) { return (h * 0x9E3779B1u) >> (32 - m->bits); }

slp_u32 slp_md_mem(int olen, int W, int STEP)
{
    int ne = slp_md_count(olen, W, STEP), nb = 16;
    while (nb < ne / 2 && nb < (1 << 20)) nb <<= 1;
    return 4u * (u32)(nb + 2 + ne) + (u32)nb / 8u + 16u;
}

void slp_md_init(struct slp_md *m, const u8 *old, int olen, const u8 *nw, int nlen, int W, int STEP, int MINC, void *mem, u8 *out, int ocap)
{
    int i, nb = 16, bits = 4;
    m->old = old; m->nw = nw; m->olen = olen; m->nlen = nlen; m->W = W; m->STEP = STEP; m->MINC = MINC;
    m->ne = slp_md_count(olen, W, STEP);
    while (nb < m->ne / 2 && nb < (1 << 20)) { nb <<= 1; bits++; }
    m->nb = nb; m->bits = bits;
    m->start = (int *)mem; m->spos = m->start + nb + 2; m->occ = (u8 *)(m->spos + m->ne);
    for (i = 0; i <= nb + 1; i++) m->start[i] = 0;
    for (i = 0; i < nb / 8; i++) m->occ[i] = 0;
    m->bi = 0;
    m->mpow = 1;
    for (i = 0; i < W - 1; i++) m->mpow *= HM;
    m->out = out; m->oc = 0; m->ocap = ocap;
    m->i = m->pend = m->prev_end = m->expo = 0;
    m->hi = -1; m->hv = 0;
    m->phase = 0;
}

/* literal / copy emitters; -1 when the output is full */
static int md_lit(struct slp_md *m, int from, int to)
{
    int ln = to - from;
    if (m->oc + ln + 8 > m->ocap) return -1;
    m->out[m->oc++] = 1;
    m->oc += varint(m->out + m->oc, (u32)ln);
    mcopy(m->out + m->oc, m->nw + from, (u32)ln);
    m->oc += ln;
    return 0;
}

#define RETM(v) do { m->used = work; return (v); } while (0)
int slp_md_step(struct slp_md *m, u32 budget)
{
    u32 work = 0;
    const u8 *old = m->old, *nw = m->nw;
    int W = m->W, n = m->nlen;
    if (m->phase == 0) {                                   /* count the indexed positions per bucket */
        while (m->bi < m->ne) {
            u32 b = bucket_of(m, hash_direct(old + (u32)m->bi * (u32)m->STEP, W));
            m->start[b + 1]++;
            m->occ[b >> 3] |= (u8)(1u << (b & 7u));
            m->bi++;
            work += 4;
            if (work >= budget) RETM(0);
        }
        { int b; for (b = 0; b < m->nb; b++) m->start[b + 1] += m->start[b]; m->start[m->nb + 1] = m->ne; }          /* start[b + 1] = end of bucket b; the fill below moves it back to the start of bucket b */
        work += (u32)m->nb / 4u;
        m->bi = m->ne - 1;
        m->phase = 1;
    }
    if (m->phase == 1) {                                   /* fill, descending, so that each bucket ends up ascending */
        while (m->bi >= 0) {
            u32 b = bucket_of(m, hash_direct(old + (u32)m->bi * (u32)m->STEP, W));
            m->spos[--m->start[b + 1]] = m->bi * m->STEP;
            m->bi--;
            work += 4;
            if (work >= budget) RETM(0);
        }
        m->phase = 2;
    }
    while (m->phase == 2) {
        int cc[6], nc = 0, bc = -1, bl = 0, k;
        if (m->i > n - W) {
            if (m->pend < n && md_lit(m, m->pend, n)) RETM(-1);
            if (m->oc + 1 > m->ocap) RETM(-1);
            m->out[m->oc++] = 0;
            m->phase = 3;
            RETM(1);
        }
        if (work >= budget) RETM(0);
        work += 4;
        if (m->ne > 0) {
            u32 h, b;
            if (m->hi + 1 == m->i && m->hi >= 0) h = (m->hv - (u32)nw[m->hi] * m->mpow) * HM + nw[m->i + W - 1];        /* roll */
            else h = hash_direct(nw + m->i, W);
            m->hi = m->i; m->hv = h;
            b = bucket_of(m, h);
            if (m->occ[b >> 3] & (1u << (b & 7u))) {
                int lo = m->start[b + 1], hi = m->start[b + 2], a, z, L, R, cl, cr, dl, dr;
                const u8 *key = nw + m->i;
                /* first entry at or after the expected position */
                a = lo; z = hi;
                while (a < z) { int mid = (a + z) >> 1; if (m->spos[mid] < m->expo) a = mid + 1; else z = mid; }
                L = a - 1; R = a;
#define NEXTC(P, DIR, OUTC) do { OUTC = -1; while ((P) >= lo && (P) < hi) { const u8 *q_ = old + m->spos[P]; int k_; work++; for (k_ = 0; k_ < W; k_++) if (q_[k_] != key[k_]) break; \
                    if (k_ == W) { OUTC = m->spos[P]; break; } (P) += (DIR); } } while (0)
                NEXTC(L, -1, cl); NEXTC(R, 1, cr);
                while (nc < 6 && (cl >= 0 || cr >= 0)) {
                    dl = cl >= 0 ? m->expo - cl : 0; dr = cr >= 0 ? cr - m->expo : 0;
                    if (cl >= 0 && (cr < 0 || dl <= dr)) { cc[nc++] = cl; L--; NEXTC(L, -1, cl); }          /* a tie goes to the lower position, as the stable sort does */
                    else { cc[nc++] = cr; R++; NEXTC(R, 1, cr); }
                }
#undef NEXTC
            }
            for (k = 0; k < nc; k++) {
                int c = cc[k], l = 0, mx = n - m->i < m->olen - c ? n - m->i : m->olen - c;
                while (l < mx && old[c + l] == nw[m->i + l]) l++;
                work += (u32)(l >> 6);
                if (bc < 0 || l > bl) { bc = c; bl = l; }
            }
        }
        if (bc >= 0 && bl >= m->MINC) {
            int c = bc, l = bl, back = 0, i = m->i;
            while (i - back > m->pend && c - back > 0 && old[c - back - 1] == nw[i - back - 1]) back++;
            c -= back; i -= back; l += back;
            if (i > m->pend && md_lit(m, m->pend, i)) RETM(-1);
            if (m->oc + 24 > m->ocap) RETM(-1);
            m->out[m->oc++] = 2;
            m->oc += varint(m->out + m->oc, zz(c - m->prev_end));
            m->oc += varint(m->out + m->oc, (u32)l);
            m->prev_end = c + l;
            i += l;
            m->pend = i;
            m->expo = c + l;
            m->i = i;
        } else {
            m->i++;
            m->expo++;
        }
    }
    RETM(1);
}

/* apply an op stream (mdelta.apply) into out, or (out == 0) only run the CRC-32 of the produced bytes over it: *crc.  0 ok */
static int delta_run(const u8 *old, u32 olen, const u8 *d, u32 dn, u8 *out, u32 nsize, u32 *crc)
{
    u32 i = 0, o = 0, cc = 0;
    int prev_end = 0;
    for (;;) {
        u32 t, v, s, ln, k;
        if (i >= dn) return -1;
        t = d[i++];
        if (t == 0) break;
#define RV(var) do { var = 0; s = 0; for (;;) { u32 b_; if (i >= dn) return -1; b_ = d[i++]; var |= (b_ & 0x7Fu) << s; if (!(b_ & 0x80u)) break; s += 7; if (s > 28) return -1; } } while (0)
        if (t == 1) {
            RV(ln);
            if (ln > dn - i || ln > nsize - o) return -1;
            if (out) mcopy(out + o, d + i, ln); else cc = slp_crc32(cc, d + i, ln);
            i += ln; o += ln;
        } else if (t == 2) {
            int src;
            RV(v); RV(ln);
            src = prev_end + ((v & 1u) ? -(int)((v + 1u) >> 1) : (int)(v >> 1));
            if (src < 0 || (u32)src > olen || ln > olen - (u32)src || ln > nsize - o) return -1;
            if (out) for (k = 0; k < ln; k++) out[o + k] = old[(u32)src + k]; else cc = slp_crc32(cc, old + src, ln);
            o += ln; prev_end = src + (int)ln;
        } else if (t == 3) {
            RV(ln);
            if (i >= dn || ln > nsize - o) return -1;
            if (out) for (k = 0; k < ln; k++) out[o + k] = d[i];
            else { u8 b8[64]; for (k = 0; k < 64; k++) b8[k] = d[i]; for (k = 0; k < ln; k += 64) cc = slp_crc32(cc, b8, ln - k < 64 ? ln - k : 64); }
            i++; o += ln;
        } else return -1;
    }
    if (crc) *crc = cc;
    return o == nsize ? 0 : -1;
}
int slp_delta_apply(const u8 *old, u32 olen, const u8 *d, u32 dn, u8 *out, u32 nsize) { return delta_run(old, olen, d, dn, out, nsize, 0); }
int slp_delta_crc(const u8 *old, u32 olen, const u8 *d, u32 dn, u32 nsize, u32 *crc) { return delta_run(old, olen, d, dn, 0, nsize, crc); }

int slp_delta_inplace_ok(const u8 *d, u32 dn, u32 oldlen, u32 new_size)
{
    s64 oldcap = rup(oldlen, 32), newcap = rup(new_size, 32), shift, dd = 0, prev_end = 0;
    u32 i = 0;
    if (newcap < oldcap) return 0;
    shift = newcap - oldcap;
    for (;;) {
        u32 t, a, b = 0, s;
        if (i >= dn) return 0;
        t = d[i++];
        if (t == 0) return 1;
        RV(a);
        if (t == 1) { if (a > dn - i) return 0; i += a; }
        else if (t == 2) { RV(b); }
        else if (t == 3) { i++; }
        else return 0;
        if (t == 2) {
            s64 src = (a & 1u) ? prev_end - ((s64)(a >> 1) + 1) : prev_end + (s64)(a >> 1);
            if (src < prev_end || dd > shift + src) return 0;
            dd += b; prev_end = src + b;
        } else {
            s64 ln = a;
            if (dd > shift + prev_end || ln > shift + prev_end - dd) return 0;
            dd += ln;
        }
    }
}

int slp_mslz_gap(const u8 *z, u32 zn, u32 out_len)
{
    u32 ip = 0, op = 0;
    s64 need = 0;
    for (;;) {
        u32 tok, lit, ml;
        s64 v;
        if (ip >= zn) return -1;
        tok = z[ip++];
        lit = tok >> 4;
        if (lit == 15) { u32 b; do { if (ip >= zn) return -1; b = z[ip++]; lit += b; } while (b == 255); }
        v = (s64)op - ip; if (v > need) need = v;
        if (lit > zn - ip) return -1;
        ip += lit; op += lit;
        if (ip == zn) break;
        ip += 2;
        ml = tok & 15u;
        if (ml == 15) { u32 b; do { if (ip >= zn) return -1; b = z[ip++]; ml += b; } while (b == 255); }
        ml += 4;
        v = (s64)op + ml - ip; if (v > need) need = v;
        op += ml;
    }
    if (op != out_len) return -1;
    return (int)(need > 0 ? need : 0);
}

/* ================================================================== optimal-parse MSLZ ================================================================== */
static int zext(long n) { return n >= 15 ? (int)(1 + (n - 15) / 255) : 0; }

slp_u32 slp_z_mem(int n)
{
    u32 a = (u32)HB + (u32)(n + 1), b = 4u * (u32)(n + 1);
    if (b > a) a = b;
    return 4u * a + 4u * (u32)(n + 1) + 2u * (u32)(n + 1) + (u32)(n + 1) + 16u;
}

void slp_z_init(struct slp_z *z, const u8 *d, int n, s64 cap, void *mem, u8 *out)
{
    u32 a = (u32)HB + (u32)(n + 1), b = 4u * (u32)(n + 1), i;
    int *p = (int *)mem;
    if (b > a) a = b;
    z->d = d; z->n = n; z->depth = 1024; z->cap = cap; z->ph = 0;
    z->A = p; z->head = p; z->prev = p + HB;
    z->Lmax = p + a;
    z->Off = (unsigned short *)(z->Lmax + (n + 1));
    z->Uf = (u8 *)(z->Off + (n + 1));
    z->S = p; z->Sm = p + (n + 1); z->M = p + 2 * (n + 1); z->ML = p + 3 * (n + 1);
    for (i = 0; i < (u32)HB; i++) z->head[i] = -1;
    mzero(z->Lmax, 4u * (u32)(n + 1));
    mzero(z->Off, 2u * (u32)(n + 1));
    z->i = 0; z->j = n;
    for (i = 0; i < 255; i++) { z->rbest[i] = (s64)INF * 4; z->rk[i] = -1; }
    z->out = out; z->outlen = 0;
}

#define RETZ(v) do { z->used = work; return (v); } while (0)
int slp_z_step(struct slp_z *z, u32 budget)
{
    u32 work = 0;
    const u8 *d = z->d;
    int n = z->n;
    if (z->ph == 0) {
        while (z->i + MINM <= n) {
            int i = z->i, c, t = z->depth;
            int bl = 0, bo = 0;
            u32 k = (u32)((d[i] | d[i + 1] << 8 | d[i + 2] << 16 | (u32)d[i + 3] << 24) * 2654435761u) >> 12;
            c = z->head[k];
            while (c >= 0 && i - c <= MAXOFF && t--) {
                work++;
                if (bl == 0 || d[c + bl] == d[i + bl]) {
                    int l = 0;
                    while (i + l < n && d[c + l] == d[i + l]) l++;
                    work += (u32)(l >> 5);
                    if (l > bl) { bl = l; bo = i - c; if (i + l >= n) break; }
                }
                c = z->prev[c];
            }
            if (bl >= MINM) { z->Lmax[i] = bl; z->Off[i] = (unsigned short)bo; }
            z->prev[i] = z->head[k];
            z->head[k] = i;
            z->i++;
            if (++work >= budget) RETZ(0);
        }
        z->ph = 1;
        z->j = n;
    }
    if (z->ph == 1) {
        int *S = z->S, *Sm = z->Sm, *M = z->M, *ML = z->ML;
        int cap_ok = z->cap > 0;
        while (z->j >= 0) {
            int j = z->j, bm = INF, bl = 0, L = z->Lmax[j], best, bmlit = -1, r;
            long l;
            if (work >= budget) RETZ(0);
            work += 40;                                    /* one position of the DP: 255 residue classes, about 40 chain nodes of the match search in cost */
#define TRY(LL) do { long l_ = (LL); long e_ = j + l_; if (z->Uf[e_]) { int v_ = zext(l_ - MINM) + S[e_]; if (v_ < bm || (v_ == bm && l_ > bl)) { bm = v_; bl = (int)l_; } } } while (0)
            for (l = MINM; l <= L && l <= 40; l++) TRY(l);
            if (L > 40) {
                long tail = L - 7 > 41 ? L - 7 : 41;
                for (l = 19 + 255 * ((41 - 19 + 254) / 255); l < tail; l += 255) TRY(l);
                for (l = tail; l <= L; l++) TRY(l);
            }
#undef TRY
            M[j] = bm; ML[j] = bl;
            best = 1 + zext(n - j) + (n - j);
            for (l = 0; l < 15 && j + l <= n; l++) {
                int v;
                if (M[j + l] >= INF) continue;
                v = 1 + (int)l + 2 + M[j + l];
                if (v < best) { best = v; bmlit = (int)l; }
            }
            if (j + 15 <= n && M[j + 15] < INF) {
                long k = j + 15, a = k - 15;
                int rr = (int)(a % 255);
                s64 A = (s64)M[k] + k + a / 255;
                if (A <= z->rbest[rr]) { z->rbest[rr] = A; z->rk[rr] = (int)k; }
            }
            {
                long qj = j / 255; int rj = (int)(j % 255);
                for (r = 0; r < 255; r++) {
                    s64 v;
                    if (z->rbest[r] >= (s64)INF * 4) continue;
                    v = 4 - j - qj + z->rbest[r] - (r < rj ? 1 : 0);
                    if (v < best) { best = (int)v; bmlit = z->rk[r] - j; }
                }
            }
            S[j] = best; Sm[j] = bmlit;
            z->Uf[j] = (u8)(!cap_ok || (s64)best <= z->cap - j);
            z->j--;
        }
        if (cap_ok && S[0] > z->cap) RETZ(3);
        z->ph = 2;
    }
    if (z->ph == 2) {                                   /* emit */
        int j = 0, o = 0;
        int *Sm = z->Sm, *ML = z->ML;
        u8 *out = z->out;
#define PUT(b) (out[o++] = (u8)(b))
#define EXT(v) do { long vv = (v); while (vv >= 255) { PUT(255); vv -= 255; } PUT((int)vv); } while (0)
        for (;;) {
            int m = Sm[j];
            if (m < 0) {
                long lit = n - j, i;
                PUT((lit > 15 ? 15 : lit) << 4);
                if (lit >= 15) EXT(lit - 15);
                for (i = j; i < n; i++) PUT(d[i]);
                break;
            } else {
                long k = j + m, i;
                int l = ML[k], ml = l - MINM;
                PUT(((m > 15 ? 15 : m) << 4) | (ml > 15 ? 15 : ml));
                if (m >= 15) EXT(m - 15);
                for (i = j; i < k; i++) PUT(d[i]);
                PUT(z->Off[k] & 255); PUT(z->Off[k] >> 8);
                if (ml >= 15) EXT(ml - 15);
                j = (int)(k + l);
            }
        }
        z->outlen = o;
        if (o != z->S[0]) RETZ(-1);
        z->ph = 3;
    }
    RETZ(1);
}

