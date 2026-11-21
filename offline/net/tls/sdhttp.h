/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
#ifndef SDHTTP_H
#define SDHTTP_H
/* sdhttp: the smallest useful HTTP/1.1 client pieces for the console: build one request, parse one
   response incrementally.  Bodies: Content-Length, or "Transfer-Encoding: chunked" (public servers such as Slippi's GraphQL front end may answer
   chunked;  phase 2; chunk extensions ignored, trailers skipped, at most SDHTTP_TRAILER_MAX bytes of them).  No gzip, no redirects,
   no read-until-close bodies; the request says "Accept-Encoding: identity".  Header lines longer than 160 bytes are ignored except the ones we
   need.  Strict C89. */
typedef unsigned char sdh_u8;
typedef unsigned int sdh_u32;

/* Writes "METHOD path HTTP/1.1" + Host + Content-Length (+ Content-Type) + Connection: close|keep-alive + Range (if range_from > 0) + body.
   Returns the number of bytes, or 0 if cap is too small.  body may be NULL when blen is 0. */
sdh_u32 sdhttp_request(sdh_u8 *out, sdh_u32 cap, const char *method, const char *host, const char *path, const sdh_u8 *body, sdh_u32 blen,
                       const char *content_type, int keep_alive, sdh_u32 range_from);

#define SDHTTP_HEAD_STATUS 0
#define SDHTTP_HEAD_HEADERS 1
#define SDHTTP_BODY 2
#define SDHTTP_DONE 3
#define SDHTTP_ERROR 4

#define SDHTTP_TRAILER_MAX 2048
#define SDHTTP_CS_SIZE 0        /* chunked body: reading a chunk-size line */
#define SDHTTP_CS_DATA 1
#define SDHTTP_CS_CRLF 2        /* the CRLF after chunk data */
#define SDHTTP_CS_TRAILER 3     /* after the last chunk: header-like lines up to the empty line */

struct sdhttp_rsp {
    sdh_u8 state;
    sdh_u8 has_len, close, line_overflow;
    sdh_u8 chunked, cs;
    int status;
    sdh_u32 chunk_left, trailer_got;
    sdh_u32 content_length, body_got, content_range_start;
    sdh_u8 line[160];
    sdh_u32 lpos;
};
void sdhttp_rsp_init(struct sdhttp_rsp *r);
/* Feed bytes from the stream.  Body bytes found in this call are returned as *body / *blen (a pointer into `in`).  Returns the number of input
   bytes consumed, or -1 on a protocol error.  Content-Length bodies: < n only when the response is complete (the rest belongs to the next
   response).  Chunked bodies: one call returns at most ONE contiguous slice of body bytes, so consumed < n does not mean "done": call again with
   the rest (`in + used`, `n - used`) until the state is SDHTTP_DONE or the input is used up; body_got counts the decoded bytes. */
int sdhttp_feed(struct sdhttp_rsp *r, const sdh_u8 *in, sdh_u32 n, const sdh_u8 **body, sdh_u32 *blen);
#endif
