/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* mslz_opt: optimal-parse ENCODER for the MSLZ stream format (tools/payload_pack/mslz.py, decoder offline/payload_pack/ms_lz.c).  The stream format and the console
 * decoder are unchanged: this only chooses a cheaper token sequence than the greedy+lazy reference compressor (about 1-3 % fewer bytes on PPC code).
 *
 *   mslz_opt IN OUT [depth] [cap]
 *
 *   depth  hash-chain search depth (default 1024)
 *   cap    0 (default): minimise the stream.  cap > 0: minimise the stream among the streams that decode IN PLACE with g + z <= cap, i.e. the in-place gap (mscf_z.inplace_gap)
 *          never exceeds cap - z; used for the pack and overlay entries, where the destination is rup32(new_size) bytes (build_pack.plan_mslz).  Exit code 3 and no output when
 *          no such stream exists.
 *
 * Algorithm: backward dynamic programme over "sequence boundaries" (0, every match end, n).  S[j] = the cheapest stream suffix that starts with a sequence at j.
 * A match costs the same for every offset (2 bytes), so only the longest match per position matters, any shorter length being a prefix of it.  The in-place condition
 * is a condition on the suffix cost at every match end e: S[e] <= cap - e (derived  3.1), so infeasible boundaries are simply not
 * allowed to be used.  Deterministic: no randomness, fixed tie-breaks (longer match, then shorter literal run).  C99, host only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
#define MINM 4
#define MAXOFF 65535
#define INF 0x3fffffff

static int ext(long n) { return n >= 15 ? (int)(1 + (n - 15) / 255) : 0; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: mslz_opt IN OUT [depth] [cap]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n + 8);
    if (n && fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "read\n"); return 2; }
    fclose(f);
    int depth = argc > 3 ? atoi(argv[3]) : 1024;
    long cap = argc > 4 ? atol(argv[4]) : 0;

    int HB = 1 << 20;
    int *head = malloc(sizeof(int) * HB), *prev = malloc(sizeof(int) * (n + 1));
    for (int i = 0; i < HB; i++) head[i] = -1;
    int *Lmax = calloc(n + 1, sizeof(int)), *Off = calloc(n + 1, sizeof(int));
    for (long i = 0; i + MINM <= n; i++) {
        u32 k = (u32)((d[i] | d[i + 1] << 8 | d[i + 2] << 16 | (u32)d[i + 3] << 24) * 2654435761u) >> 12;
        int c = head[k], t = depth;
        long bl = 0, bo = 0;
        while (c >= 0 && i - c <= MAXOFF && t--) {
            if (bl == 0 || d[c + bl] == d[i + bl]) {
                long l = 0;
                while (i + l < n && d[c + l] == d[i + l]) l++;
                if (l > bl) { bl = l; bo = i - c; if (i + l >= n) break; }
            }
            c = prev[c];
        }
        if (bl >= MINM) { Lmax[i] = (int)bl; Off[i] = (int)bo; }
        prev[i] = head[k];
        head[k] = (int)i;
    }

    int *S = malloc(sizeof(int) * (n + 1));      /* cheapest suffix cost starting with a sequence at j, INF = none */
    int *Sm = malloc(sizeof(int) * (n + 1));     /* chosen literal-run length at j (-1: the final run) */
    int *M = malloc(sizeof(int) * (n + 1));      /* best (match cost + suffix) for a match starting at k */
    int *ML = malloc(sizeof(int) * (n + 1));     /* its match length */
    int *Ufeas = malloc(sizeof(int) * (n + 1));  /* S[e] usable as a match end (in-place condition) */
    long rbest[255]; int rk[255];
    for (int r = 0; r < 255; r++) { rbest[r] = (long)INF * 4; rk[r] = -1; }
    for (long j = n; j >= 0; j--) {
        /* M[j]: a match starting at j */
        int bm = INF, bl = 0;
        int L = Lmax[j];
        /* candidate lengths: every length up to 40, the steps of the 255-byte length extension (19 + 255 t) and the 8 longest (S[e] varies little with e) */
#define TRY(LL) do { long l_ = (LL); long e_ = j + l_; if (Ufeas[e_]) { int v_ = ext(l_ - MINM) + S[e_]; if (v_ < bm || (v_ == bm && l_ > bl)) { bm = v_; bl = (int)l_; } } } while (0)
        for (long l = MINM; l <= L && l <= 40; l++) TRY(l);
        if (L > 40) {
            long tail = L - 7 > 41 ? L - 7 : 41;
            for (long l = 19 + 255 * ((41 - 19 + 254) / 255); l < tail; l += 255) TRY(l);
            for (long l = tail; l <= L; l++) TRY(l);
        }
#undef TRY
        M[j] = bm; ML[j] = bl;
        /* S[j]: the final literal run, or a literal run of m bytes (token + length extension + m + offset) and a match at k = j + m.  m < 15: direct.  m >= 15: the extension
           cost 1 + floor((m - 15) / 255) is separated over residue classes of (k - 15) modulo 255: with a = k - 15 = 255 q_k + r_k and j = 255 q_j + r_j,
           floor((k - j - 15) / 255) = q_k - q_j - [r_k < r_j], so one running minimum per residue class gives the exact optimum over every longer run in 255 steps. */
        int best = 1 + ext(n - j) + (int)(n - j), bmlit = -1;           /* the final literal run */
        for (long m = 0; m < 15 && j + m <= n; m++) {
            if (M[j + m] >= INF) continue;
            int v = 1 + (int)m + 2 + M[j + m];
            if (v < best) { best = v; bmlit = (int)m; }
        }
        if (j + 15 <= n && M[j + 15] < INF) {                            /* k = j + 15 becomes eligible for the long runs */
            long k = j + 15, a = k - 15;
            int r = (int)(a % 255);
            long A = (long)M[k] + k + a / 255;
            if (A <= rbest[r]) { rbest[r] = A; rk[r] = (int)k; }         /* equal: the smaller k (shorter literal run) */
        }
        {
            long qj = j / 255; int rj = (int)(j % 255);
            for (int r = 0; r < 255; r++) {
                if (rbest[r] >= (long)INF * 4) continue;
                long v = 4 - j - qj + rbest[r] - (r < rj ? 1 : 0);
                if (v < best) { best = (int)v; bmlit = rk[r] - (int)j; }
            }
        }
        S[j] = best; Sm[j] = bmlit;
        Ufeas[j] = (cap == 0) || ((long)best <= cap - j);
    }
    if (cap > 0 && S[0] > cap) { fprintf(stderr, "no in-place stream within cap %ld (best %d)\n", cap, S[0]); return 3; }

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 2; }
    long out = 0, j = 0;
#define PUT(b) do { fputc((b), o); out++; } while (0)
#define EXT(v) do { long vv = (v); while (vv >= 255) { PUT(255); vv -= 255; } PUT((int)vv); } while (0)
    for (;;) {
        int m = Sm[j];
        if (m < 0) {
            long lit = n - j;
            PUT((lit > 15 ? 15 : lit) << 4);
            if (lit >= 15) EXT(lit - 15);
            for (long i = j; i < n; i++) PUT(d[i]);
            break;
        }
        long k = j + m;
        int l = ML[k], ml = l - MINM;
        PUT(((m > 15 ? 15 : m) << 4) | (ml > 15 ? 15 : ml));
        if (m >= 15) EXT(m - 15);
        for (long i = j; i < k; i++) PUT(d[i]);
        PUT(Off[k] & 255); PUT(Off[k] >> 8);
        if (ml >= 15) EXT(ml - 15);
        j = k + l;
    }
    fclose(o);
    if (out != S[0]) { fprintf(stderr, "internal: %ld != %d\n", out, S[0]); return 4; }
    printf("%ld\n", out);
    return 0;
}
