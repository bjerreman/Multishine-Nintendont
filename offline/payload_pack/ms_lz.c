/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
#include "ms_lz.h"

int ms_lz_decode(const ms_u8 *src, ms_u32 n, ms_u8 *dst, ms_u32 cap, ms_u32 *out_len)
{
    ms_u32 ip = 0, op = 0, len, i, off;
    ms_u8 tok, b;
    for (;;) {
        if (ip >= n) return MS_LZ_TRUNC;
        tok = src[ip++];
        len = tok >> 4;
        if (len == 15) {
            do {
                if (ip >= n) return MS_LZ_TRUNC;
                b = src[ip++];
                len += b;
                if (len > cap) return MS_LZ_OVERFLOW;   /* also stops 32-bit wrap */
            } while (b == 255);
        }
        if (len > n - ip) return MS_LZ_TRUNC;
        if (len > cap - op) return MS_LZ_OVERFLOW;
        for (i = 0; i < len; i++) dst[op + i] = src[ip + i];
        ip += len; op += len;
        if (ip == n) { *out_len = op; return MS_LZ_OK; }
        if (n - ip < 2) return MS_LZ_TRUNC;
        off = (ms_u32)src[ip] | ((ms_u32)src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return MS_LZ_BADOFF;
        len = tok & 15;
        if (len == 15) {
            do {
                if (ip >= n) return MS_LZ_TRUNC;
                b = src[ip++];
                len += b;
                if (len > cap) return MS_LZ_OVERFLOW;
            } while (b == 255);
        }
        len += 4;
        if (len > cap - op) return MS_LZ_OVERFLOW;
        for (i = 0; i < len; i++) dst[op + i] = dst[op + i - off];
        op += len;
    }
}
