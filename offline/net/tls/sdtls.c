/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* sdtls: TLS 1.3 client, see sdtls.h.  Strict C89. */
#include "sdtls.h"
#include "sdc25519.h"

#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_NEW_TICKET 4
#define HS_ENC_EXT 8
#define HS_CERT 11
#define HS_CERT_VERIFY 15
#define HS_FINISHED 20
#define HS_KEY_UPDATE 24

#define CT_CCS 20
#define CT_ALERT 21
#define CT_HS 22
#define CT_APP 23

enum { HEAVY_NONE = 0, HEAVY_X25519 = 1, HEAVY_VERIFY = 2 };

static const sdc_u8 BASE9[32] = { 9 };
static const sdc_u8 SPKI_ED25519[12] = { 0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00 };

static sdc_u32 rd16(const sdc_u8 *p) { return ((sdc_u32)p[0] << 8) | p[1]; }
static sdc_u32 rd24(const sdc_u8 *p) { return ((sdc_u32)p[0] << 16) | ((sdc_u32)p[1] << 8) | p[2]; }
static void wr16(sdc_u8 *p, sdc_u32 v) { p[0] = (sdc_u8)(v >> 8); p[1] = (sdc_u8)v; }
static void wr24(sdc_u8 *p, sdc_u32 v) { p[0] = (sdc_u8)(v >> 16); p[1] = (sdc_u8)(v >> 8); p[2] = (sdc_u8)v; }
static void mcpy(sdc_u8 *d, const sdc_u8 *s, sdc_u32 n) { sdc_u32 i; for (i = 0; i < n; i++) d[i] = s[i]; }
static void mzero(sdc_u8 *d, sdc_u32 n) { sdc_u32 i; for (i = 0; i < n; i++) d[i] = 0; }

static int fail(struct sdtls *t, int err)
{
    if (t->state != SDTLS_FAILED) { t->err = (sdc_u8)err; t->state = SDTLS_FAILED; }
    sdc_wipe(t->priv, 32);
    return SDTLS_FAILED;
}

/* ---- transcript ---- */
static void th_add(struct sdtls *t, const sdc_u8 *msg, sdc_u32 n) { sdc_sha_update(&t->th, msg, n); }
static void th_now(const struct sdtls *t, sdc_u8 out[32])
{
    struct sdc_sha c = t->th;
    sdc_sha_final(&c, out);
}

/* ---- keys ---- */
static void derive_keys(const sdc_u8 secret[32], sdc_u8 key[32], sdc_u8 iv[12])
{
    sdc_expand_label(secret, "key", 0, 0, 32, key);
    sdc_expand_label(secret, "iv", 0, 0, 12, iv);
}
static void make_nonce(const sdc_u8 iv[12], const sdc_u32 seq[2], sdc_u8 n[12])
{
    sdc_u32 i;
    for (i = 0; i < 12; i++) n[i] = iv[i];
    for (i = 0; i < 4; i++) { n[11 - i] = (sdc_u8)(n[11 - i] ^ (sdc_u8)(seq[0] >> (8 * i))); n[7 - i] = (sdc_u8)(n[7 - i] ^ (sdc_u8)(seq[1] >> (8 * i))); }
}
static void seq_inc(sdc_u32 s[2]) { if (++s[0] == 0) s[1]++; }

/* ---- output records ---- */
static int flush(struct sdtls *t)
{
    int n;
    while (t->out_off < t->out_len) {
        n = t->io.send(t->io.ctx, t->out + t->out_off, t->out_len - t->out_off);
        if (n < 0) return -1;
        if (n == 0) return 0;
        t->out_off += (sdc_u32)n;
    }
    t->out_len = t->out_off = 0;
    return 1;
}
/* Build one record in out[] (must be empty).  type: inner content type; encrypted with the client write keys when enc_out. */
static int queue_record(struct sdtls *t, sdc_u8 type, const sdc_u8 *body, sdc_u32 n)
{
    sdc_u8 nonce[12];
    if (t->out_len) return 0;
    if (n > SDTLS_MAX_PLAIN) return 0;
    if (!t->enc_out) {
        t->out[0] = type; t->out[1] = 3; t->out[2] = 1; wr16(t->out + 3, n);
        mcpy(t->out + 5, body, n);
        t->out_len = 5 + n;
    } else {
        t->out[0] = CT_APP; t->out[1] = 3; t->out[2] = 3; wr16(t->out + 3, n + 1 + 16);
        mcpy(t->out + 5, body, n);
        t->out[5 + n] = type;
        make_nonce(t->c_iv, t->c_seq, nonce);
        sdc_aead_seal(t->c_key, nonce, t->out, 5, t->out + 5, n + 1, t->out + 5);
        seq_inc(t->c_seq);
        t->out_len = 5 + n + 1 + 16;
    }
    t->out_off = 0;
    return 1;
}

/* ---- begin / ClientHello ---- */
int sdtls_begin(struct sdtls *t, const struct sdtls_io *io, sdtls_rand_fn rnd, void *rand_ctx, const sdc_u8 *pins, sdc_u32 npins, const char *sni)
{
    sdc_u8 *b = (sdc_u8 *)t;
    sdc_u32 i;
    for (i = 0; i < sizeof(*t); i++) b[i] = 0;
    t->io = *io;
    t->rand = rnd;
    t->rand_ctx = rand_ctx;
    t->pins = pins;
    t->npins = npins > 4 ? 4 : npins;
    t->sni = sni;
    sdc_sha_init(&t->th);
    t->in = t->in_small; t->in_cap = SDTLS_IN_BUF; t->max_in = SDTLS_MAX_PLAIN;
    t->state = SDTLS_START;
    return 0;
}

#ifdef SDTLS_WEBPKI
int sdtls_begin_web(struct sdtls *t, const struct sdtls_io *io, sdtls_rand_fn rnd, void *rand_ctx, struct sdtls_web *web, const char *host)
{
    if (!web || !host || !web->rx || web->rx_cap < SDTLS_WEB_RX) return -1;
    sdtls_begin(t, io, rnd, rand_ctx, 0, 0, host);
    web->cert_err = 0;
    t->web = web;
    t->in = web->rx; t->in_cap = web->rx_cap; t->max_in = web->rx_cap - 5 - 256;
    if (t->max_in > 16384) t->max_in = 16384;
    return 0;
}

#endif
static int build_client_hello(struct sdtls *t, const sdc_u8 pub[32])
{
    sdc_u8 m[256];
    sdc_u32 n = 0, sni_len = 0, ext_at, i;

    if (t->sni) { while (t->sni[sni_len]) sni_len++; if (sni_len > 80) return 0; }
    m[n++] = HS_CLIENT_HELLO; n += 3;                          /* length patched below */
    m[n++] = 3; m[n++] = 3;                                    /* legacy_version */
    if (t->rand(t->rand_ctx, m + n, 32)) return -1;
    n += 32;
    m[n++] = 0;                                                /* legacy_session_id: empty */
    m[n++] = 0; m[n++] = 2; m[n++] = 0x13; m[n++] = 0x03;      /* TLS_CHACHA20_POLY1305_SHA256 */
    m[n++] = 1; m[n++] = 0;                                    /* null compression */
    ext_at = n; n += 2;
    m[n++] = 0; m[n++] = 0x2b; m[n++] = 0; m[n++] = 3; m[n++] = 2; m[n++] = 3; m[n++] = 4;                    /* supported_versions: 1.3 */
    m[n++] = 0; m[n++] = 0x0a; m[n++] = 0; m[n++] = 4; m[n++] = 0; m[n++] = 2; m[n++] = 0; m[n++] = 0x1d;     /* supported_groups: x25519 */
#ifdef SDTLS_WEBPKI
    if (t->web) {                                              /* signature_algorithms: rsa_pss_rsae_sha256/384, ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384 */
        m[n++] = 0; m[n++] = 0x0d; m[n++] = 0; m[n++] = 10; m[n++] = 0; m[n++] = 8;
        m[n++] = 8; m[n++] = 4; m[n++] = 8; m[n++] = 5; m[n++] = 4; m[n++] = 3; m[n++] = 5; m[n++] = 3;
    } else
#endif
    {
    m[n++] = 0; m[n++] = 0x0d; m[n++] = 0; m[n++] = 4; m[n++] = 0; m[n++] = 2; m[n++] = 8; m[n++] = 7;        /* signature_algorithms: ed25519 */
    }
    m[n++] = 0; m[n++] = 0x33; m[n++] = 0; m[n++] = 38; m[n++] = 0; m[n++] = 36; m[n++] = 0; m[n++] = 0x1d; m[n++] = 0; m[n++] = 32;
    mcpy(m + n, pub, 32); n += 32;                             /* key_share */
    if (!t->web) { m[n++] = 0; m[n++] = 0x01; m[n++] = 0; m[n++] = 1; m[n++] = 3; }                          /* max_fragment_length: 2^11 (pinned mode only: public servers ignore it) */
    if (t->sni) {
        m[n++] = 0; m[n++] = 0; wr16(m + n, sni_len + 5); n += 2; wr16(m + n, sni_len + 3); n += 2;
        m[n++] = 0; wr16(m + n, sni_len); n += 2;
        for (i = 0; i < sni_len; i++) m[n++] = (sdc_u8)t->sni[i];
    }
    wr16(m + ext_at, n - ext_at - 2);
    wr24(m + 1, n - 4);
    th_add(t, m, n);
    if (!queue_record(t, CT_HS, m, n)) return 0;
    return 1;
}

/* ---- heavy operations ---- */
static int run_heavy(struct sdtls *t)
{
    sdc_u8 pub[32];
    int r;

    if (t->state == SDTLS_START) {
        if (t->rand(t->rand_ctx, t->priv, 32)) return fail(t, SDTLS_E_RNG);
        sdc_x25519(pub, t->priv, BASE9);
        t->stat_heavy[0]++;
        r = build_client_hello(t, pub);
        if (r < 0) return fail(t, SDTLS_E_RNG);
        if (r == 0) return fail(t, SDTLS_E_OVERFLOW);
        t->state = SDTLS_SEND_CH;
    } else if (t->state == SDTLS_WAIT_ECDH) {
        sdc_u8 shared[32], early[32], derived[32], zero[32], h0[32];
        sdc_u32 i;
        sdc_x25519(shared, t->priv, t->srv_pub);
        t->stat_heavy[0]++;
        sdc_wipe(t->priv, 32);
        {
            sdc_u8 acc = 0;
            for (i = 0; i < 32; i++) acc = (sdc_u8)(acc | shared[i]);
            if (acc == 0) return fail(t, SDTLS_E_SECRET);
        }
        mzero(zero, 32);
        sdc_hkdf_extract(zero, zero, 32, early);
        sdc_sha256(0, 0, h0);
        sdc_expand_label(early, "derived", h0, 32, 32, derived);
        sdc_hkdf_extract(derived, shared, 32, t->hs);
        sdc_expand_label(t->hs, "c hs traffic", t->sh_hash, 32, 32, t->c_hs);
        sdc_expand_label(t->hs, "s hs traffic", t->sh_hash, 32, 32, t->s_hs);
        derive_keys(t->c_hs, t->c_key, t->c_iv);
        derive_keys(t->s_hs, t->s_key, t->s_iv);
        t->c_seq[0] = t->c_seq[1] = t->s_seq[0] = t->s_seq[1] = 0;
        t->enc_in = 1;
        sdc_wipe(shared, 32); sdc_wipe(early, 32); sdc_wipe(derived, 32);
        t->state = SDTLS_WAIT_EE;
#ifdef SDTLS_WEBPKI
    } else if (t->state == SDTLS_WAIT_CHAIN) {
        r = sdx_chain_step(&t->web->chain, &t->web->ws);
        t->stat_heavy[1]++;
        if (r == SDX_E_SIG) { t->web->cert_err = r; return fail(t, SDTLS_E_UNTRUSTED); }
        if (r < 0) { t->web->cert_err = r; return fail(t, SDTLS_E_CERT); }
        if (r == 1) t->state = SDTLS_WAIT_CV;
    } else if (t->state == SDTLS_WAIT_VERIFY && t->web) {
        sdc_u8 msg[64 + 34 + 32], dg[48];
        sdc_u32 i, hl, sch = t->web->scheme;
        struct sdtls_web *w = t->web;
        int ok = 0;
        for (i = 0; i < 64; i++) msg[i] = 0x20;
        mcpy(msg + 64, (const sdc_u8 *)"TLS 1.3, server CertificateVerify", 33);
        msg[97] = 0;
        mcpy(msg + 98, t->cvhash, 32);
        t->stat_heavy[1]++;
        if (sch == 0x0805 || sch == 0x0503) { sdpk_sha384(msg, 130, dg); hl = 48; } else { sdc_sha256(msg, 130, dg); hl = 32; }
        if (sch == 0x0804 || sch == 0x0805)
            ok = sdpk_rsa_pss_verify(&w->ws, w->leaf_n, w->leaf_n_len, w->leaf_e, w->leaf_e_len, sch == 0x0804 ? SDPK_HASH_SHA256 : SDPK_HASH_SHA384, dg, w->sig, w->sig_len);
        else if (sch == 0x0403)
            ok = sdpk_ecdsa_verify(&w->ws, SDPK_CURVE_P256, w->leaf_pt, 65, dg, hl, w->sig, w->sig_len);
        else if (sch == 0x0503)
            ok = sdpk_ecdsa_verify(&w->ws, SDPK_CURVE_P384, w->leaf_pt, 97, dg, hl, w->sig, w->sig_len);
        if (!ok) return fail(t, SDTLS_E_SIG);
        t->state = SDTLS_WAIT_FIN;
#endif
    } else if (t->state == SDTLS_WAIT_VERIFY) {
        sdc_u8 msg[64 + 34 + 32];
        sdc_u32 i;
        for (i = 0; i < 64; i++) msg[i] = 0x20;
        mcpy(msg + 64, (const sdc_u8 *)"TLS 1.3, server CertificateVerify", 33);
        msg[97] = 0;
        mcpy(msg + 98, t->cvhash, 32);
        t->stat_heavy[1]++;
        if (!sdc_ed25519_verify(t->pinned, msg, 130, t->sig)) return fail(t, SDTLS_E_SIG);
        t->state = SDTLS_WAIT_FIN;
    }
    return t->state;
}

int sdtls_heavy_next(const struct sdtls *t)
{
    return t->state == SDTLS_START || t->state == SDTLS_WAIT_ECDH || t->state == SDTLS_WAIT_VERIFY || t->state == SDTLS_WAIT_CHAIN;
}

/* ---- handshake message handlers (body = message after the 4-byte header) ---- */
static int on_server_hello(struct sdtls *t, const sdc_u8 *msg, sdc_u32 mlen)
{
    const sdc_u8 *b = msg + 4;
    sdc_u32 n = mlen - 4, p, sidl, extl, got_ks = 0, got_ver = 0;
    static const sdc_u8 hrr[32] = { 0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
                                    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };
    if (n < 2 + 32 + 1) return fail(t, SDTLS_E_PROTO);
    if (rd16(b) != 0x0303) return fail(t, SDTLS_E_PROTO);
    if (sdc_ct_eq(b + 2, hrr, 32)) return fail(t, SDTLS_E_UNSUPPORTED);        /* HelloRetryRequest: we only offer x25519 and send its share */
    p = 34; sidl = b[p++];
    if (sidl != 0 || p + 2 + 1 + 2 > n) return fail(t, SDTLS_E_PROTO);
    if (rd16(b + p) != 0x1303) return fail(t, SDTLS_E_UNSUPPORTED);
    p += 2;
    if (b[p++] != 0) return fail(t, SDTLS_E_PROTO);
    extl = rd16(b + p); p += 2;
    if (p + extl != n) return fail(t, SDTLS_E_PROTO);
    while (p + 4 <= n) {
        sdc_u32 et = rd16(b + p), el = rd16(b + p + 2);
        p += 4;
        if (p + el > n) return fail(t, SDTLS_E_PROTO);
        if (et == 0x002b) { if (el != 2 || rd16(b + p) != 0x0304) return fail(t, SDTLS_E_UNSUPPORTED); got_ver = 1; }
        else if (et == 0x0033) {
            if (el != 36 || rd16(b + p) != 0x001d || rd16(b + p + 2) != 32) return fail(t, SDTLS_E_UNSUPPORTED);
            mcpy(t->srv_pub, b + p + 4, 32); got_ks = 1;
        } else return fail(t, SDTLS_E_UNSUPPORTED);                           /* anything we did not ask for */
        p += el;
    }
    if (!got_ks || !got_ver) return fail(t, SDTLS_E_PROTO);
    th_add(t, msg, mlen);
    th_now(t, t->sh_hash);
    t->state = SDTLS_WAIT_ECDH;
    return t->state;
}

#ifdef SDTLS_WEBPKI
static int on_cert_web(struct sdtls *t, const sdc_u8 *msg, sdc_u32 mlen)
{
    const sdc_u8 *b = msg + 4, *ders[SDX_MAX_CHAIN + 1];
    sdc_u32 lens[SDX_MAX_CHAIN + 1];
    sdc_u32 n = mlen - 4, p, ctxl, listl, cl, el, cnt = 0;
    struct sdtls_web *w = t->web;
    const struct sdx_cert *leaf;
    int r;

    if (n < 1 + 3) return fail(t, SDTLS_E_PROTO);
    ctxl = b[0];
    if (ctxl != 0) return fail(t, SDTLS_E_PROTO);
    p = 1;
    listl = rd24(b + p); p += 3;
    if (p + listl != n || listl < 3) return fail(t, SDTLS_E_PROTO);
    while (p < n) {                                       /* CertificateEntry: cert_data<3>, extensions<2> (ignored) */
        if (p + 3 > n) return fail(t, SDTLS_E_PROTO);
        cl = rd24(b + p); p += 3;
        if (cl == 0 || p + cl + 2 > n) return fail(t, SDTLS_E_PROTO);
        if (cnt <= SDX_MAX_CHAIN) { ders[cnt] = b + p; lens[cnt] = cl; cnt++; }
        p += cl;
        el = rd16(b + p); p += 2;
        if (p + el > n) return fail(t, SDTLS_E_PROTO);
        p += el;
    }
    th_add(t, msg, mlen);
    r = sdx_chain_begin(&w->chain, w->anchors, w->nanchors, t->sni, w->now, w->flags, ders, lens, cnt);
    if (r) {
        w->cert_err = r;
        return fail(t, r == SDX_E_UNTRUSTED ? SDTLS_E_UNTRUSTED : r == SDX_E_HOST ? SDTLS_E_HOST : (r == SDX_E_EXPIRED || r == SDX_E_NOTIME) ? SDTLS_E_TIME : SDTLS_E_CERT);
    }
    leaf = &w->chain.c[0];
    w->leaf_type = leaf->key_type;
    if (leaf->key_type == SDX_KEY_RSA) {
        if (leaf->key_len > sizeof w->leaf_n || leaf->key_e_len > sizeof w->leaf_e) { w->cert_err = SDX_E_UNSUPPORTED; return fail(t, SDTLS_E_CERT); }
        mcpy(w->leaf_n, leaf->key, leaf->key_len); w->leaf_n_len = leaf->key_len;
        mcpy(w->leaf_e, leaf->key_e, leaf->key_e_len); w->leaf_e_len = leaf->key_e_len;
    } else if (leaf->key_type == SDX_KEY_EC256 || leaf->key_type == SDX_KEY_EC384) {
        mcpy(w->leaf_pt, leaf->key, leaf->key_len);
    } else { w->cert_err = SDX_E_UNSUPPORTED; return fail(t, SDTLS_E_CERT); }
    t->state = SDTLS_WAIT_CHAIN;
    return t->state;
}

#endif
static int on_cert(struct sdtls *t, const sdc_u8 *msg, sdc_u32 mlen)
{
    const sdc_u8 *b = msg + 4;
    sdc_u32 n = mlen - 4, p, ctxl, listl, cl, i, k, found = 0;

#ifdef SDTLS_WEBPKI
    if (t->web) return on_cert_web(t, msg, mlen);
#endif

    if (n < 1 + 3) return fail(t, SDTLS_E_PROTO);
    ctxl = b[0];
    p = 1 + ctxl;
    if (p + 3 > n) return fail(t, SDTLS_E_PROTO);
    listl = rd24(b + p); p += 3;
    if (p + listl != n || listl < 3) return fail(t, SDTLS_E_PROTO);
    cl = rd24(b + p); p += 3;
    if (p + cl > n || cl < 12 + 32) return fail(t, SDTLS_E_PROTO);
    /* The certificate is only a carrier for the key: find the Ed25519 SPKI and require its key to be a pin.  What the rest of the
       certificate says does not matter, because the signature in CertificateVerify must verify under that pinned key. */
    for (i = 0; i + 12 + 32 <= cl && !found; i++) {
        for (k = 0; k < 12 && b[p + i + k] == SPKI_ED25519[k]; k++) { }
        if (k == 12) {
            sdc_u32 j;
            for (j = 0; j < t->npins; j++) {
                if (sdc_ct_eq(t->pins + 32 * j, b + p + i + 12, 32)) { mcpy(t->pinned, t->pins + 32 * j, 32); found = 1; break; }
            }
        }
    }
    if (!found) return fail(t, SDTLS_E_PIN);
    th_add(t, msg, mlen);
    t->state = SDTLS_WAIT_CV;
    return t->state;
}

#ifdef SDTLS_WEBPKI
static int on_cert_verify_web(struct sdtls *t, const sdc_u8 *msg, sdc_u32 mlen)
{
    const sdc_u8 *b = msg + 4;
    struct sdtls_web *w = t->web;
    sdc_u32 sch, sl;
    if (mlen < 4 + 4) return fail(t, SDTLS_E_PROTO);
    sch = rd16(b); sl = rd16(b + 2);
    if (mlen - 8 != sl || sl == 0 || sl > sizeof w->sig) return fail(t, SDTLS_E_PROTO);
    if (!(((sch == 0x0804 || sch == 0x0805) && w->leaf_type == SDX_KEY_RSA) || (sch == 0x0403 && w->leaf_type == SDX_KEY_EC256) ||
          (sch == 0x0503 && w->leaf_type == SDX_KEY_EC384))) return fail(t, SDTLS_E_UNSUPPORTED);
    th_now(t, t->cvhash);                                  /* hash through Certificate */
    mcpy(w->sig, b + 4, sl); w->sig_len = sl; w->scheme = sch;
    th_add(t, msg, mlen);
    t->state = SDTLS_WAIT_VERIFY;
    return t->state;
}

#endif
static int on_cert_verify(struct sdtls *t, const sdc_u8 *msg, sdc_u32 mlen)
{
    const sdc_u8 *b = msg + 4;
#ifdef SDTLS_WEBPKI
    if (t->web) return on_cert_verify_web(t, msg, mlen);
#endif
    if (mlen - 4 != 2 + 2 + 64 || rd16(b) != 0x0807 || rd16(b + 2) != 64) return fail(t, SDTLS_E_PROTO);
    th_now(t, t->cvhash);                                  /* hash through Certificate */
    mcpy(t->sig, b + 4, 64);
    th_add(t, msg, mlen);
    t->state = SDTLS_WAIT_VERIFY;
    return t->state;
}

static int on_finished(struct sdtls *t, const sdc_u8 *msg, sdc_u32 mlen)
{
    sdc_u8 fk[32], want[32], th_cv[32], th_fin[32], derived[32], zero[32], h0[32], master[32], cf[4 + 32], ck[32];
    if (mlen != 4 + 32) return fail(t, SDTLS_E_PROTO);
    th_now(t, th_cv);                                      /* through CertificateVerify */
    sdc_expand_label(t->s_hs, "finished", 0, 0, 32, fk);
    sdc_hmac(fk, 32, th_cv, 32, want);
    if (!sdc_ct_eq(want, msg + 4, 32)) return fail(t, SDTLS_E_FINISHED);
    th_add(t, msg, mlen);
    th_now(t, th_fin);                                     /* through server Finished */
    mzero(zero, 32);
    sdc_sha256(0, 0, h0);
    sdc_expand_label(t->hs, "derived", h0, 32, 32, derived);
    sdc_hkdf_extract(derived, zero, 32, master);
    sdc_expand_label(master, "c ap traffic", th_fin, 32, 32, t->c_secret);
    sdc_expand_label(master, "s ap traffic", th_fin, 32, 32, t->s_secret);
    /* client Finished goes out under the client handshake keys */
    sdc_expand_label(t->c_hs, "finished", 0, 0, 32, ck);
    cf[0] = HS_FINISHED; cf[1] = 0; cf[2] = 0; cf[3] = 32;
    sdc_hmac(ck, 32, th_fin, 32, cf + 4);
    t->enc_out = 1;
    if (!queue_record(t, CT_HS, cf, 36)) return fail(t, SDTLS_E_OVERFLOW);
    /* switch to application keys */
    derive_keys(t->c_secret, t->c_key, t->c_iv);
    derive_keys(t->s_secret, t->s_key, t->s_iv);
    t->c_seq[0] = t->c_seq[1] = t->s_seq[0] = t->s_seq[1] = 0;
    /* the queued Finished was sealed before the switch above only if queue_record ran first: it did (seal happens in queue_record) */
    sdc_wipe(fk, 32); sdc_wipe(master, 32); sdc_wipe(ck, 32); sdc_wipe(t->hs, 32); sdc_wipe(t->c_hs, 32); sdc_wipe(t->s_hs, 32);
    t->state = SDTLS_CONNECTED;
    return t->state;
}

static int key_update_recv(struct sdtls *t, sdc_u32 request)
{
    sdc_u8 ns[32];
    sdc_expand_label(t->s_secret, "traffic upd", 0, 0, 32, ns);
    mcpy(t->s_secret, ns, 32);
    derive_keys(t->s_secret, t->s_key, t->s_iv);
    t->s_seq[0] = t->s_seq[1] = 0;
    sdc_wipe(ns, 32);
    if (request == 1) t->ku_pending = 1;
    return 1;
}

static int hs_dispatch(struct sdtls *t, sdc_u8 type, const sdc_u8 *msg, sdc_u32 mlen)
{
    switch (t->state) {
    case SDTLS_WAIT_SH: return type == HS_SERVER_HELLO ? on_server_hello(t, msg, mlen) : fail(t, SDTLS_E_PROTO);
    case SDTLS_WAIT_EE:
        if (type != HS_ENC_EXT) return fail(t, SDTLS_E_PROTO);
        th_add(t, msg, mlen);
        t->state = SDTLS_WAIT_CERT;
        return t->state;
    case SDTLS_WAIT_CERT: return type == HS_CERT ? on_cert(t, msg, mlen) : fail(t, SDTLS_E_PROTO);
    case SDTLS_WAIT_CV: return type == HS_CERT_VERIFY ? on_cert_verify(t, msg, mlen) : fail(t, SDTLS_E_PROTO);
    case SDTLS_WAIT_FIN: return type == HS_FINISHED ? on_finished(t, msg, mlen) : fail(t, SDTLS_E_PROTO);
    case SDTLS_CONNECTED:
        if (type == HS_NEW_TICKET) return t->state;                 /* resumption is not implemented yet: ignore */
        if (type == HS_KEY_UPDATE) {
            if (mlen != 5 || msg[4] > 1) return fail(t, SDTLS_E_PROTO);
            key_update_recv(t, msg[4]);
            return t->state;
        }
        return fail(t, SDTLS_E_PROTO);
    default: return fail(t, SDTLS_E_PROTO);
    }
}

/* Process queued handshake messages in the current record until done, failed, or a heavy operation is due. */
static void hs_run(struct sdtls *t)
{
    while (t->state != SDTLS_FAILED && t->hs_off < t->pt_len && !sdtls_heavy_next(t)) {
        const sdc_u8 *p = t->in + t->pt_off + t->hs_off;
        sdc_u32 left = t->pt_len - t->hs_off, ml;
        if (left < 4) { fail(t, SDTLS_E_PROTO); return; }
        ml = rd24(p + 1);
        if (4 + ml > left) { fail(t, SDTLS_E_OVERFLOW); return; }     /* fragmented handshake message */
        t->hs_off += 4 + ml;
        hs_dispatch(t, p[0], p, 4 + ml);
    }
    if (t->hs_off >= t->pt_len && !sdtls_heavy_next(t)) { t->in_ready = 0; t->in_len = 0; t->hs_off = 0; }
}

/* ---- record input ---- */
/* returns 1 when a full record is in in[], 0 when more bytes are needed, -1 on transport failure */
static int fill_record(struct sdtls *t)
{
    sdc_u32 need;
    int n;
    for (;;) {
        if (t->in_len >= 5) {
            need = 5 + rd16(t->in + 3);
            if (need > t->in_cap) return -2;
            if (t->in_len >= need) return 1;
        } else need = 5;
        n = t->io.recv(t->io.ctx, t->in + t->in_len, need - t->in_len);
        if (n < 0) return -1;
        if (n == 0) return 0;
        t->in_len += (sdc_u32)n;
    }
}

/* Decode the record in in[]; sets pt_*.  Returns 0 ok (record consumed or ready), nonzero = failed state set. */
static int open_record(struct sdtls *t)
{
    sdc_u32 clen = rd16(t->in + 3);
    sdc_u8 type = t->in[0];

    t->hs_off = 0;
    if (type == CT_CCS) {
        if (t->enc_in && t->state >= SDTLS_CONNECTED) return fail(t, SDTLS_E_PROTO);
        if (clen != 1 || t->in[5] != 1) return fail(t, SDTLS_E_PROTO);
        t->in_len = 0;
        return 0;
    }
    if (!t->enc_in) {
        if (type != CT_HS && type != CT_ALERT) return fail(t, SDTLS_E_PROTO);
        if (clen > t->max_in + 256u) return fail(t, SDTLS_E_OVERFLOW);
        t->pt_type = type; t->pt_off = 5; t->pt_len = clen; t->in_ready = 1;
        return 0;
    }
    if (type != CT_APP || clen < 17 || clen > t->max_in + 256u) return fail(t, SDTLS_E_PROTO);
    {
        sdc_u8 nonce[12];
        sdc_u32 plen = clen - 16;
        make_nonce(t->s_iv, t->s_seq, nonce);
        if (!sdc_aead_open(t->s_key, nonce, t->in, 5, t->in + 5, plen, t->in + 5)) return fail(t, SDTLS_E_MAC);
        seq_inc(t->s_seq);
        while (plen > 0 && t->in[5 + plen - 1] == 0) plen--;
        if (plen == 0) return fail(t, SDTLS_E_PROTO);
        t->pt_type = t->in[5 + plen - 1];
        t->pt_off = 5; t->pt_len = plen - 1; t->in_ready = 1;
        if (t->pt_len > t->max_in) return fail(t, SDTLS_E_OVERFLOW);
    }
    return 0;
}

/* Handle the decoded record when it is not application data. Returns 1 if consumed. */
static void consume_control(struct sdtls *t)
{
    if (t->pt_type == CT_ALERT) {
        if (t->pt_len == 2) {
            t->alert = t->in[t->pt_off + 1];
            if (t->in[t->pt_off + 1] == 0 && t->in[t->pt_off] == 1) { t->state = SDTLS_CLOSED; t->in_ready = 0; t->in_len = 0; return; }
        }
        t->in_ready = 0; t->in_len = 0;
        fail(t, SDTLS_E_ALERT);
        return;
    }
    if (t->pt_type == CT_HS) { hs_run(t); return; }
    if (t->pt_type == CT_APP) {
        if (t->state != SDTLS_CONNECTED) fail(t, SDTLS_E_PROTO);
        return;                                           /* application data stays for sdtls_read */
    }
    fail(t, SDTLS_E_PROTO);
}

static void pump_in(struct sdtls *t)
{
    int r;
    while (t->state != SDTLS_FAILED && t->state != SDTLS_CLOSED && !sdtls_heavy_next(t)) {
        if (!t->in_ready) {
            r = fill_record(t);
            if (r == 0) return;
            if (r == -2) { fail(t, SDTLS_E_OVERFLOW); return; }
            if (r < 0) { if (t->state != SDTLS_CONNECTED) fail(t, SDTLS_E_IO); else t->state = SDTLS_CLOSED; return; }
            if (open_record(t)) return;
            if (!t->in_ready) continue;                   /* CCS dropped */
        }
        if (t->pt_type == CT_APP) return;                 /* leave for the application */
        if (t->state == SDTLS_WAIT_SH && t->pt_type == CT_HS) { /* fallthrough to hs_run */ }
        consume_control(t);
        if (t->in_ready && sdtls_heavy_next(t)) return;
    }
}

int sdtls_poll(struct sdtls *t)
{
    int f;
    if (t->state == SDTLS_IDLE || t->state == SDTLS_FAILED) return t->state;
    f = flush(t);
    if (f < 0) { if (t->state != SDTLS_CONNECTED) return fail(t, SDTLS_E_IO); t->state = SDTLS_CLOSED; return t->state; }
    if (t->ku_pending && f == 1 && t->state == SDTLS_CONNECTED) {
        sdc_u8 ku[5], ns[32];
        ku[0] = HS_KEY_UPDATE; ku[1] = 0; ku[2] = 0; ku[3] = 1; ku[4] = 0;
        if (queue_record(t, CT_HS, ku, 5)) {
            sdc_expand_label(t->c_secret, "traffic upd", 0, 0, 32, ns);
            mcpy(t->c_secret, ns, 32);
            derive_keys(t->c_secret, t->c_key, t->c_iv);
            t->c_seq[0] = t->c_seq[1] = 0;
            sdc_wipe(ns, 32);
            t->ku_pending = 0;
        }
    }
    if (t->state == SDTLS_SEND_CH) {
        if (f == 1) t->state = SDTLS_WAIT_SH;
    }
    if (sdtls_heavy_next(t)) {
        if (!(t->state == SDTLS_WAIT_ECDH || t->state == SDTLS_WAIT_VERIFY || t->state == SDTLS_WAIT_CHAIN || t->state == SDTLS_START) || (t->state == SDTLS_START && t->out_len)) return t->state;
        run_heavy(t);
        if (t->state == SDTLS_WAIT_EE || t->state == SDTLS_WAIT_FIN || t->state == SDTLS_WAIT_CV) hs_run(t);     /* rest of the record that carried the message */
        return t->state;
    }
    if (t->state >= SDTLS_WAIT_SH && t->state <= SDTLS_WAIT_FIN) pump_in(t);
    else if (t->state == SDTLS_CONNECTED && !t->in_ready) {
        /* control records (tickets, key updates, alerts) are processed while the app is not reading */
        if (t->in_len == 0 || t->in_len >= 5) pump_in(t);
    }
    flush(t);
    return t->state;
}

int sdtls_out_pending(const struct sdtls *t) { return t->out_len > t->out_off; }

int sdtls_write(struct sdtls *t, const sdc_u8 *buf, sdc_u32 n)
{
    if (t->state == SDTLS_FAILED) return -1;
    if (t->state != SDTLS_CONNECTED) return t->state == SDTLS_CLOSED ? -1 : 0;
    if (t->out_len) { if (flush(t) < 0) { t->state = SDTLS_CLOSED; return -1; } if (t->out_len) return 0; }
    if (t->ku_pending) return 0;
    if (n > SDTLS_MAX_PLAIN) n = SDTLS_MAX_PLAIN;
    if (n == 0) return 0;
    if (!queue_record(t, CT_APP, buf, n)) return 0;
    if (flush(t) < 0) { t->state = SDTLS_CLOSED; return -1; }
    return (int)n;
}

int sdtls_read(struct sdtls *t, sdc_u8 *buf, sdc_u32 max)
{
    sdc_u32 n, i;
    if (t->state == SDTLS_FAILED) return SDTLS_READ_ERR;
    for (;;) {
        if (t->in_ready && t->pt_type == CT_APP) {
            n = t->pt_len - t->hs_off;                    /* hs_off doubles as the read offset of an application record */
            if (n > max) n = max;
            for (i = 0; i < n; i++) buf[i] = t->in[t->pt_off + t->hs_off + i];
            t->hs_off += n;
            if (t->hs_off >= t->pt_len) { t->in_ready = 0; t->in_len = 0; t->hs_off = 0; }
            return (int)n;
        }
        if (t->state == SDTLS_CLOSED) return SDTLS_READ_EOF;
        if (t->state != SDTLS_CONNECTED) return t->state == SDTLS_FAILED ? SDTLS_READ_ERR : 0;
        pump_in(t);
        if (t->state == SDTLS_FAILED) return SDTLS_READ_ERR;
        if (!(t->in_ready && t->pt_type == CT_APP)) return t->state == SDTLS_CLOSED ? SDTLS_READ_EOF : 0;
    }
}

void sdtls_close(struct sdtls *t)
{
    static const sdc_u8 al[2] = { 1, 0 };
    if (t->state != SDTLS_CONNECTED) return;
    if (queue_record(t, CT_ALERT, al, 2)) { flush(t); t->state = SDTLS_CLOSED; }
}
