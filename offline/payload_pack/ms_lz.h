/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader)
 * Freestanding C89 MSLZ decoder (no libc, no malloc, no floating point, no globals).  Format: tools/payload_pack/mslz.py. */
#ifndef MS_LZ_H
#define MS_LZ_H
typedef unsigned char ms_u8;
typedef unsigned long ms_u32;           /* at least 32 bits; used for sizes only */

#define MS_LZ_OK        0
#define MS_LZ_TRUNC    (-1)
#define MS_LZ_BADOFF   (-2)
#define MS_LZ_OVERFLOW (-3)

/* Decode src[0..n) into dst[0..cap); on success *out_len = bytes written.  Never reads outside src or writes outside dst. */
int ms_lz_decode(const ms_u8 *src, ms_u32 n, ms_u8 *dst, ms_u32 cap, ms_u32 *out_len);
#endif
