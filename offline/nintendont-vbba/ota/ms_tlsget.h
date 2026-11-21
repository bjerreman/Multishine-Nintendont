/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* One HTTPS GET over the repo's in-house TLS 1.3 client (sdtls: pinned Ed25519 server key, no X.509, no clock) on top of a byte stream the caller supplies (the loader backs it with non-blocking libogc
 * sockets, the host test with BSD sockets).  Polled with a hard deadline, never a blocking call; every failure is a code, the caller then falls back to plain HTTP (the downloaded files are signature-verified
 * anyway). */
#ifndef MS_TLSGET_H
#define MS_TLSGET_H
#include "sdtls.h"

struct mstls_net {
    void *ctx;
    int (*recv)(void *ctx, sdc_u8 *buf, sdc_u32 max);          /* > 0 bytes, 0 nothing now, < 0 closed or failed */
    int (*send)(void *ctx, const sdc_u8 *buf, sdc_u32 n);      /* >= 0 bytes accepted, < 0 failed */
    sdc_u32 (*now)(void *ctx);                                  /* milliseconds */
    void (*wait)(void *ctx, sdc_u32 ms);
    void *(*alloc)(void *ctx, sdc_u32 n);                       /* the buffer for the body (the caller frees it) */
};
enum { MSTLS_OK = 0, MSTLS_E_NOPIN, MSTLS_E_HS, MSTLS_E_TIMEOUT, MSTLS_E_IO, MSTLS_E_HTTP, MSTLS_E_BIG, MSTLS_E_PROTO, MSTLS_E_MEM, MSTLS_E_RNG };
struct mstls_result {
    int status;                 /* the HTTP status (0 = none) */
    sdc_u32 clen;               /* the body length */
    sdc_u8 *body;               /* allocated through net->alloc once the length is known, clen bytes; non-NULL also after a later failure: the caller frees it */
    int tls_err, alert;         /* sdtls_err / alert when the handshake failed */
    sdc_u32 hs_ms;              /* time from the start to the end of the handshake */
};
/* GET path from host (also the SNI) within deadline_ms of the call, at most max_body bytes.  MSTLS_OK only for a 200 with a Content-Length body that arrived whole. */
int mstls_get(const struct mstls_net *net, const char *host, const char *path, const sdc_u8 *pins, sdc_u32 npins, sdtls_rand_fn rnd, void *rctx,
              sdc_u32 max_body, sdc_u32 deadline_ms, struct mstls_result *res);
#define MSTLS_POST_MAX 16384u      /* the https site's request body cap */
/* POST body (text/plain, at most MSTLS_POST_MAX bytes) over the same pinned TLS; the answer is read like a GET's (a small Content-Length body, at most 1 KB).  MSTLS_OK only for a 200; a refusal keeps res->status (429 = rate limited) */
int mstls_post(const struct mstls_net *net, const char *host, const char *path, const sdc_u8 *body, sdc_u32 blen, const sdc_u8 *pins, sdc_u32 npins, sdtls_rand_fn rnd, void *rctx,
               sdc_u32 deadline_ms, struct mstls_result *res);
const char *mstls_err(int e);
#endif
