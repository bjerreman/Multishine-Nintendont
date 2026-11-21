/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#ifndef SDCRYPTO_H
#define SDCRYPTO_H
/* sdcrypto: the symmetric half of the console TLS 1.3 client: SHA-256 (incremental, copyable for the
   transcript), HMAC-SHA256, HKDF-Expand-Label, ChaCha20-Poly1305 (RFC 8439), and a ChaCha20 fast-key-erasure RNG with an explicit
   entropy account.  Strict C89 (the 64-bit typedef carries __extension__), no libc, no heap.  Written from FIPS 180-4, RFC 2104,
   RFC 5869, RFC 8439, RFC 8446 section 7.1; checked against OpenSSL/hashlib/cryptography in tests/test_sdcrypto.py. */
typedef unsigned char sdc_u8;
typedef unsigned int sdc_u32;

struct sdc_sha {
    sdc_u32 h[8];
    sdc_u32 n;                  /* bytes hashed (messages < 4 GB) */
    sdc_u8 buf[64];
};
void sdc_sha_init(struct sdc_sha *c);
void sdc_sha_update(struct sdc_sha *c, const sdc_u8 *p, sdc_u32 n);
void sdc_sha_final(struct sdc_sha *c, sdc_u8 out[32]);      /* destroys c; copy the struct first to keep hashing */
void sdc_sha256(const sdc_u8 *p, sdc_u32 n, sdc_u8 out[32]);

void sdc_hmac(const sdc_u8 *key, sdc_u32 klen, const sdc_u8 *msg, sdc_u32 mlen, sdc_u8 out[32]);
/* HKDF-Extract = HMAC(salt, ikm).  Expand-Label per RFC 8446 7.1: HKDF-Expand(secret, HkdfLabel{len, "tls13 "+label, ctx}, len), len <= 32 */
void sdc_hkdf_extract(const sdc_u8 salt[32], const sdc_u8 *ikm, sdc_u32 ilen, sdc_u8 out[32]);
void sdc_expand_label(const sdc_u8 secret[32], const char *label, const sdc_u8 *ctx, sdc_u32 clen, sdc_u32 len, sdc_u8 *out);

void sdc_chacha20_xor(const sdc_u8 key[32], sdc_u32 counter, const sdc_u8 nonce[12], const sdc_u8 *in, sdc_u8 *out, sdc_u32 n);
/* AEAD: out = ciphertext (n bytes) then 16-byte tag.  Open returns 1 and writes n bytes of plaintext, 0 on tag mismatch (nothing written). */
void sdc_aead_seal(const sdc_u8 key[32], const sdc_u8 nonce[12], const sdc_u8 *aad, sdc_u32 alen, const sdc_u8 *in, sdc_u32 n, sdc_u8 *out);
int sdc_aead_open(const sdc_u8 key[32], const sdc_u8 nonce[12], const sdc_u8 *aad, sdc_u32 alen, const sdc_u8 *in, sdc_u32 n, sdc_u8 *out);

int sdc_ct_eq(const sdc_u8 *a, const sdc_u8 *b, sdc_u32 n);
void sdc_wipe(void *p, sdc_u32 n);

/* ---- RNG: ChaCha20 fast key erasure (new key from the first 32 bytes of every block request) over a SHA-256 pool ----
   The account is deliberately conservative: sdc_rng_mix credits only the bits the caller claims, and sdc_rng_ready is true only when
   the credited total reaches 128.  See  for what the console can honestly credit. */
struct sdc_rng {
    sdc_u8 key[32];
    sdc_u32 ctr;
    struct sdc_sha pool;
    sdc_u32 credit;             /* bits */
    sdc_u32 seeded;
};
void sdc_rng_init(struct sdc_rng *r);
void sdc_rng_mix(struct sdc_rng *r, const sdc_u8 *p, sdc_u32 n, sdc_u32 credit_bits);
void sdc_rng_mix_u32(struct sdc_rng *r, sdc_u32 v, sdc_u32 credit_bits);
int sdc_rng_ready(const struct sdc_rng *r);                 /* credited >= 128 bits */
void sdc_rng_bytes(struct sdc_rng *r, sdc_u8 *out, sdc_u32 n);   /* folds the pool into the key first; works (with weak output) even if not ready: callers must check ready */
#endif
