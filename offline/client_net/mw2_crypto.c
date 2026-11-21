/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* mw2_crypto: HMAC-SHA256, SHA-512, Ed25519 verify.  Strict C89 (the one gcc extension: __extension__ on the 64-bit typedefs). */
#include "mw2_crypto.h"
#include "../payload_pack/ms_sha256.h"

__extension__ typedef unsigned long long u64;
__extension__ typedef long long i64;

int mw2_ct_equal(const mw2_u8 *a, const mw2_u8 *b, mw2_u32 n)
{
    mw2_u8 d = 0;
    mw2_u32 i;
    for (i = 0; i < n; i++) d = (mw2_u8)(d | (a[i] ^ b[i]));
    return d == 0;
}

void mw2_hmac(const mw2_u8 key[32], const mw2_u8 *msg, mw2_u32 n, mw2_u8 out[32])
{
    mw2_u8 buf[64 + MW2_HMAC_MAX], outer[64 + 32];
    mw2_u32 i;
    if (n > MW2_HMAC_MAX) n = MW2_HMAC_MAX;
    for (i = 0; i < 64; i++) { mw2_u8 k = i < 32 ? key[i] : 0; buf[i] = (mw2_u8)(k ^ 0x36); outer[i] = (mw2_u8)(k ^ 0x5c); }
    for (i = 0; i < n; i++) buf[64 + i] = msg[i];
    ms_sha256(buf, 64 + n, outer + 64);
    ms_sha256(outer, 96, out);
}

/* ---------------------------------------------------------------- SHA-512 */
static const unsigned long K512[160] = {
    0x428a2f98UL, 0xd728ae22UL, 0x71374491UL, 0x23ef65cdUL,
    0xb5c0fbcfUL, 0xec4d3b2fUL, 0xe9b5dba5UL, 0x8189dbbcUL,
    0x3956c25bUL, 0xf348b538UL, 0x59f111f1UL, 0xb605d019UL,
    0x923f82a4UL, 0xaf194f9bUL, 0xab1c5ed5UL, 0xda6d8118UL,
    0xd807aa98UL, 0xa3030242UL, 0x12835b01UL, 0x45706fbeUL,
    0x243185beUL, 0x4ee4b28cUL, 0x550c7dc3UL, 0xd5ffb4e2UL,
    0x72be5d74UL, 0xf27b896fUL, 0x80deb1feUL, 0x3b1696b1UL,
    0x9bdc06a7UL, 0x25c71235UL, 0xc19bf174UL, 0xcf692694UL,
    0xe49b69c1UL, 0x9ef14ad2UL, 0xefbe4786UL, 0x384f25e3UL,
    0x0fc19dc6UL, 0x8b8cd5b5UL, 0x240ca1ccUL, 0x77ac9c65UL,
    0x2de92c6fUL, 0x592b0275UL, 0x4a7484aaUL, 0x6ea6e483UL,
    0x5cb0a9dcUL, 0xbd41fbd4UL, 0x76f988daUL, 0x831153b5UL,
    0x983e5152UL, 0xee66dfabUL, 0xa831c66dUL, 0x2db43210UL,
    0xb00327c8UL, 0x98fb213fUL, 0xbf597fc7UL, 0xbeef0ee4UL,
    0xc6e00bf3UL, 0x3da88fc2UL, 0xd5a79147UL, 0x930aa725UL,
    0x06ca6351UL, 0xe003826fUL, 0x14292967UL, 0x0a0e6e70UL,
    0x27b70a85UL, 0x46d22ffcUL, 0x2e1b2138UL, 0x5c26c926UL,
    0x4d2c6dfcUL, 0x5ac42aedUL, 0x53380d13UL, 0x9d95b3dfUL,
    0x650a7354UL, 0x8baf63deUL, 0x766a0abbUL, 0x3c77b2a8UL,
    0x81c2c92eUL, 0x47edaee6UL, 0x92722c85UL, 0x1482353bUL,
    0xa2bfe8a1UL, 0x4cf10364UL, 0xa81a664bUL, 0xbc423001UL,
    0xc24b8b70UL, 0xd0f89791UL, 0xc76c51a3UL, 0x0654be30UL,
    0xd192e819UL, 0xd6ef5218UL, 0xd6990624UL, 0x5565a910UL,
    0xf40e3585UL, 0x5771202aUL, 0x106aa070UL, 0x32bbd1b8UL,
    0x19a4c116UL, 0xb8d2d0c8UL, 0x1e376c08UL, 0x5141ab53UL,
    0x2748774cUL, 0xdf8eeb99UL, 0x34b0bcb5UL, 0xe19b48a8UL,
    0x391c0cb3UL, 0xc5c95a63UL, 0x4ed8aa4aUL, 0xe3418acbUL,
    0x5b9cca4fUL, 0x7763e373UL, 0x682e6ff3UL, 0xd6b2b8a3UL,
    0x748f82eeUL, 0x5defb2fcUL, 0x78a5636fUL, 0x43172f60UL,
    0x84c87814UL, 0xa1f0ab72UL, 0x8cc70208UL, 0x1a6439ecUL,
    0x90befffaUL, 0x23631e28UL, 0xa4506cebUL, 0xde82bde9UL,
    0xbef9a3f7UL, 0xb2c67915UL, 0xc67178f2UL, 0xe372532bUL,
    0xca273eceUL, 0xea26619cUL, 0xd186b8c7UL, 0x21c0c207UL,
    0xeada7dd6UL, 0xcde0eb1eUL, 0xf57d4f7fUL, 0xee6ed178UL,
    0x06f067aaUL, 0x72176fbaUL, 0x0a637dc5UL, 0xa2c898a6UL,
    0x113f9804UL, 0xbef90daeUL, 0x1b710b35UL, 0x131c471bUL,
    0x28db77f5UL, 0x23047d84UL, 0x32caab7bUL, 0x40c72493UL,
    0x3c9ebe0aUL, 0x15c9bebcUL, 0x431d67c4UL, 0x9c100d4cUL,
    0x4cc5d4beUL, 0xcb3e42b6UL, 0x597f299cUL, 0xfc657e2aUL,
    0x5fcb6fabUL, 0x3ad6faecUL, 0x6c44198cUL, 0x4a475817UL,
};
static const unsigned long H512[16] = {
    0x6a09e667UL, 0xf3bcc908UL, 0xbb67ae85UL, 0x84caa73bUL,
    0x3c6ef372UL, 0xfe94f82bUL, 0xa54ff53aUL, 0x5f1d36f1UL,
    0x510e527fUL, 0xade682d1UL, 0x9b05688cUL, 0x2b3e6c1fUL,
    0x1f83d9abUL, 0xfb41bd6bUL, 0x5be0cd19UL, 0x137e2179UL,
};
static u64 rd64(const mw2_u8 *p)
{
    u64 v = 0;
    int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}
#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static void sha512_block(u64 h[8], const mw2_u8 *p)
{
    u64 w[80], a[8], t1, t2;
    int i, j;
    for (i = 0; i < 16; i++) w[i] = rd64(p + 8 * i);
    for (i = 16; i < 80; i++) {
        u64 s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (j = 0; j < 8; j++) a[j] = h[j];
    for (i = 0; i < 80; i++) {
        u64 k = ((u64)K512[2 * i] << 32) | (u64)K512[2 * i + 1];
        t1 = a[7] + (ROR(a[4], 14) ^ ROR(a[4], 18) ^ ROR(a[4], 41)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k + w[i];
        t2 = (ROR(a[0], 28) ^ ROR(a[0], 34) ^ ROR(a[0], 39)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        for (j = 7; j > 0; j--) a[j] = a[j - 1];
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (j = 0; j < 8; j++) h[j] += a[j];
}
void mw2_sha512(const mw2_u8 *msg, mw2_u32 n, mw2_u8 out[64])
{
    u64 h[8];
    mw2_u8 blk[128];
    mw2_u32 i, done = 0;
    int j;
    for (j = 0; j < 8; j++) h[j] = ((u64)H512[2 * j] << 32) | (u64)H512[2 * j + 1];
    while (n - done >= 128) { sha512_block(h, msg + done); done += 128; }
    for (i = 0; i < 128; i++) blk[i] = 0;
    for (i = 0; i < n - done; i++) blk[i] = msg[done + i];
    blk[n - done] = 0x80;
    if (n - done >= 112) { sha512_block(h, blk); for (i = 0; i < 128; i++) blk[i] = 0; }
    {
        u64 bits = (u64)n * 8u;
        for (i = 0; i < 8; i++) blk[120 + i] = (mw2_u8)(bits >> (56 - 8 * i));
    }
    sha512_block(h, blk);
    for (j = 0; j < 8; j++) for (i = 0; i < 8; i++) out[8 * j + i] = (mw2_u8)(h[j] >> (56 - 8 * i));
}

/* ---------------------------------------------------------------- Ed25519 verify (TweetNaCl-style field and group code) */
typedef i64 gf[16];
static const gf gf0 = {0}, gf1 = {1};
static const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static void set25519(gf r, const gf a) { int i; for (i = 0; i < 16; i++) r[i] = a[i]; }
static void car25519(gf o)
{
    int i;
    i64 c;
    for (i = 0; i < 16; i++) {
        o[i] += ((i64)1 << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void sel25519(gf p, gf q, int b)
{
    i64 t, c = ~((i64)b - 1);
    int i;
    for (i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(mw2_u8 *o, const gf n)
{
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) { o[2 * i] = (mw2_u8)(t[i] & 0xff); o[2 * i + 1] = (mw2_u8)(t[i] >> 8); }
}
static int neq25519(const gf a, const gf b)
{
    mw2_u8 c[32], d[32];
    pack25519(c, a); pack25519(d, b);
    return !mw2_ct_equal(c, d, 32);
}
static mw2_u8 par25519(const gf a) { mw2_u8 d[32]; pack25519(d, a); return (mw2_u8)(d[0] & 1); }
static void unpack25519(gf o, const mw2_u8 *n)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fA(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fZ(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fM(gf o, const gf a, const gf b)
{
    i64 t[31];
    int i, j;
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fS(gf o, const gf a) { fM(o, a, a); }
static void inv25519(gf o, const gf i)
{
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) { fS(c, c); if (a != 2 && a != 4) fM(c, c, i); }
    for (a = 0; a < 16; a++) o[a] = c[a];
}
static void pow2523(gf o, const gf i)
{
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 250; a >= 0; a--) { fS(c, c); if (a != 1) fM(c, c, i); }
    for (a = 0; a < 16; a++) o[a] = c[a];
}
static void gadd(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    fZ(a, p[1], p[0]); fZ(t, q[1], q[0]); fM(a, a, t);
    fA(b, p[0], p[1]); fA(t, q[0], q[1]); fM(b, b, t);
    fM(c, p[3], q[3]); fM(c, c, D2);
    fM(d, p[2], q[2]); fA(d, d, d);
    fZ(e, b, a); fZ(f, d, c); fA(g, d, c); fA(h, b, a);
    fM(p[0], e, f); fM(p[1], h, g); fM(p[2], g, f); fM(p[3], e, h);
}
#if !defined(MW2_FAST_VERIFY) || defined(MW2_WITH_SIGN)
static void cswap(gf p[4], gf q[4], int b)
{
    int i;
    for (i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}
#endif
static void gpack(mw2_u8 *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    fM(tx, p[0], zi); fM(ty, p[1], zi);
    pack25519(r, ty);
    r[31] = (mw2_u8)(r[31] ^ (par25519(tx) << 7));
}
#if !defined(MW2_FAST_VERIFY) || defined(MW2_WITH_SIGN)
static void scalarmult(gf p[4], gf q[4], const mw2_u8 *s)
{
    int i;
    set25519(p[0], gf0); set25519(p[1], gf1); set25519(p[2], gf1); set25519(p[3], gf0);
    for (i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        gadd(q, p);
        gadd(p, p);
        cswap(p, q, b);
    }
}
static void scalarbase(gf p[4], const mw2_u8 *s)
{
    gf q[4];
    set25519(q[0], X); set25519(q[1], Y); set25519(q[2], gf1); fM(q[3], X, Y);
    scalarmult(p, q, s);
}
#endif
static const i64 L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
static void modL(mw2_u8 *r, i64 x[64])
{
    i64 carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) { x[j] += carry - (x[31] >> 4) * L[j]; carry = x[j] >> 8; x[j] &= 255; }
    for (j = 0; j < 32; j++) x[j] -= carry * L[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = (mw2_u8)(x[i] & 255); }
}
static void reduce(mw2_u8 *r)
{
    i64 x[64];
    int i;
    for (i = 0; i < 64; i++) x[i] = (i64)r[i];
    for (i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}
static int unpackneg(gf r[4], const mw2_u8 p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    fS(num, r[1]);
    fM(den, num, D);
    fZ(num, num, r[2]);
    fA(den, r[2], den);
    fS(den2, den); fS(den4, den2); fM(den6, den4, den2);
    fM(t, den6, num); fM(t, t, den);
    pow2523(t, t);
    fM(t, t, num); fM(t, t, den); fM(t, t, den);
    fM(r[0], t, den);
    fS(chk, r[0]); fM(chk, chk, den);
    if (neq25519(chk, num)) fM(r[0], r[0], I);
    fS(chk, r[0]); fM(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) fZ(r[0], gf0, r[0]);
    fM(r[3], r[0], r[1]);
    return 0;
}
static int s_below_l(const mw2_u8 *s)
{
    int i;
    for (i = 31; i >= 0; i--) {
        if (s[i] < (mw2_u8)L[i]) return 1;
        if (s[i] > (mw2_u8)L[i]) return 0;
    }
    return 0;
}
#ifdef MW2_FAST_VERIFY
/* Verification-only double scalar multiplication [h](-A) + [s]B: joint binary NAF (digits 0, +-1) over one shared chain of doublings.  Verification handles public data only,
   so the variable-time branches are fine; the group law is the same complete addition gadd() as the ladder, so the result is the same point (differential test:
   the project's other tooling).  About 253 doublings + 170 additions instead of 1024 additions ( 5). */
static void bnaf(signed char d[258], const mw2_u8 s[32])
{
    mw2_u8 k[34];
    int i, j;
    for (i = 0; i < 32; i++) k[i] = s[i];
    k[32] = 0; k[33] = 0;
    for (i = 0; i < 258; i++) {
        int di = 0;
        if (k[0] & 1) {
            di = 2 - (k[0] & 3);                              /* k = 1 mod 4: +1, k = 3 mod 4: -1 */
            if (di > 0) k[0] = (mw2_u8)(k[0] - 1);
            else for (j = 0; j < 34; j++) { k[j] = (mw2_u8)(k[j] + 1); if (k[j]) break; }
        }
        d[i] = (signed char)di;
        for (j = 0; j < 33; j++) k[j] = (mw2_u8)((k[j] >> 1) | (k[j + 1] << 7));
        k[33] = (mw2_u8)(k[33] >> 1);
    }
}
static void gneg(gf o[4], gf p[4])
{
    fZ(o[0], gf0, p[0]); set25519(o[1], p[1]); set25519(o[2], p[2]); fZ(o[3], gf0, p[3]);
}
static void gdsm(gf r[4], gf nA[4], const mw2_u8 h[32], const mw2_u8 s[32])
{
    signed char dh[258], ds[258];
    gf b[4], t[4];
    int i, started = 0;
    set25519(b[0], X); set25519(b[1], Y); set25519(b[2], gf1); fM(b[3], X, Y);
    bnaf(dh, h); bnaf(ds, s);
    set25519(r[0], gf0); set25519(r[1], gf1); set25519(r[2], gf1); set25519(r[3], gf0);
    for (i = 257; i >= 0; i--) {
        if (started) gadd(r, r);
        if (dh[i]) { if (dh[i] > 0) gadd(r, nA); else { gneg(t, nA); gadd(r, t); } started = 1; }
        if (ds[i]) { if (ds[i] > 0) gadd(r, b); else { gneg(t, b); gadd(r, t); } started = 1; }
    }
}
#endif
int mw2_ed25519_verify(const mw2_u8 pub[32], const mw2_u8 *msg, mw2_u32 n, const mw2_u8 sig[64])
{
    mw2_u8 t[32], h[64], buf[64 + MW2_VERIFY_MAX];
    gf p[4], q[4];
    mw2_u32 i;
    if (n > MW2_VERIFY_MAX || !s_below_l(sig + 32)) return 0;
    if (unpackneg(q, pub)) return 0;
    for (i = 0; i < 32; i++) { buf[i] = sig[i]; buf[32 + i] = pub[i]; }
    for (i = 0; i < n; i++) buf[64 + i] = msg[i];
    mw2_sha512(buf, 64 + n, h);
    reduce(h);
#ifdef MW2_FAST_VERIFY
    gdsm(p, q, h, sig + 32);
#else
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    gadd(p, q);
#endif
    gpack(t, p);
    return mw2_ct_equal(sig, t, 32);
}

#ifdef MW2_WITH_SIGN
/* ---------------------------------------------------------------- Ed25519 signing (RFC 8032, deterministic: no per-signature randomness)
   Compiled only with -DMW2_WITH_SIGN (the device-key code of ); nothing above changes.  The seed is the 32-byte private key.
   The scalar multiplication is the same constant-time ladder as verification (conditional swaps, no secret-dependent branches or indexing). */
static void mw2_wipe(void *p, mw2_u32 n) { volatile mw2_u8 *q = (volatile mw2_u8 *)p; mw2_u32 i; for (i = 0; i < n; i++) q[i] = 0; }
void mw2_ed25519_pubkey(mw2_u8 pub[32], const mw2_u8 seed[32])
{
    mw2_u8 d[64];
    gf p[4];
    mw2_sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    scalarbase(p, d);
    gpack(pub, p);
    mw2_wipe(d, sizeof d);
}
int mw2_ed25519_sign(mw2_u8 sig[64], const mw2_u8 *msg, mw2_u32 n, const mw2_u8 seed[32])
{
    mw2_u8 d[64], r[64], h[64], pub[32], buf[64 + MW2_SIGN_MAX];
    i64 x[64];
    gf p[4];
    mw2_u32 i, j;
    if (n > MW2_SIGN_MAX) return 0;
    mw2_sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    scalarbase(p, d); gpack(pub, p);
    for (i = 0; i < 32; i++) buf[i] = d[32 + i];
    for (i = 0; i < n; i++) buf[32 + i] = msg[i];
    mw2_sha512(buf, 32 + n, r);
    reduce(r);
    scalarbase(p, r); gpack(sig, p);                            /* R */
    for (i = 0; i < 32; i++) { buf[i] = sig[i]; buf[32 + i] = pub[i]; }
    for (i = 0; i < n; i++) buf[64 + i] = msg[i];
    mw2_sha512(buf, 64 + n, h);
    reduce(h);
    for (i = 0; i < 64; i++) x[i] = 0;
    for (i = 0; i < 32; i++) x[i] = (i64)r[i];
    for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) x[i + j] += (i64)h[i] * (i64)d[j];
    modL(sig + 32, x);
    mw2_wipe(d, sizeof d); mw2_wipe(r, sizeof r); mw2_wipe(buf, sizeof buf); mw2_wipe(x, sizeof x);
    return 1;
}
#endif
