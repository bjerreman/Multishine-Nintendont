/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#ifndef MW2_CRYPTO_H
#define MW2_CRYPTO_H
/* Freestanding C89 crypto for wire v2: HMAC-SHA256 (over offline/payload_pack/ms_sha256.c),
   SHA-512 and Ed25519 verification (RFC 8032, TweetNaCl-style 16-limb arithmetic, plus the S < L check).  No libc, no heap. */
typedef unsigned char mw2_u8;
typedef unsigned long mw2_u32;          /* at least 32 bits; sizes only */

/* HMAC-SHA256 with a 32-byte key over msg[0..n), n <= MW2_HMAC_MAX */
#define MW2_HMAC_MAX 600
void mw2_hmac(const mw2_u8 key[32], const mw2_u8 *msg, mw2_u32 n, mw2_u8 out[32]);
void mw2_sha512(const mw2_u8 *msg, mw2_u32 n, mw2_u8 out[64]);
/* 1 = valid signature of msg (n <= MW2_VERIFY_MAX) under the 32-byte public key, 0 otherwise */
#define MW2_VERIFY_MAX 600
int mw2_ed25519_verify(const mw2_u8 pub[32], const mw2_u8 *msg, mw2_u32 n, const mw2_u8 sig[64]);
int mw2_ct_equal(const mw2_u8 *a, const mw2_u8 *b, mw2_u32 n);
#ifdef MW2_WITH_SIGN
/* Ed25519 signing, compiled only with -DMW2_WITH_SIGN (device key).  seed = the 32-byte private key. */
#define MW2_SIGN_MAX 536
void mw2_ed25519_pubkey(mw2_u8 pub[32], const mw2_u8 seed[32]);
int mw2_ed25519_sign(mw2_u8 sig[64], const mw2_u8 *msg, mw2_u32 n, const mw2_u8 seed[32]);   /* 1, or 0 if n > MW2_SIGN_MAX */
#endif
#endif
