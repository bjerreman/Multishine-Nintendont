/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#include "sdcrypto.h"

__extension__ typedef unsigned long long sdc_u64;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* ------------------------------------------------------------------ SHA-256 */
static const sdc_u32 K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha_block(sdc_u32 h[8], const sdc_u8 *p)
{
    sdc_u32 w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((sdc_u32)p[4 * i] << 24) | ((sdc_u32)p[4 * i + 1] << 16) | ((sdc_u32)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        sdc_u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        sdc_u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sdc_sha_init(struct sdc_sha *c)
{
    static const sdc_u32 iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    int i;
    for (i = 0; i < 8; i++) c->h[i] = iv[i];
    c->n = 0;
}
void sdc_sha_update(struct sdc_sha *c, const sdc_u8 *p, sdc_u32 n)
{
    sdc_u32 fill = c->n & 63, i;
    c->n += n;
    for (i = 0; i < n; i++) {
        c->buf[fill++] = p[i];
        if (fill == 64) { sha_block(c->h, c->buf); fill = 0; }
    }
}
void sdc_sha_final(struct sdc_sha *c, sdc_u8 out[32])
{
    sdc_u32 fill = c->n & 63, bits_hi = c->n >> 29, bits_lo = c->n << 3, i;
    c->buf[fill++] = 0x80;
    if (fill > 56) {
        while (fill < 64) c->buf[fill++] = 0;
        sha_block(c->h, c->buf);
        fill = 0;
    }
    while (fill < 56) c->buf[fill++] = 0;
    for (i = 0; i < 4; i++) { c->buf[56 + i] = (sdc_u8)(bits_hi >> (24 - 8 * i)); c->buf[60 + i] = (sdc_u8)(bits_lo >> (24 - 8 * i)); }
    sha_block(c->h, c->buf);
    for (i = 0; i < 32; i++) out[i] = (sdc_u8)(c->h[i >> 2] >> (24 - 8 * (i & 3)));
}
void sdc_sha256(const sdc_u8 *p, sdc_u32 n, sdc_u8 out[32])
{
    struct sdc_sha c;
    sdc_sha_init(&c);
    sdc_sha_update(&c, p, n);
    sdc_sha_final(&c, out);
}

void sdc_wipe(void *p, sdc_u32 n)
{
    volatile sdc_u8 *q = (volatile sdc_u8 *)p;
    while (n--) *q++ = 0;
}
int sdc_ct_eq(const sdc_u8 *a, const sdc_u8 *b, sdc_u32 n)
{
    sdc_u8 d = 0;
    sdc_u32 i;
    for (i = 0; i < n; i++) d = (sdc_u8)(d | (a[i] ^ b[i]));
    return d == 0;
}

/* ------------------------------------------------------------------ HMAC / HKDF */
void sdc_hmac(const sdc_u8 *key, sdc_u32 klen, const sdc_u8 *msg, sdc_u32 mlen, sdc_u8 out[32])
{
    sdc_u8 k[64], pad[64], ih[32];
    struct sdc_sha c;
    sdc_u32 i;
    for (i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) sdc_sha256(key, klen, k);
    else for (i = 0; i < klen; i++) k[i] = key[i];
    for (i = 0; i < 64; i++) pad[i] = (sdc_u8)(k[i] ^ 0x36);
    sdc_sha_init(&c); sdc_sha_update(&c, pad, 64); sdc_sha_update(&c, msg, mlen); sdc_sha_final(&c, ih);
    for (i = 0; i < 64; i++) pad[i] = (sdc_u8)(k[i] ^ 0x5c);
    sdc_sha_init(&c); sdc_sha_update(&c, pad, 64); sdc_sha_update(&c, ih, 32); sdc_sha_final(&c, out);
    sdc_wipe(k, 64);
}
void sdc_hkdf_extract(const sdc_u8 salt[32], const sdc_u8 *ikm, sdc_u32 ilen, sdc_u8 out[32])
{
    sdc_hmac(salt, 32, ikm, ilen, out);
}
void sdc_expand_label(const sdc_u8 secret[32], const char *label, const sdc_u8 *ctx, sdc_u32 clen, sdc_u32 len, sdc_u8 *out)
{
    sdc_u8 info[2 + 1 + 6 + 12 + 1 + 32 + 1];
    sdc_u8 t[32];
    sdc_u32 n = 0, ll = 0, i;
    while (label[ll]) ll++;
    info[n++] = (sdc_u8)(len >> 8); info[n++] = (sdc_u8)len;
    info[n++] = (sdc_u8)(6 + ll);
    info[n++] = 't'; info[n++] = 'l'; info[n++] = 's'; info[n++] = '1'; info[n++] = '3'; info[n++] = ' ';
    for (i = 0; i < ll; i++) info[n++] = (sdc_u8)label[i];
    info[n++] = (sdc_u8)clen;
    for (i = 0; i < clen; i++) info[n++] = ctx[i];
    info[n++] = 1;                                   /* HKDF-Expand T(1); len <= 32 */
    sdc_hmac(secret, 32, info, n, t);
    for (i = 0; i < len; i++) out[i] = t[i];
    sdc_wipe(t, 32);
}

/* ------------------------------------------------------------------ ChaCha20 */
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROL(d, 16); c += d; b ^= c; b = ROL(b, 12); a += b; d ^= a; d = ROL(d, 8); c += d; b ^= c; b = ROL(b, 7)
static sdc_u32 le32(const sdc_u8 *p) { return (sdc_u32)p[0] | ((sdc_u32)p[1] << 8) | ((sdc_u32)p[2] << 16) | ((sdc_u32)p[3] << 24); }
static void put_le32(sdc_u8 *p, sdc_u32 v) { p[0] = (sdc_u8)v; p[1] = (sdc_u8)(v >> 8); p[2] = (sdc_u8)(v >> 16); p[3] = (sdc_u8)(v >> 24); }

static void chacha_block(const sdc_u32 in[16], sdc_u8 out[64])
{
    sdc_u32 x[16];
    int i;
    for (i = 0; i < 16; i++) x[i] = in[i];
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + in[i]);
}
static void chacha_init(sdc_u32 st[16], const sdc_u8 key[32], sdc_u32 counter, const sdc_u8 nonce[12])
{
    int i;
    st[0] = 0x61707865u; st[1] = 0x3320646eu; st[2] = 0x79622d32u; st[3] = 0x6b206574u;
    for (i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[12] = counter;
    for (i = 0; i < 3; i++) st[13 + i] = le32(nonce + 4 * i);
}
void sdc_chacha20_xor(const sdc_u8 key[32], sdc_u32 counter, const sdc_u8 nonce[12], const sdc_u8 *in, sdc_u8 *out, sdc_u32 n)
{
    sdc_u32 st[16], i, j, m;
    sdc_u8 ks[64];
    chacha_init(st, key, counter, nonce);
    for (i = 0; i < n; i += 64) {
        chacha_block(st, ks);
        st[12]++;
        m = n - i < 64 ? n - i : 64;
        for (j = 0; j < m; j++) out[i + j] = (sdc_u8)(in[i + j] ^ ks[j]);
    }
    sdc_wipe(ks, 64);
    sdc_wipe(st, sizeof st);
}

/* ------------------------------------------------------------------ Poly1305 (26-bit limbs) */
struct poly {
    sdc_u32 r[5], h[5], pad[4];
    sdc_u8 buf[16];
    sdc_u32 fill;
};
static void poly_init(struct poly *p, const sdc_u8 key[32])
{
    int i;
    p->r[0] = le32(key) & 0x3ffffffu;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03u;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ffu;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fffu;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffffu;
    for (i = 0; i < 5; i++) p->h[i] = 0;
    for (i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
    p->fill = 0;
}
static void poly_blocks(struct poly *p, const sdc_u8 *m, sdc_u32 n, sdc_u32 hibit)
{
    sdc_u32 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    sdc_u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    sdc_u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    sdc_u64 d0, d1, d2, d3, d4;
    while (n >= 16) {
        h0 += le32(m) & 0x3ffffffu;
        h1 += (le32(m + 3) >> 2) & 0x3ffffffu;
        h2 += (le32(m + 6) >> 4) & 0x3ffffffu;
        h3 += (le32(m + 9) >> 6) & 0x3ffffffu;
        h4 += (le32(m + 12) >> 8) | hibit;
        d0 = (sdc_u64)h0 * r0 + (sdc_u64)h1 * s4 + (sdc_u64)h2 * s3 + (sdc_u64)h3 * s2 + (sdc_u64)h4 * s1;
        d1 = (sdc_u64)h0 * r1 + (sdc_u64)h1 * r0 + (sdc_u64)h2 * s4 + (sdc_u64)h3 * s3 + (sdc_u64)h4 * s2;
        d2 = (sdc_u64)h0 * r2 + (sdc_u64)h1 * r1 + (sdc_u64)h2 * r0 + (sdc_u64)h3 * s4 + (sdc_u64)h4 * s3;
        d3 = (sdc_u64)h0 * r3 + (sdc_u64)h1 * r2 + (sdc_u64)h2 * r1 + (sdc_u64)h3 * r0 + (sdc_u64)h4 * s4;
        d4 = (sdc_u64)h0 * r4 + (sdc_u64)h1 * r3 + (sdc_u64)h2 * r2 + (sdc_u64)h3 * r1 + (sdc_u64)h4 * r0;
        c = (sdc_u32)(d0 >> 26); h0 = (sdc_u32)d0 & 0x3ffffffu;
        d1 += c; c = (sdc_u32)(d1 >> 26); h1 = (sdc_u32)d1 & 0x3ffffffu;
        d2 += c; c = (sdc_u32)(d2 >> 26); h2 = (sdc_u32)d2 & 0x3ffffffu;
        d3 += c; c = (sdc_u32)(d3 >> 26); h3 = (sdc_u32)d3 & 0x3ffffffu;
        d4 += c; c = (sdc_u32)(d4 >> 26); h4 = (sdc_u32)d4 & 0x3ffffffu;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;
        m += 16; n -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}
static void poly_update(struct poly *p, const sdc_u8 *m, sdc_u32 n)
{
    sdc_u32 i;
    if (p->fill) {
        while (n && p->fill < 16) { p->buf[p->fill++] = *m++; n--; }
        if (p->fill < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->fill = 0;
    }
    if (n >= 16) {
        sdc_u32 k = n & ~15u;
        poly_blocks(p, m, k, 1u << 24);
        m += k; n -= k;
    }
    for (i = 0; i < n; i++) p->buf[p->fill++] = m[i];
}
static void poly_final(struct poly *p, sdc_u8 tag[16])
{
    sdc_u32 h0, h1, h2, h3, h4, c, g0, g1, g2, g3, g4, mask;
    sdc_u64 f;
    if (p->fill) {
        sdc_u32 i = p->fill;
        p->buf[i++] = 1;
        while (i < 16) p->buf[i++] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    h0 = p->h[0]; h1 = p->h[1]; h2 = p->h[2]; h3 = p->h[3]; h4 = p->h[4];
    c = h1 >> 26; h1 &= 0x3ffffffu; h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu; h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu; h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
    g4 = h4 + c - (1u << 26);
    mask = (g4 >> 31) - 1u;                       /* all ones when h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));
    f = (sdc_u64)h0 + p->pad[0]; h0 = (sdc_u32)f;
    f = (sdc_u64)h1 + p->pad[1] + (f >> 32); h1 = (sdc_u32)f;
    f = (sdc_u64)h2 + p->pad[2] + (f >> 32); h2 = (sdc_u32)f;
    f = (sdc_u64)h3 + p->pad[3] + (f >> 32); h3 = (sdc_u32)f;
    put_le32(tag, h0); put_le32(tag + 4, h1); put_le32(tag + 8, h2); put_le32(tag + 12, h3);
    sdc_wipe(p, sizeof *p);
}

static void aead_tag(const sdc_u8 key[32], const sdc_u8 nonce[12], const sdc_u8 *aad, sdc_u32 alen, const sdc_u8 *ct, sdc_u32 n, sdc_u8 tag[16])
{
    sdc_u8 pk[64], zero[64], lens[16], padz[16];
    sdc_u32 i;
    struct poly p;
    for (i = 0; i < 64; i++) zero[i] = 0;
    for (i = 0; i < 16; i++) padz[i] = 0;
    sdc_chacha20_xor(key, 0, nonce, zero, pk, 64);
    poly_init(&p, pk);
    poly_update(&p, aad, alen);
    if (alen & 15) poly_update(&p, padz, 16 - (alen & 15));
    poly_update(&p, ct, n);
    if (n & 15) poly_update(&p, padz, 16 - (n & 15));
    for (i = 0; i < 8; i++) { lens[i] = (sdc_u8)((i < 4 ? alen >> (8 * i) : 0)); lens[8 + i] = (sdc_u8)((i < 4 ? n >> (8 * i) : 0)); }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    sdc_wipe(pk, 64);
}
void sdc_aead_seal(const sdc_u8 key[32], const sdc_u8 nonce[12], const sdc_u8 *aad, sdc_u32 alen, const sdc_u8 *in, sdc_u32 n, sdc_u8 *out)
{
    sdc_chacha20_xor(key, 1, nonce, in, out, n);
    aead_tag(key, nonce, aad, alen, out, n, out + n);
}
int sdc_aead_open(const sdc_u8 key[32], const sdc_u8 nonce[12], const sdc_u8 *aad, sdc_u32 alen, const sdc_u8 *in, sdc_u32 n, sdc_u8 *out)
{
    sdc_u8 tag[16];
    aead_tag(key, nonce, aad, alen, in, n, tag);
    if (!sdc_ct_eq(tag, in + n, 16)) return 0;
    sdc_chacha20_xor(key, 1, nonce, in, out, n);
    return 1;
}

/* ------------------------------------------------------------------ RNG */
void sdc_rng_init(struct sdc_rng *r)
{
    sdc_u32 i;
    for (i = 0; i < 32; i++) r->key[i] = 0;
    r->ctr = 0; r->credit = 0; r->seeded = 0;
    sdc_sha_init(&r->pool);
}
void sdc_rng_mix(struct sdc_rng *r, const sdc_u8 *p, sdc_u32 n, sdc_u32 credit_bits)
{
    sdc_sha_update(&r->pool, p, n);
    r->credit += credit_bits;
}
void sdc_rng_mix_u32(struct sdc_rng *r, sdc_u32 v, sdc_u32 credit_bits)
{
    sdc_u8 b[4];
    put_le32(b, v);
    sdc_rng_mix(r, b, 4, credit_bits);
}
int sdc_rng_ready(const struct sdc_rng *r) { return r->credit >= 128; }
void sdc_rng_bytes(struct sdc_rng *r, sdc_u8 *out, sdc_u32 n)
{
    sdc_u8 d[32], blk[64], nonce[12];
    struct sdc_sha c;
    sdc_u32 i, st[16], done = 0, m;
    /* fold the pool into the key: key = SHA256(key || pool_digest || ctr) */
    c = r->pool;
    sdc_sha_final(&c, d);
    sdc_sha_init(&c); sdc_sha_update(&c, r->key, 32); sdc_sha_update(&c, d, 32);
    put_le32(blk, r->ctr++); sdc_sha_update(&c, blk, 4);
    sdc_sha_final(&c, r->key);
    sdc_sha_init(&r->pool); sdc_sha_update(&r->pool, d, 32);     /* chain: old pool state is never discarded, only extended */
    r->seeded = 1;
    for (i = 0; i < 12; i++) nonce[i] = 0;
    while (done < n) {
        chacha_init(st, r->key, 0, nonce);
        chacha_block(st, blk);
        for (i = 0; i < 32; i++) r->key[i] = blk[i];              /* fast key erasure */
        m = n - done < 32 ? n - done : 32;
        for (i = 0; i < m; i++) out[done + i] = blk[32 + i];
        done += m;
        nonce[0]++;
    }
    sdc_wipe(blk, 64); sdc_wipe(d, 32); sdc_wipe(st, sizeof st);
}
