/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#include "sdhttp.h"

static sdh_u32 slen(const char *s) { sdh_u32 n = 0; while (s[n]) n++; return n; }
static int put(sdh_u8 *o, sdh_u32 cap, sdh_u32 *pos, const char *s)
{
    sdh_u32 n = slen(s), i;
    if (*pos + n > cap) return 0;
    for (i = 0; i < n; i++) o[*pos + i] = (sdh_u8)s[i];
    *pos += n;
    return 1;
}
static int put_num(sdh_u8 *o, sdh_u32 cap, sdh_u32 *pos, sdh_u32 v)
{
    char t[12];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    if (*pos + (sdh_u32)n > cap) return 0;
    while (n) o[(*pos)++] = (sdh_u8)t[--n];
    return 1;
}

sdh_u32 sdhttp_request(sdh_u8 *out, sdh_u32 cap, const char *method, const char *host, const char *path, const sdh_u8 *body, sdh_u32 blen,
                       const char *content_type, int keep_alive, sdh_u32 range_from)
{
    sdh_u32 p = 0, i;
    if (!put(out, cap, &p, method) || !put(out, cap, &p, " ") || !put(out, cap, &p, path) || !put(out, cap, &p, " HTTP/1.1\r\nHost: ") ||
        !put(out, cap, &p, host) || !put(out, cap, &p, "\r\nAccept-Encoding: identity\r\nConnection: ") ||
        !put(out, cap, &p, keep_alive ? "keep-alive" : "close") || !put(out, cap, &p, "\r\n")) return 0;
    if (range_from) {
        if (!put(out, cap, &p, "Range: bytes=") || !put_num(out, cap, &p, range_from) || !put(out, cap, &p, "-\r\n")) return 0;
    }
    if (blen || body) {
        if (content_type && (!put(out, cap, &p, "Content-Type: ") || !put(out, cap, &p, content_type) || !put(out, cap, &p, "\r\n"))) return 0;
        if (!put(out, cap, &p, "Content-Length: ") || !put_num(out, cap, &p, blen) || !put(out, cap, &p, "\r\n")) return 0;
    }
    if (!put(out, cap, &p, "\r\n")) return 0;
    if (p + blen > cap) return 0;
    for (i = 0; i < blen; i++) out[p + i] = body[i];
    return p + blen;
}

void sdhttp_rsp_init(struct sdhttp_rsp *r)
{
    sdh_u8 *b = (sdh_u8 *)r;
    sdh_u32 i;
    for (i = 0; i < sizeof(*r); i++) b[i] = 0;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int starts(const sdh_u8 *l, sdh_u32 n, const char *p)
{
    sdh_u32 i = 0;
    while (p[i]) { if (i >= n || lc(l[i]) != p[i]) return 0; i++; }
    return 1;
}
static int parse_uint(const sdh_u8 *l, sdh_u32 n, sdh_u32 i, sdh_u32 *v)
{
    sdh_u32 x = 0, d = 0;
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    while (i < n && l[i] >= '0' && l[i] <= '9') {
        if (x > 429496729u) return 0;
        x = x * 10 + (sdh_u32)(l[i] - '0'); i++; d++;
    }
    if (!d) return 0;
    *v = x;
    return 1;
}

static int on_line(struct sdhttp_rsp *r, const sdh_u8 *l, sdh_u32 n)
{
    sdh_u32 v;
    if (r->state == SDHTTP_HEAD_STATUS) {
        /* "HTTP/1.1 200 OK" */
        if (n < 12 || !starts(l, n, "http/1.") || l[8] != ' ') return -1;
        if (!parse_uint(l, n, 9, &v) || v < 100 || v > 599) return -1;
        r->status = (int)v;
        r->state = SDHTTP_HEAD_HEADERS;
        return 0;
    }
    if (n == 0) {                                    /* end of headers */
        if (r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200)) { r->has_len = 1; r->content_length = 0; }
        if (r->chunked) { r->state = SDHTTP_BODY; r->cs = SDHTTP_CS_SIZE; return 0; }
        if (!r->has_len) return -1;                  /* no read-to-close: refuse rather than guess */
        r->state = r->content_length ? SDHTTP_BODY : SDHTTP_DONE;
        return 0;
    }
    if (r->line_overflow) return 0;
    if (starts(l, n, "content-length:")) {
        if (!parse_uint(l, n, 15, &v)) return -1;
        r->content_length = v; r->has_len = 1;
    } else if (starts(l, n, "connection:")) {
        sdh_u32 i;
        for (i = 11; i + 5 <= n; i++) if (starts(l + i, n - i, "close")) r->close = 1;
    } else if (starts(l, n, "transfer-encoding:")) {
        sdh_u32 i = 18, e = n;
        while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
        while (e > i && (l[e - 1] == ' ' || l[e - 1] == '\t')) e--;
        if (e - i != 7 || !starts(l + i, e - i, "chunked")) return -1;      /* gzip, "gzip, chunked", anything else: refuse */
        r->chunked = 1;
    } else if (starts(l, n, "content-range:")) {
        sdh_u32 i = 14;
        while (i < n && l[i] != ' ') i++;
        while (i < n && l[i] == ' ') i++;
        if (starts(l + i, n - i, "bytes ")) parse_uint(l, n, i + 6, &r->content_range_start);
    }
    return 0;
}

static int hexval(sdh_u8 c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* chunked body: consume from in[i..n); returns the new i, or -1 on error.  Sets body and blen when a data slice was found (then returns at once). */
static int feed_chunked(struct sdhttp_rsp *r, const sdh_u8 *in, sdh_u32 n, sdh_u32 i, const sdh_u8 **body, sdh_u32 *blen)
{
    while (i < n && r->state == SDHTTP_BODY) {
        sdh_u8 c;
        if (r->cs == SDHTTP_CS_DATA) {
            sdh_u32 take = n - i;
            if (take > r->chunk_left) take = r->chunk_left;
            *body = in + i; *blen = take;
            r->chunk_left -= take; r->body_got += take;
            i += take;
            if (r->chunk_left == 0) r->cs = SDHTTP_CS_CRLF;
            r->lpos = 0;
            return (int)i;
        }
        c = in[i++];
        if (r->cs == SDHTTP_CS_CRLF) {               /* exactly CR LF (a bare LF is tolerated) */
            if (c == '\r' && r->lpos == 0) { r->lpos = 1; continue; }
            if (c != '\n') return -1;
            r->lpos = 0; r->cs = SDHTTP_CS_SIZE;
        } else if (r->cs == SDHTTP_CS_SIZE) {
            if (c == '\n') {
                sdh_u32 k = 0, v = 0, digits = 0;
                while (k < r->lpos && hexval(r->line[k]) >= 0) {
                    if (v > 0x07FFFFFFu) return -1;
                    v = v * 16 + (sdh_u32)hexval(r->line[k]); k++; digits++;
                }
                if (!digits) return -1;
                if (k < r->lpos && r->line[k] != ';' && r->line[k] != ' ' && r->line[k] != '\t' && r->line[k] != '\r') return -1;   /* extensions ignored */
                if (r->body_got + v < r->body_got) return -1;
                r->lpos = 0; r->line_overflow = 0;
                if (v == 0) { r->cs = SDHTTP_CS_TRAILER; r->trailer_got = 0; }
                else { r->chunk_left = v; r->cs = SDHTTP_CS_DATA; }
            } else if (r->lpos < sizeof r->line) r->line[r->lpos++] = c;
            else return -1;                          /* a chunk-size line this long is not a chunk-size line */
        } else {                                     /* trailer: skip lines until an empty one */
            if (++r->trailer_got > SDHTTP_TRAILER_MAX) return -1;
            if (c == '\n') {
                if (r->lpos == 0 || (r->lpos == 1 && r->line[0] == '\r')) { r->state = SDHTTP_DONE; r->lpos = 0; return (int)i; }
                r->lpos = 0;
            } else if (r->lpos == 0 || r->lpos < 2) r->line[r->lpos++] = c;
            else r->lpos = 2;                        /* only "is the line empty" matters */
        }
    }
    return (int)i;
}

int sdhttp_feed(struct sdhttp_rsp *r, const sdh_u8 *in, sdh_u32 n, const sdh_u8 **body, sdh_u32 *blen)
{
    sdh_u32 i = 0;
    *body = in; *blen = 0;
    if (r->state == SDHTTP_ERROR) return -1;
    while (i < n && r->state < SDHTTP_BODY) {
        sdh_u8 c = in[i++];
        if (c == '\n') {
            sdh_u32 l = r->lpos;
            if (l && r->line[l - 1] == '\r') l--;
            if (r->line_overflow && r->state == SDHTTP_HEAD_STATUS) { r->state = SDHTTP_ERROR; return -1; }
            if (on_line(r, r->line, l) < 0) { r->state = SDHTTP_ERROR; return -1; }
            r->lpos = 0; r->line_overflow = 0;
        } else if (r->lpos < sizeof r->line) {
            r->line[r->lpos++] = c;
        } else {
            r->line_overflow = 1;
        }
    }
    if (r->state == SDHTTP_BODY && r->chunked) {
        int k = feed_chunked(r, in, n, i, body, blen);
        if (k < 0) { r->state = SDHTTP_ERROR; return -1; }
        return k;
    }
    if (r->state == SDHTTP_BODY) {
        sdh_u32 left = r->content_length - r->body_got, take = n - i;
        if (take > left) take = left;
        *body = in + i; *blen = take;
        r->body_got += take;
        i += take;
        if (r->body_got == r->content_length) r->state = SDHTTP_DONE;
    }
    return (int)i;
}
