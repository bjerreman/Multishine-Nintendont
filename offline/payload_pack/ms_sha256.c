/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader)
 * One-shot SHA-256 (FIPS 180-4), 32-bit words kept in ms_u32 and masked so it is correct on 64-bit hosts too. */
#include "ms_sha256.h"
#define M 0xFFFFFFFFUL
#define ROR(x, n) ((((x) >> (n)) | ((x) << (32 - (n)))) & M)

static const ms_u32 K[64] = {
0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL};

static void block(ms_u32 h[8], const ms_u8 *p)
{
    ms_u32 w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((ms_u32)p[4*i] << 24) | ((ms_u32)p[4*i+1] << 16) | ((ms_u32)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; i++) {
        ms_u32 s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        ms_u32 s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = (w[i-16] + s0 + w[i-7] + s1) & M;
    }
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
    for (i = 0; i < 64; i++) {
        t1 = (hh + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g & M)) + K[i] + w[i]) & M;
        t2 = ((ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & c) ^ (b & c))) & M;
        hh=g; g=f; f=e; e=(d+t1)&M; d=c; c=b; b=a; a=(t1+t2)&M;
    }
    h[0]=(h[0]+a)&M; h[1]=(h[1]+b)&M; h[2]=(h[2]+c)&M; h[3]=(h[3]+d)&M;
    h[4]=(h[4]+e)&M; h[5]=(h[5]+f)&M; h[6]=(h[6]+g)&M; h[7]=(h[7]+hh)&M;
}

void ms_sha256(const ms_u8 *data, ms_u32 n, ms_u8 out[32])
{
    ms_u32 h[8], i, full = n / 64, rem = n % 64;
    ms_u8 tail[128];
    ms_u32 tl, bits_hi, bits_lo;
    h[0]=0x6a09e667UL; h[1]=0xbb67ae85UL; h[2]=0x3c6ef372UL; h[3]=0xa54ff53aUL;
    h[4]=0x510e527fUL; h[5]=0x9b05688cUL; h[6]=0x1f83d9abUL; h[7]=0x5be0cd19UL;
    for (i = 0; i < full; i++) block(h, data + 64 * i);
    for (i = 0; i < rem; i++) tail[i] = data[64 * full + i];
    tail[rem] = 0x80;
    tl = (rem < 56) ? 64 : 128;
    for (i = rem + 1; i < tl; i++) tail[i] = 0;
    bits_hi = (n >> 29) & M;
    bits_lo = (n << 3) & M;
    tail[tl-8]=(ms_u8)(bits_hi>>24); tail[tl-7]=(ms_u8)(bits_hi>>16); tail[tl-6]=(ms_u8)(bits_hi>>8); tail[tl-5]=(ms_u8)bits_hi;
    tail[tl-4]=(ms_u8)(bits_lo>>24); tail[tl-3]=(ms_u8)(bits_lo>>16); tail[tl-2]=(ms_u8)(bits_lo>>8); tail[tl-1]=(ms_u8)bits_lo;
    block(h, tail);
    if (tl == 128) block(h, tail + 64);
    for (i = 0; i < 8; i++) {
        out[4*i]=(ms_u8)(h[i]>>24); out[4*i+1]=(ms_u8)(h[i]>>16); out[4*i+2]=(ms_u8)(h[i]>>8); out[4*i+3]=(ms_u8)h[i];
    }
}

/* ---- incremental form (shares block with the one-shot function above; same byte-for-byte result, tested against it) ---- */
void ms_sha256_init(ms_sha256_ctx *c)
{
    c->h[0]=0x6a09e667UL; c->h[1]=0xbb67ae85UL; c->h[2]=0x3c6ef372UL; c->h[3]=0xa54ff53aUL;
    c->h[4]=0x510e527fUL; c->h[5]=0x9b05688cUL; c->h[6]=0x1f83d9abUL; c->h[7]=0x5be0cd19UL;
    c->len = 0; c->fill = 0;
}

void ms_sha256_update(ms_sha256_ctx *c, const ms_u8 *data, ms_u32 n)
{
    c->len = (c->len + n) & M;
    while (n) {
        if (c->fill == 0 && n >= 64) { block(c->h, data); data += 64; n -= 64; continue; }
        while (n && c->fill < 64) { c->buf[c->fill++] = *data++; n--; }
        if (c->fill == 64) { block(c->h, c->buf); c->fill = 0; }
    }
}

void ms_sha256_final(ms_sha256_ctx *c, ms_u8 out[32])
{
    ms_u32 i, bits_hi = (c->len >> 29) & M, bits_lo = (c->len << 3) & M;
    c->buf[c->fill++] = 0x80;
    if (c->fill > 56) { while (c->fill < 64) c->buf[c->fill++] = 0; block(c->h, c->buf); c->fill = 0; }
    while (c->fill < 56) c->buf[c->fill++] = 0;
    c->buf[56]=(ms_u8)(bits_hi>>24); c->buf[57]=(ms_u8)(bits_hi>>16); c->buf[58]=(ms_u8)(bits_hi>>8); c->buf[59]=(ms_u8)bits_hi;
    c->buf[60]=(ms_u8)(bits_lo>>24); c->buf[61]=(ms_u8)(bits_lo>>16); c->buf[62]=(ms_u8)(bits_lo>>8); c->buf[63]=(ms_u8)bits_lo;
    block(c->h, c->buf);
    for (i = 0; i < 8; i++) {
        out[4*i]=(ms_u8)(c->h[i]>>24); out[4*i+1]=(ms_u8)(c->h[i]>>16); out[4*i+2]=(ms_u8)(c->h[i]>>8); out[4*i+3]=(ms_u8)c->h[i];
    }
}
