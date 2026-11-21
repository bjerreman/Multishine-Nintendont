/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) - freestanding C89 SHA-256 */
#ifndef MS_SHA256_H
#define MS_SHA256_H
#include "ms_lz.h"
void ms_sha256(const ms_u8 *data, ms_u32 n, ms_u8 out[32]);
/* Incremental form (added for the OTA download, which hashes the chunks in order as they arrive); the one-shot function above is unchanged.
   The context is not wiped by final (its one user hashes public data). */
typedef struct ms_sha256_ctx { ms_u32 h[8]; ms_u32 len; ms_u8 buf[64]; ms_u32 fill; } ms_sha256_ctx;      /* len = total bytes so far, below 2^32 */
void ms_sha256_init(ms_sha256_ctx *c);
void ms_sha256_update(ms_sha256_ctx *c, const ms_u8 *data, ms_u32 n);
void ms_sha256_final(ms_sha256_ctx *c, ms_u8 out[32]);
#endif
