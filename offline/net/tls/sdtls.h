/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#ifndef SDTLS_H
#define SDTLS_H
/* sdtls: a deliberately narrow TLS 1.3 client for the console.
   Two server-authentication modes: PINNED key (below, for our own server) and WEBPKI (sdtls_begin_web: X.509 chain to a compiled-in anchor,
   host name check, validity dates against a caller-supplied trusted time; RSA-PSS or ECDSA CertificateVerify).
   One ciphersuite (TLS_CHACHA20_POLY1305_SHA256), one group (x25519), one signature scheme (Ed25519), server authentication by PINNED
   public key (no X.509 chain, no names, no dates, so no clock), no client certificates, no HelloRetryRequest, no 0-RTT, no compression,
   no renegotiation.  Handshake messages must fit one record each side of a record boundary (a message may not be fragmented over records).
   Transport and randomness are callbacks, so the same code runs over sdtcp on the console and over a host socket in the tests.
   Non-blocking: sdtls_poll does I/O plus at most ONE expensive operation per call (x25519 or Ed25519 verify), so the caller decides on
   which frame the 10-70 ms operations run. */
#include "sdcrypto.h"
#ifdef SDTLS_WEBPKI               /* build with -DSDTLS_WEBPKI to include the WebPKI mode (adds about 20 KB of code: sdpk + sdx509 + sdroots must be linked) */
#include "sdx509.h"
#endif

#ifndef SDTLS_MAX_PLAIN                 /* plaintext bytes per record we send and accept (advertised via max_fragment_length 2^11) */
#define SDTLS_MAX_PLAIN 2048
#endif
#define SDTLS_IN_BUF (5 + SDTLS_MAX_PLAIN + 256)        /* RFC 8446 5.2: ciphertext may exceed the plaintext limit by up to 255 */
#define SDTLS_OUT_BUF (5 + SDTLS_MAX_PLAIN + 1 + 16)

/* poll results / state */
#define SDTLS_IDLE 0
#define SDTLS_START 1           /* generating the key share (heavy: x25519) */
#define SDTLS_SEND_CH 2
#define SDTLS_WAIT_SH 3
#define SDTLS_WAIT_ECDH 4       /* heavy: x25519 shared secret */
#define SDTLS_WAIT_EE 5
#define SDTLS_WAIT_CERT 6
#define SDTLS_WAIT_CV 7
#define SDTLS_WAIT_VERIFY 8     /* heavy: Ed25519 verify */
#define SDTLS_WAIT_FIN 9
#define SDTLS_CONNECTED 10
#define SDTLS_CLOSED 11         /* peer sent close_notify or we did */
#define SDTLS_FAILED 12
#define SDTLS_WAIT_CHAIN 13     /* WebPKI only; heavy: one certificate signature per poll */

/* errors (tls->err, positive) */
#define SDTLS_E_NONE 0
#define SDTLS_E_PROTO 1         /* malformed or unexpected message */
#define SDTLS_E_UNSUPPORTED 2   /* peer chose something we do not offer */
#define SDTLS_E_PIN 3           /* server key is not in the pin list */
#define SDTLS_E_SIG 4           /* CertificateVerify did not verify */
#define SDTLS_E_FINISHED 5
#define SDTLS_E_MAC 6           /* record authentication failed */
#define SDTLS_E_IO 7            /* transport closed or failed before the handshake ended */
#define SDTLS_E_OVERFLOW 8      /* record or handshake message larger than our buffers */
#define SDTLS_E_RNG 9           /* randomness not ready */
#define SDTLS_E_ALERT 10        /* peer sent a fatal alert (tls->alert holds the description) */
#define SDTLS_E_SECRET 11       /* all-zero x25519 shared secret */
#define SDTLS_E_CERT 12         /* certificate malformed, unsupported, or refused by policy (web->cert_err has the sdx509 code) */
#define SDTLS_E_UNTRUSTED 13    /* no path to a compiled-in anchor */
#define SDTLS_E_HOST 14         /* certificate does not cover the host name */
#define SDTLS_E_TIME 15         /* expired, not yet valid, or no trusted time to judge by */

/* read return codes */
#define SDTLS_READ_EOF (-1)     /* close_notify received and all data delivered */
#define SDTLS_READ_ERR (-2)

struct sdtls_io {
    int (*recv)(void *ctx, sdc_u8 *buf, sdc_u32 max);           /* bytes (>0), 0 = none now, <0 = closed or failed */
    int (*send)(void *ctx, const sdc_u8 *buf, sdc_u32 n);       /* bytes accepted (0 = none now), <0 = failed */
    void *ctx;
};
typedef int (*sdtls_rand_fn)(void *ctx, sdc_u8 *out, sdc_u32 n);   /* 0 ok, nonzero = not ready */

#define SDTLS_WEB_RX (5 + 16384 + 256)          /* record buffer a WebPKI session needs: servers send full 16 KB records */
#ifdef SDTLS_WEBPKI

/* WebPKI session context, owned by the caller (about 21 KB with the record buffer; allocate it from the overlay arena, only while a session lives). */
struct sdtls_web {
    const struct sdx_anchor *anchors; sdc_u32 nanchors;     /* normally sdx_roots / sdx_nroots */
    sdc_u32 now;                                            /* trusted Unix time, 0 = none (then handshakes fail with SDTLS_E_TIME unless SDX_F_NO_TIME) */
    sdc_u32 flags;                                          /* SDX_F_* */
    sdc_u8 *rx; sdc_u32 rx_cap;                             /* record buffer, rx_cap >= SDTLS_WEB_RX */
    int cert_err;                                           /* last sdx509 error code (0 none) */
    struct sdx_chain chain;
    struct sdpk_ws ws;
    sdc_u8 leaf_type;                                       /* SDX_KEY_* */
    sdc_u8 leaf_n[SDPK_RSA_MAX_BYTES]; sdc_u32 leaf_n_len;
    sdc_u8 leaf_e[4]; sdc_u32 leaf_e_len;
    sdc_u8 leaf_pt[97];
    sdc_u32 scheme;                                         /* CertificateVerify signature scheme */
    sdc_u8 sig[SDPK_RSA_MAX_BYTES]; sdc_u32 sig_len;
};

#else
struct sdtls_web;
#endif

struct sdtls {
    sdc_u8 state, err, alert, heavy_kind;
    sdc_u8 enc_in, enc_out, ku_pending, pad;
    struct sdtls_io io;
    sdtls_rand_fn rand;
    void *rand_ctx;
    const sdc_u8 *pins;         /* npins * 32 bytes */
    sdc_u32 npins;
    const char *sni;            /* NULL = no server_name extension */
    struct sdc_sha th;          /* transcript */
    sdc_u8 priv[32], srv_pub[32], sh_hash[32], cvhash[32], sig[64], pinned[32];
    sdc_u8 hs[32];              /* handshake secret */
    sdc_u8 c_hs[32], s_hs[32];  /* handshake traffic secrets */
    sdc_u8 c_secret[32], s_secret[32];                          /* current application traffic secrets */
    sdc_u8 c_key[32], c_iv[12], s_key[32], s_iv[12];
    sdc_u32 c_seq[2], s_seq[2];
    sdc_u32 in_len;             /* bytes in in[] */
    sdc_u32 pt_off, pt_len;     /* decrypted/plain payload inside in[] */
    sdc_u8 pt_type;
    sdc_u8 in_ready;            /* in[] holds a complete record that has been decrypted (pt_*) */
    sdc_u32 hs_off;
    sdc_u32 out_len, out_off;
    struct sdtls_web *web;      /* NULL = pinned mode */
    sdc_u8 *in;                 /* in_small, or web->rx */
    sdc_u32 in_cap, max_in;     /* buffer size, largest plaintext record accepted */
    sdc_u8 in_small[SDTLS_IN_BUF];
    sdc_u8 out[SDTLS_OUT_BUF];
    sdc_u32 stat_heavy[2];      /* number of x25519 / Ed25519 operations run */
};

/* Start a handshake.  pins: npins (1..4) 32-byte Ed25519 public keys, any one of which the server must hold.  Returns 0. */
int sdtls_begin(struct sdtls *t, const struct sdtls_io *io, sdtls_rand_fn rnd, void *rand_ctx, const sdc_u8 *pins, sdc_u32 npins, const char *sni);
/* Start a WebPKI handshake to `host` (also sent as SNI and checked against the certificate's subjectAltName).  web must be filled (anchors, now,
   flags, rx, rx_cap) and stay alive until the session ends. */
#ifdef SDTLS_WEBPKI
int sdtls_begin_web(struct sdtls *t, const struct sdtls_io *io, sdtls_rand_fn rnd, void *rand_ctx, struct sdtls_web *web, const char *host);
#endif
/* Drive I/O and the handshake (at most one heavy operation per call).  Returns the state. */
int sdtls_poll(struct sdtls *t);
/* 1 if the next sdtls_poll will run a heavy operation (x25519 / Ed25519), 0 otherwise: lets the caller pick the frame. */
int sdtls_heavy_next(const struct sdtls *t);
/* Application data.  write returns bytes accepted (0 = busy, retry), <0 on error; read returns bytes, 0 = nothing yet, SDTLS_READ_EOF, SDTLS_READ_ERR. */
int sdtls_write(struct sdtls *t, const sdc_u8 *buf, sdc_u32 n);
int sdtls_read(struct sdtls *t, sdc_u8 *buf, sdc_u32 max);
/* Queue close_notify (and flush on later polls).  After that no more writes are accepted. */
void sdtls_close(struct sdtls *t);
int sdtls_out_pending(const struct sdtls *t);
#endif
