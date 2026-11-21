/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#include <string.h>
#include "ms_tlsget.h"
#include "sdhttp.h"

static struct sdtls g_tls;                 /* about 5 KB: static, one session at a time */

static int io_recv(void *c, sdc_u8 *b, sdc_u32 max) { const struct mstls_net *n = (const struct mstls_net *)c; return n->recv(n->ctx, b, max); }
static int io_send(void *c, const sdc_u8 *b, sdc_u32 len) { const struct mstls_net *n = (const struct mstls_net *)c; return n->send(n->ctx, b, len); }

const char *mstls_err(int e)
{
    switch (e) {
    case MSTLS_OK: return "ok";
    case MSTLS_E_NOPIN: return "no pin built in";
    case MSTLS_E_HS: return "handshake failed";
    case MSTLS_E_TIMEOUT: return "timeout";
    case MSTLS_E_IO: return "connection lost";
    case MSTLS_E_HTTP: return "not a 200";
    case MSTLS_E_BIG: return "answer too big";
    case MSTLS_E_PROTO: return "malformed answer";
    case MSTLS_E_MEM: return "no memory";
    case MSTLS_E_RNG: return "no randomness";
    default: return "?";
    }
}

static sdc_u8 g_post_req[MSTLS_POST_MAX + 512];      /* the whole POST request (head + body) in one buffer */

static int mstls_do(const struct mstls_net *net, const char *method, const char *host, const char *path, const sdc_u8 *pbody, sdc_u32 pblen, const sdc_u8 *pins, sdc_u32 npins,
                    sdtls_rand_fn rnd, void *rctx, sdc_u32 max_body, sdc_u32 deadline_ms, struct mstls_result *res)
{
    struct sdtls_io io;
    struct sdhttp_rsp rsp;
    sdc_u8 req_small[400], buf[2048], *req = req_small;
    sdc_u32 req_cap = sizeof req_small;
    sdc_u32 t0 = net->now(net->ctx), rl, sent = 0, got = 0;
    int st, last = -1, r, n, stage = 0;
    memset(res, 0, sizeof *res);
    if (!npins || !pins) return MSTLS_E_NOPIN;
    memset(&g_tls, 0, sizeof g_tls);
    io.recv = io_recv; io.send = io_send; io.ctx = (void *)net;
    sdtls_begin(&g_tls, &io, rnd, rctx, pins, npins, host);
    for (;;) {                                                           /* the handshake, polled */
        st = sdtls_poll(&g_tls);
        if (st == SDTLS_CONNECTED) break;
        if (st == SDTLS_FAILED || st == SDTLS_CLOSED) {
            res->tls_err = g_tls.err; res->alert = g_tls.alert;
            return g_tls.err == SDTLS_E_RNG ? MSTLS_E_RNG : (g_tls.err == SDTLS_E_IO ? MSTLS_E_IO : MSTLS_E_HS);
        }
        if ((sdc_u32)(net->now(net->ctx) - t0) >= deadline_ms) { res->tls_err = g_tls.err; return MSTLS_E_TIMEOUT; }
        if (st == last) net->wait(net->ctx, 3);                          /* nothing moved: do not spin */
        last = st;
    }
    res->hs_ms = (sdc_u32)(net->now(net->ctx) - t0);
    if (pbody) { req = g_post_req; req_cap = sizeof g_post_req; }
    rl = sdhttp_request(req, req_cap, method, host, path, pbody, pblen, pbody ? "text/plain" : 0, 0, 0);
    if (!rl) { sdtls_close(&g_tls); return MSTLS_E_PROTO; }
    sdhttp_rsp_init(&rsp);
    for (;;) {
        if (sent < rl) {                                                 /* the request */
            r = sdtls_write(&g_tls, req + sent, rl - sent);
            if (r < 0) return MSTLS_E_IO;
            sent += (sdc_u32)r;
        }
        st = sdtls_poll(&g_tls);                                         /* keeps the output flowing and decrypts */
        if (st == SDTLS_FAILED) { res->tls_err = g_tls.err; return MSTLS_E_IO; }
        n = sdtls_read(&g_tls, buf, sizeof buf);
        if (n == SDTLS_READ_EOF) return rsp.state == SDHTTP_DONE ? MSTLS_E_PROTO : MSTLS_E_IO;
        if (n == SDTLS_READ_ERR) return MSTLS_E_IO;
        if (n > 0) {
            const sdh_u8 *p = buf;
            sdc_u32 left = (sdc_u32)n;
            while (left) {
                const sdh_u8 *body = 0;
                sdh_u32 bl = 0;
                int used = sdhttp_feed(&rsp, p, left, &body, &bl);
                if (used < 0) return MSTLS_E_PROTO;
                if (rsp.state >= SDHTTP_BODY && stage == 0) {            /* the head is complete: the status and the length decide */
                    res->status = rsp.status;
                    if (rsp.status != 200) return MSTLS_E_HTTP;
                    if (!rsp.has_len || rsp.chunked || rsp.content_length == 0) return MSTLS_E_PROTO;
                    if (rsp.content_length > max_body) return MSTLS_E_BIG;
                    res->clen = rsp.content_length;
                    res->body = (sdc_u8 *)net->alloc(net->ctx, res->clen);
                    if (!res->body) return MSTLS_E_MEM;
                    stage = 1;
                }
                if (bl && stage == 1) {
                    if (got + bl > res->clen) return MSTLS_E_PROTO;
                    memcpy(res->body + got, body, bl);
                    got += bl;
                }
                if (used == 0 && !bl) break;
                p += used; left -= (sdc_u32)used;
                if (rsp.state == SDHTTP_DONE) break;
            }
            if (rsp.state == SDHTTP_DONE) {
                if (got != res->clen) return MSTLS_E_PROTO;
                sdtls_close(&g_tls);
                return MSTLS_OK;
            }
        } else if (sent >= rl) net->wait(net->ctx, 2);
        if ((sdc_u32)(net->now(net->ctx) - t0) >= deadline_ms) return MSTLS_E_TIMEOUT;
    }
}

int mstls_get(const struct mstls_net *net, const char *host, const char *path, const sdc_u8 *pins, sdc_u32 npins, sdtls_rand_fn rnd, void *rctx,
              sdc_u32 max_body, sdc_u32 deadline_ms, struct mstls_result *res)
{
    return mstls_do(net, "GET", host, path, 0, 0, pins, npins, rnd, rctx, max_body, deadline_ms, res);
}

int mstls_post(const struct mstls_net *net, const char *host, const char *path, const sdc_u8 *body, sdc_u32 blen, const sdc_u8 *pins, sdc_u32 npins, sdtls_rand_fn rnd, void *rctx,
               sdc_u32 deadline_ms, struct mstls_result *res)
{
    if (!body || !blen || blen > MSTLS_POST_MAX) { memset(res, 0, sizeof *res); return MSTLS_E_BIG; }
    return mstls_do(net, "POST", host, path, body, blen, pins, npins, rnd, rctx, 1024, deadline_ms, res);
}
