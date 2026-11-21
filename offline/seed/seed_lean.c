/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* seed_lean: see seed_lean.h.  C89 plus long long; no libc.  Inputs are the same as the system-files builder's (seed_slp.h); the format written is the disc-file pack of menus/df/df.h. */
#include "seed_lean.h"
#include "seed_slp_tab.h"
#include "../payload_pack/ms_lz.h"

typedef slp_u8 u8;
typedef slp_u32 u32;

#define BLOCK 0x2000u
#define CHUNK 0x20000u
#define LZ_HB 13
#define LZ_DEPTH 64
#define LZ_MAXLEN 2048u
#define LZ_MASK 0x7FFFu                 /* window 32 KiB - 1 */
#define K_S 0u
#define K_L 1u
#define K_R 2u
#define LMASK 0x0FFFFFFFu
#define LEAN_TOTAL 2700000u             /* work units of a stock build (measured by seed_lean_test.c): the progress bar's scale */

static const unsigned char ORD[SLP_NENT_MAX] = { 2, 3, 5, 0, 1, 4 };      /* the small entries first: the stored streams pile up on the heap, the biggest work areas come while few are there; 5 = MnSlMap.usd, only when s->n is 6 */

#define RID(k) ((k) == 5u ? SLP_RETAIL_SLMAP : SLP_RETAIL + (int)(k))     /* the io ids of table row k */
#define DID(k) ((k) == 5u ? SLP_DIFF_SLMAP : SLP_DIFF + (int)(k))

static void mcopy(u8 *d, const u8 *s, u32 n) { u32 i; for (i = 0; i < n; i++) d[i] = s[i]; }
static void mzero(void *p, u32 n) { u8 *c = (u8 *)p; u32 i; for (i = 0; i < n; i++) c[i] = 0; }
static u32 rup(u32 x, u32 a) { return (x + a - 1u) / a * a; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* ================================================================== lean MSLZ ================================================================== */
slp_u32 slpl_lz_mem(void) { return 4u * (1u << LZ_HB) + 2u * (LZ_MASK + 1u) + 32u; }

void slpl_lz_init(struct slpl_lz *z, const u8 *d, u32 n, void *mem, u8 *out, u32 ocap)
{
    z->d = d; z->n = n; z->head = (u32 *)mem; z->prev = (unsigned short *)(z->head + (1u << LZ_HB));
    mzero(mem, slpl_lz_mem() - 32u);
    z->out = out; z->ocap = ocap; z->o = 0; z->i = 0; z->anchor = 0; z->used = 0;
}

static u32 lz_hash(const u8 *p) { return (((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24) * 2654435761u) >> (32 - LZ_HB); }

static void lz_ins(struct slpl_lz *z, u32 p)
{
    u32 h, c, dist;
    if (p + 4u > z->n) return;
    h = lz_hash(z->d + p);
    c = z->head[h];
    dist = c ? p - (c - 1u) : 0;
    if (dist > LZ_MASK) dist = 0;
    z->prev[p & LZ_MASK] = (unsigned short)dist;
    z->head[h] = p + 1u;
}

static u32 lz_find(struct slpl_lz *z, u32 i, u32 *off, u32 *work)
{
    const u8 *d = z->d;
    u32 best = 0, c, t = LZ_DEPTH, mx;
    if (i + 4u > z->n) return 0;
    c = z->head[lz_hash(d + i)];
    mx = z->n - i < LZ_MAXLEN ? z->n - i : LZ_MAXLEN;
    while (c && t--) {
        u32 cand = c - 1u, dist = i - cand, dd;
        if (dist > LZ_MASK) break;
        (*work)++;
        if (best == 0 || d[cand + best] == d[i + best]) {
            u32 l = 0;
            while (l < mx && d[cand + l] == d[i + l]) l++;
            *work += l >> 5;
            if (l > best) { best = l; *off = dist; if (l == mx) break; }
        }
        dd = z->prev[cand & LZ_MASK];
        if (!dd) break;
        c = cand - dd + 1u;
    }
    return best >= 4u ? best : 0;
}

static int lz_put_ext(struct slpl_lz *z, u32 v)
{
    while (v >= 255u) { if (z->o >= z->ocap) return -1; z->out[z->o++] = 255; v -= 255u; }
    if (z->o >= z->ocap) return -1;
    z->out[z->o++] = (u8)v;
    return 0;
}

/* one sequence: literals [a, b) then a match (ml >= 4) at distance off; ml == 0 = the final literal run */
static int lz_emit(struct slpl_lz *z, u32 a, u32 b, u32 ml, u32 off)
{
    u32 lit = b - a, m = ml ? ml - 4u : 0;
    if (z->o >= z->ocap) return -1;
    z->out[z->o++] = (u8)(((lit > 15u ? 15u : lit) << 4) | (m > 15u ? 15u : m));
    if (lit >= 15u && lz_put_ext(z, lit - 15u)) return -1;
    if (lit > z->ocap - z->o) return -1;
    mcopy(z->out + z->o, z->d + a, lit);
    z->o += lit;
    if (!ml) return 0;
    if (z->ocap - z->o < 2u) return -1;
    z->out[z->o++] = (u8)(off & 255u); z->out[z->o++] = (u8)(off >> 8);
    if (m >= 15u && lz_put_ext(z, m - 15u)) return -1;
    return 0;
}

int slpl_lz_step(struct slpl_lz *z, u32 budget)
{
    u32 work = 0;
    while (z->i + 4u <= z->n) {
        u32 i = z->i, off = 0, off2 = 0, l1, l2;
        if (work >= budget) { z->used = work; return 0; }
        l1 = lz_find(z, i, &off, &work);
        if (l1) {
            lz_ins(z, i);
            if (i + 5u <= z->n) {
                l2 = lz_find(z, i + 1u, &off2, &work);
                if (l2 > l1) { z->i = i + 1u; continue; }               /* lazy: the match one byte later is longer: this byte stays a literal */
            }
            if (lz_emit(z, z->anchor, i, l1, off)) { z->used = work; return -1; }
            { u32 p; for (p = i + 1u; p < i + l1; p++) lz_ins(z, p); }
            work += l1 >> 2;
            z->i = z->anchor = i + l1;
        } else {
            lz_ins(z, i);
            z->i++;
            work++;
        }
    }
    z->used = work;
    if (lz_emit(z, z->anchor, z->n, 0, 0)) return -1;
    return 1;
}

int slpl_lz_check(const u8 *z, u32 zn, const u8 *d, u32 n)
{
    u32 ip = 0, op = 0;
    for (;;) {
        u32 tok, lit, ml, off, k;
        if (ip >= zn) return -1;
        tok = z[ip++];
        lit = tok >> 4;
        if (lit == 15u) { u32 b; do { if (ip >= zn) return -1; b = z[ip++]; lit += b; } while (b == 255u); }
        if (lit > zn - ip || lit > n - op) return -1;
        for (k = 0; k < lit; k++) if (z[ip + k] != d[op + k]) return -1;
        ip += lit; op += lit;
        if (ip == zn) break;
        if (zn - ip < 2u) return -1;
        off = (u32)z[ip] | ((u32)z[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return -1;
        ml = tok & 15u;
        if (ml == 15u) { u32 b; do { if (ip >= zn) return -1; b = z[ip++]; ml += b; } while (b == 255u); }
        ml += 4u;
        if (ml > n - op) return -1;
        for (k = 0; k < ml; k++) if (d[op + k] != d[op + k - off]) return -1;
        op += ml;
    }
    return op == n ? 0 : -1;
}

/* ================================================================== VCDIFF instructions -> op stream ================================================================== */
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

struct vcw { u32 seglen, segpos, tgtlen, data_len, inst_len, addr_len; const u8 *dp, *ip, *ap; };

static int vc_open(struct vr *r, const u8 *dl, u32 dllen)
{
    const u8 *m;
    u32 hdr;
    r->b = dl; r->p = 0; r->n = dllen; r->bad = 0;
    m = vr_take(r, 4);
    if (!m || m[0] != 0xD6 || m[1] != 0xC3 || m[2] != 0xC4 || m[3] != 0) return -1;
    hdr = vr_byte(r);
    if (hdr & 3u) return -1;
    if (hdr & 4u) { u32 l = vr_var(r); vr_take(r, l); }
    return r->bad ? -1 : 0;
}

/* 0 = a window was read, 1 = no more, -1 = bad (VCD_TARGET windows and custom code tables are not supported: Slippi's files have neither) */
static int vc_win(struct vr *r, struct vcw *w, u32 srclen)
{
    u32 win, dlen, end, chk_n;
    if (r->p >= r->n) return 1;
    win = vr_byte(r);
    if (win & 2u) return -1;
    w->seglen = w->segpos = 0;
    if (win & 1u) {
        w->seglen = vr_var(r); w->segpos = vr_var(r);
        if (r->bad || w->segpos > srclen || w->seglen > srclen - w->segpos) return -1;
    }
    dlen = vr_var(r);
    if (r->bad || dlen > r->n - r->p) return -1;
    end = r->p + dlen;
    w->tgtlen = vr_var(r);
    if (vr_byte(r)) return -1;
    w->data_len = vr_var(r); w->inst_len = vr_var(r); w->addr_len = vr_var(r);
    if (r->bad || w->data_len > end || w->inst_len > end || w->addr_len > end) return -1;
    if (win & 4u) {
        u32 used = r->p + w->data_len + w->inst_len + w->addr_len;
        if (used > end) return -1;
        chk_n = end - used;
        if (chk_n < 1u || chk_n > 5u) return -1;
        vr_take(r, chk_n);                       /* the Adler-32 of the window cannot be checked without the whole window: the container's CRCs cover the file */
    }
    w->dp = vr_take(r, w->data_len); w->ip = vr_take(r, w->inst_len); w->ap = vr_take(r, w->addr_len);
    return r->bad ? -1 : 0;
}

/* VCDIFF default code table (the same as seed_slp.c) */
enum { V_NOOP = 0, V_ADD, V_RUN, V_COPY };
static u8 CT[256][6];
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

/* an output range: appended, merged with the last one when it continues it */
static int tr_emit(struct slpl *s, u32 kind, u32 a, u32 n)
{
    struct slpl_piece *p;
    if (!n) return 0;
    if (s->npc) {
        u32 pk, pl;
        p = &s->pc[s->npc - 1u];
        pk = p->lk >> 28; pl = p->lk & LMASK;
        if (pk == kind && pl + n <= LMASK && ((kind == K_R) ? p->a == a : p->a + pl == a)) { p->lk += n; s->nlen += n; return 0; }
    }
    if (s->npc >= s->cpc) return -1;
    p = &s->pc[s->npc++];
    p->pos = s->nlen; p->a = a; p->lk = n | (kind << 28);
    s->nlen += n;
    return 0;
}

/* a VCDIFF COPY from the target (window output so far): resolved to the ranges it was made of (overlapping copies repeat) */
static int tr_tcopy(struct slpl *s, u32 a, u32 len)
{
    while (len) {
        u32 c, x, e;
        if (a >= s->nlen) return -1;
        c = s->nlen - a;
        if (c > len) c = len;
        x = a; e = a + c;
        while (x < e) {
            u32 lo = 0, hi = s->npc, off, pl, n, kind;
            struct slpl_piece p;
            while (hi - lo > 1u) { u32 mid = (lo + hi) >> 1; if (s->pc[mid].pos <= x) lo = mid; else hi = mid; }
            p = s->pc[lo];
            off = x - p.pos; pl = p.lk & LMASK; kind = p.lk >> 28;
            n = e - x < pl - off ? e - x : pl - off;
            if (kind == K_R ? tr_emit(s, K_R, p.a, n) : tr_emit(s, kind, p.a + off, n)) return -1;
            x += n;
        }
        a += c; len -= c;
    }
    return 0;
}

static int translate(struct slpl *s, u32 srclen)
{
    struct vr r;
    struct vcw w;
    int rc;
    ct_init();
    if (vc_open(&r, s->dl, s->dllen)) return -1;
    s->nlen = 0; s->npc = 0;
    while ((rc = vc_win(&r, &w, srclen)) == 0) {
        struct vr data, inst, addr;
        u32 near[4], near_i = 0, same[3 * 256], i, wstart = s->nlen, dbase = (u32)(w.dp - s->dl);
        data.b = w.dp; data.p = 0; data.n = w.data_len; data.bad = 0;
        inst.b = w.ip; inst.p = 0; inst.n = w.inst_len; inst.bad = 0;
        addr.b = w.ap; addr.p = 0; addr.n = w.addr_len; addr.bad = 0;
        for (i = 0; i < 4; i++) near[i] = 0;
        for (i = 0; i < 3 * 256; i++) same[i] = 0;
        while (inst.p < inst.n) {
            const u8 *e = CT[inst.b[inst.p++]];
            for (i = 0; i < 2; i++) {
                u32 typ = e[3 * i], size = e[3 * i + 1], mode = e[3 * i + 2], tl = s->nlen - wstart;
                if (typ == V_NOOP) continue;
                if (size == 0) size = vr_var(&inst);
                if (inst.bad || size > w.tgtlen - tl) return -1;
                if (typ == V_ADD) {
                    u32 at = data.p;
                    if (!vr_take(&data, size)) return -1;
                    if (tr_emit(s, K_L, dbase + at, size)) return -2;
                } else if (typ == V_RUN) {
                    u32 b = vr_byte(&data);
                    if (data.bad) return -1;
                    if (tr_emit(s, K_R, b, size)) return -2;
                } else {
                    u32 here = w.seglen + tl, a, k;
                    if (mode == 0) a = vr_var(&addr);
                    else if (mode == 1) { u32 v = vr_var(&addr); if (v > here) return -1; a = here - v; }
                    else if (mode < 6) a = near[mode - 2] + vr_var(&addr);
                    else a = same[(mode - 6) * 256 + vr_byte(&addr)];
                    if (addr.bad || a >= here) return -1;
                    near[near_i] = a; near_i = (near_i + 1) & 3u;
                    same[a % (3u * 256u)] = a;
                    (void)k;
                    while (size) {
                        u32 n;
                        if (a < w.seglen) {
                            n = w.seglen - a < size ? w.seglen - a : size;
                            if (tr_emit(s, K_S, w.segpos + a, n)) return -2;
                        } else {
                            n = size;
                            if (tr_tcopy(s, wstart + (a - w.seglen), n)) return -3;
                        }
                        a += n; size -= n;
                    }
                }
            }
        }
        if (s->nlen - wstart != w.tgtlen) return -1;
    }
    return rc < 0 ? -1 : 0;
}

/* the pieces as an mdelta op stream: out == 0 only measures.  -> length */
static u32 vput(u8 *out, u32 *o, u32 v)
{
    for (;;) {
        u8 b = (u8)(v & 0x7Fu);
        v >>= 7;
        if (out) out[*o] = (u8)(v ? b | 0x80u : b);
        (*o)++;
        if (!v) return 0;
    }
}
static u32 ops_gen(const struct slpl *s, u8 *out)
{
    u32 o = 0, i = 0, prev_end = 0;
    while (i < s->npc) {
        const struct slpl_piece *p = &s->pc[i];
        u32 kind = p->lk >> 28, len = p->lk & LMASK;
        if (kind == K_L) {
            u32 total = 0, j = i;
            while (j < s->npc && (s->pc[j].lk >> 28) == K_L) { total += s->pc[j].lk & LMASK; j++; }
            if (out) out[o] = 1;
            o++;
            vput(out, &o, total);
            for (; i < j; i++) { u32 l = s->pc[i].lk & LMASK; if (out) mcopy(out + o, s->dl + s->pc[i].a, l); o += l; }
        } else if (kind == K_R) {
            if (out) out[o] = 3;
            o++;
            vput(out, &o, len);
            if (out) out[o] = (u8)p->a;
            o++;
            i++;
        } else {
            int d = (int)p->a - (int)prev_end;
            if (out) out[o] = 2;
            o++;
            vput(out, &o, d >= 0 ? (u32)d << 1 : (((u32)(-d)) << 1) - 1u);
            vput(out, &o, len);
            prev_end = p->a + len;
            i++;
        }
    }
    if (out) out[o] = 0;
    return o + 1u;
}

/* ================================================================== the sliced driver ================================================================== */
enum { P_START = 0, P_CRC, P_PARSE, P_OPS, P_RD_OLD, P_VCDIFF, P_MD_INIT, P_MD_RUN, P_MD_CHECK, P_Z_INIT, P_Z_RUN, P_Z_CHECK, P_ASM, P_DONE };

/* 1 when the disc carries the optional MnSlMap.usd entry's inputs: the retail file of exactly the stock size (a disc that already has Slippi's patched MnSlMap, or another file, gets no entry) and Slippi's diff */
int slpl_has_slmap(const struct slp_io *io)
{
    const struct slp_tab *t = &SLP_TAB[5];
    return io->size(io->ctx, SLP_RETAIL_SLMAP) == (int)t->old_size && io->size(io->ctx, SLP_DIFF_SLMAP) > 0;
}

void slpl_init(struct slpl *s, const struct slpl_io *io)
{
    mzero(s, sizeof *s);
    s->io = io;
    s->ph = P_START;
    s->n = slpl_has_slmap(&io->io) && io->map && io->map(io->io.ctx, SLP_DIFF_SLMAP) ? SLP_NENT_MAX : SLP_NENT;
}

int slpl_disc_has(const struct slp_io *io)
{
    unsigned k;
    for (k = 0; k < SLP_NENT; k++) {
        const struct slp_tab *t = &SLP_TAB[k];
        if (k < 4) { if (io->size(io->ctx, SLP_RETAIL + (int)k) != (int)t->old_size) return 0; if (io->size(io->ctx, SLP_DIFF + (int)k) <= 0) return 0; }
        else if (io->size(io->ctx, SLP_WHOLE) != (int)t->new_size) return 0;
    }
    return io->size(io->ctx, SLP_HEAD) == (int)(0x40u + BLOCK);
}

static void *A_(struct slpl *s, u32 n)
{
    u8 *p = (u8 *)s->io->io.alloc(s->io->io.ctx, n + 32u);
    if (!p) return 0;
    put32(p, n);
    s->cur += n;
    if (s->cur > s->peak) s->peak = s->cur;
    return p + 32;
}
static void F_(struct slpl *s, void *q)
{
    u8 *p = (u8 *)q;
    if (!q) return;
    p -= 32;
    s->cur -= be32(p);
    s->io->io.release(s->io->io.ctx, p);
}
static int fail(struct slpl *s, int e)
{
    s->err = e; s->detail = (s->k << 8) | (unsigned)s->ph;
    return e;
}

void slpl_free(struct slpl *s)
{
    unsigned k;
    F_(s, s->chunk); F_(s, s->old); F_(s, s->nw); F_(s, s->dout); F_(s, s->pc); F_(s, s->md.start); F_(s, s->zbuf); F_(s, s->zout); F_(s, s->head); F_(s, s->gci_raw);
    s->chunk = s->old = s->nw = s->dout = s->zbuf = s->zout = s->head = s->gci_raw = s->gci_data = 0;
    s->pc = 0; s->md.start = 0;
    for (k = 0; k < SLP_NENT_MAX; k++) { F_(s, s->e[k].stored); s->e[k].stored = 0; }
}

unsigned slpl_percent(const struct slpl *s)
{
    u32 v;
    if (s->ph >= P_DONE) return 100;
    v = s->work / (LEAN_TOTAL / 100u);
    return v > 99 ? 99u : v;
}

static int read_chunk(struct slpl *s, int id, u8 *dst, u32 size)
{
    u32 n = size - s->pos > CHUNK ? CHUNK : size - s->pos;
    if (s->io->io.read(s->io->io.ctx, id, dst + (dst == s->chunk ? 0u : s->pos), s->pos, n)) return -1;
    s->pos += n;
    s->work += 2000u + n / 64u;
    return 0;
}

int slpl_step(struct slpl *s, u32 budget)
{
    const struct slp_tab *t;
    if (s->err) return s->err;
    if (s->ph >= P_DONE) return SLP_DONE;
    t = &SLP_TAB[s->k < SLP_NENT_MAX ? s->k : 0];
    switch (s->ph) {
    case P_START:
        while (s->ord < SLP_NENT_MAX && ORD[s->ord] >= s->n) s->ord++;       /* the optional entry is not there */
        if (s->ord >= SLP_NENT_MAX) { s->ph = P_ASM; return SLP_RUN; }
        s->k = ORD[s->ord]; t = &SLP_TAB[s->k];
        s->pos = 0; s->crc = 0;
        if (s->k == 4) {
            s->src = s->io->map ? s->io->map(s->io->io.ctx, SLP_WHOLE) : 0;
            if (s->io->io.size(s->io->io.ctx, SLP_WHOLE) != (int)t->new_size) return fail(s, SLP_E_INPUT);
            if (!s->src) return fail(s, SLP_E_READ);
            if (slp_crc32(0, s->src, t->new_size) != t->new_crc) return fail(s, SLP_E_INPUT);
            s->work += 2000u + t->new_size / 16u;
            s->doutlen = t->new_size;
            s->ph = P_Z_INIT;
            return SLP_RUN;
        }
        if (s->io->io.size(s->io->io.ctx, RID(s->k)) != (int)t->old_size) return fail(s, SLP_E_INPUT);
        s->dllen = (u32)s->io->io.size(s->io->io.ctx, DID(s->k));
        s->dl = s->io->map ? s->io->map(s->io->io.ctx, DID(s->k)) : 0;
        if ((int)s->dllen <= 0 || s->dllen > 0x100000u) return fail(s, SLP_E_INPUT);
        if (!s->dl) return fail(s, SLP_E_READ);
        if (s->k < 2 || s->k == 5) {                                     /* the retail file is only checked, in chunks (MnExtAll.usd, MnMaAll.usd, MnSlMap.usd) */
            s->chunk = (u8 *)A_(s, CHUNK + 32u);
            if (!s->chunk) return fail(s, SLP_E_MEM);
            s->ph = P_CRC;
        } else {
            s->old = (u8 *)A_(s, rup(t->old_size, 32) + 32);
            s->nw = (u8 *)A_(s, rup(t->new_size, 32) + 32);
            if (!s->old || !s->nw) return fail(s, SLP_E_MEM);
            s->ph = P_RD_OLD;
        }
        return SLP_RUN;
    case P_CRC: {
        u32 n = t->old_size - s->pos > CHUNK ? CHUNK : t->old_size - s->pos;
        if (read_chunk(s, RID(s->k), s->chunk, t->old_size)) return fail(s, SLP_E_READ);
        s->crc = slp_crc32(s->crc, s->chunk, n);
        if (s->pos >= t->old_size) {
            if (s->crc != t->old_crc) return fail(s, SLP_E_INPUT);
            F_(s, s->chunk); s->chunk = 0;
            s->ph = P_PARSE;
        }
        return SLP_RUN; }
    case P_PARSE: {
        struct vr r;
        struct vcw w;
        u32 inst = 0;
        int rc;
        ct_init();
        if (vc_open(&r, s->dl, s->dllen)) return fail(s, SLP_E_VCDIFF);
        while ((rc = vc_win(&r, &w, t->old_size)) == 0) inst += w.inst_len;
        if (rc < 0) return fail(s, SLP_E_VCDIFF);
        s->cpc = inst + 64u;
        s->pc = (struct slpl_piece *)A_(s, s->cpc * (u32)sizeof(struct slpl_piece));
        if (!s->pc) return fail(s, SLP_E_MEM);
        rc = translate(s, t->old_size);
        if (rc == -2 || rc == -3) return fail(s, SLP_E_MEM);                 /* more ranges than the instruction bytes allow for */
        if (rc || s->nlen != t->new_size) return fail(s, SLP_E_VCDIFF);
        s->work += 20000u + s->dllen / 16u;
        s->ph = P_OPS;
        return SLP_RUN; }
    case P_OPS: {
        u32 n = ops_gen(s, 0);
        s->dout = (u8 *)A_(s, n + 32u);
        if (!s->dout) return fail(s, SLP_E_MEM);
        ops_gen(s, s->dout);
        s->doutlen = n;
        if (t->mode == SLP_M_DELTA_IP && (n >= 4096u || !slp_delta_inplace_ok(s->dout, n, t->old_size, t->new_size))) return fail(s, SLP_E_DELTA);
        s->e[s->k].p_pos = n;
        s->src = s->dout;
        F_(s, s->pc); s->pc = 0;
        s->work += n / 16u;
        s->ph = P_Z_INIT;
        return SLP_RUN; }
    case P_RD_OLD:
        if (read_chunk(s, RID(s->k), s->old, t->old_size)) return fail(s, SLP_E_READ);
        if (s->pos >= t->old_size) {
            if (slp_crc32(0, s->old, t->old_size) != t->old_crc) return fail(s, SLP_E_INPUT);
            s->ph = P_VCDIFF;
        }
        return SLP_RUN;
    case P_VCDIFF: {
        u32 got = 0;
        if (slp_vcdiff(s->old, t->old_size, s->dl, s->dllen, s->nw, t->new_size, &got) || got != t->new_size) return fail(s, SLP_E_VCDIFF);
        if (s->k == 2) slp_fix_item_names(s->old, t->old_size, s->nw, t->new_size);
        if (slp_crc32(0, s->nw, t->new_size) != t->new_crc) return fail(s, SLP_E_INPUT);
        s->work += t->new_size / 16u;
        s->ph = P_MD_INIT;
        return SLP_RUN; }
    case P_MD_INIT: {
        void *blk = A_(s, slp_md_mem((int)t->old_size, t->W, t->STEP));
        s->dout = (u8 *)A_(s, t->dcap);
        if (!blk || !s->dout) { F_(s, blk); return fail(s, SLP_E_MEM); }
        slp_md_init(&s->md, s->old, (int)t->old_size, s->nw, (int)t->new_size, t->W, t->STEP, t->MINC, blk, s->dout, (int)t->dcap);
        s->md.start = (int *)blk;
        s->ph = P_MD_RUN;
        return SLP_RUN; }
    case P_MD_RUN: {
        int r = slp_md_step(&s->md, budget);
        s->work += s->md.used;
        if (r < 0) return fail(s, SLP_E_DELTA);
        if (r == 1) { s->doutlen = (u32)s->md.oc; s->ph = P_MD_CHECK; }
        return SLP_RUN; }
    case P_MD_CHECK: {
        u32 crc = 0;
        if (slp_delta_crc(s->old, t->old_size, s->dout, s->doutlen, t->new_size, &crc) || crc != t->new_crc) return fail(s, SLP_E_DELTA);
        s->e[s->k].p_pos = s->doutlen;
        s->src = s->dout;
        s->work += t->new_size / 32u;
        F_(s, s->md.start); s->md.start = 0;
        F_(s, s->old); s->old = 0;
        F_(s, s->nw); s->nw = 0;
        s->ph = P_Z_INIT;
        return SLP_RUN; }
    case P_Z_INIT: {
        u32 n = s->doutlen, zcap = n >= 65536u ? n - n / 4u : n - n / 8u + 64u;      /* the streams are far smaller (op streams about 60 %, slpCSS.dat 27 %); one that does not fit fails the build */
        s->zbuf = (u8 *)A_(s, slpl_lz_mem());
        s->zout = (u8 *)A_(s, zcap);
        if (!s->zbuf || !s->zout) return fail(s, SLP_E_MEM);
        slpl_lz_init(&s->z, s->src, n, s->zbuf, s->zout, zcap);
        s->ph = P_Z_RUN;
        return SLP_RUN; }
    case P_Z_RUN: {
        int r = slpl_lz_step(&s->z, budget);
        s->work += s->z.used;
        if (r < 0) return fail(s, SLP_E_MSLZ);
        if (r == 1) s->ph = P_Z_CHECK;
        return SLP_RUN; }
    case P_Z_CHECK: {
        struct slp_ent *e = &s->e[s->k];
        u32 zl = s->z.o, nlen = s->doutlen;
        int gap;
        if (slpl_lz_check(s->zout, zl, s->src, nlen)) return fail(s, SLP_E_MSLZ);
        s->work += nlen / 16u;
        gap = slp_mslz_gap(s->zout, zl, nlen);
        if (gap < 0) return fail(s, SLP_E_MSLZ);
        if (t->mode == SLP_M_MSLZ && (u32)gap + zl > rup(t->new_size, 32)) return fail(s, SLP_E_MSLZ);
        F_(s, s->zbuf); s->zbuf = 0;                  /* the work area and the op stream are not needed any more: free them before the stored copy is taken */
        F_(s, s->dout); s->dout = 0; s->src = 0;
        e->stored = (u8 *)A_(s, zl + 32u);
        if (!e->stored) return fail(s, SLP_E_MEM);
        mcopy(e->stored, s->zout, zl);
        e->stored_len = zl; e->gap = (u32)gap; e->mode = t->mode; e->new_size = t->new_size; e->old_size = t->old_size; e->crc_new = t->new_crc; e->crc_old = t->old_size ? t->old_crc : 0;
        e->p_old = slp_crc32(0, e->stored, zl);
        if (t->mode == SLP_M_MSLZ) e->p_pos = 0;
        F_(s, s->zout); s->zout = 0;
        s->ord++; s->ph = P_START;
        return SLP_RUN; }
    case P_ASM: {
        u32 off = BLOCK, total, k, blocks;
        u8 *g;
        const u8 *hd = s->io->map ? s->io->map(s->io->io.ctx, SLP_HEAD) : 0;
        if (!hd || s->io->io.size(s->io->io.ctx, SLP_HEAD) != (int)(0x40u + BLOCK)) return fail(s, SLP_E_READ);
        for (k = 0; k < s->n; k++) { s->e[k].data_off = off; off += rup(s->e[k].stored_len, BLOCK); }
        total = off;
        blocks = total / BLOCK;
        s->gci_raw = (u8 *)A_(s, 0x40u + total + 32u);
        if (!s->gci_raw) return fail(s, SLP_E_MEM);
        g = s->gci_raw;
        mzero(g, 0x40u + total);
        s->gci_data = g + 0x40;
        s->gci_blocks = blocks;
        mcopy(g, hd, 0x40u + BLOCK);                   /* dir entry + block 0 (comments, icon) from head.bin */
        g[0x38] = (u8)(blocks >> 8); g[0x39] = (u8)blocks;
        {
            u8 *b0 = g + 0x40, *toc = b0 + 0x1020;
            for (k = 0; k < s->n; k++) {
                struct slp_ent *e = &s->e[k];
                u8 *q = toc + 64u * k;
                const char *nm = SLP_TAB[k].name;
                u32 i;
                for (i = 0; nm[i]; i++) q[i] = (u8)nm[i];
                put32(q + 24, e->mode); put32(q + 28, e->new_size); put32(q + 32, e->old_size); put32(q + 36, e->stored_len); put32(q + 40, e->data_off);
                put32(q + 44, e->gap); put32(q + 48, e->crc_new); put32(q + 52, e->crc_old); put32(q + 56, e->p_pos); put32(q + 60, e->p_old);
                mcopy(b0 + e->data_off, e->stored, e->stored_len);
            }
            put32(b0 + 0x1000, 0x4D53504Bu); put32(b0 + 0x1004, 1); put32(b0 + 0x1008, s->n); put32(b0 + 0x100C, total);
            put32(b0 + 0x1010, slp_crc32(0, toc, 64u * s->n));
        }
        for (k = 0; k < SLP_NENT_MAX; k++) { F_(s, s->e[k].stored); s->e[k].stored = 0; }
        s->ph = P_DONE;
        return SLP_DONE; }
    default:
        return SLP_DONE;
    }
}

const u8 *slpl_gci_hdr(const struct slpl *s) { return s->gci_raw; }
const u8 *slpl_gci_data(const struct slpl *s) { return s->gci_data; }
u32 slpl_gci_blocks(const struct slpl *s) { return s->gci_blocks; }
