/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#ifndef SDC25519_H
#define SDC25519_H
/* sdc25519: X25519 (RFC 7748) and Ed25519 signature verification (RFC 8032) for the console TLS client, over a 32-bit-friendly
   10-limb radix 2^25.5 field (32x32->64 multiplies, the shape the Gekko's mullw/mulhw pair does well).  Roughly 6x fewer cycles
   than the TweetNaCl-style 16-limb code in client_net/mw2_crypto.c at the same size class.  Strict C89, no libc, no heap.
   X25519 is constant time; Ed25519 verification handles only public data (variable time is fine).  Uses mw2_sha512 for the hash. */
typedef unsigned char sdc25_u8;
typedef unsigned int sdc25_u32;

void sdc_x25519(sdc25_u8 out[32], const sdc25_u8 scalar[32], const sdc25_u8 point[32]);
/* msg length <= SDC_ED_MSG_MAX.  Returns 1 for a valid signature under the 32-byte public key, 0 otherwise. */
#define SDC_ED_MSG_MAX 256
int sdc_ed25519_verify(const sdc25_u8 pub[32], const sdc25_u8 *msg, sdc25_u32 n, const sdc25_u8 sig[64]);
#endif
