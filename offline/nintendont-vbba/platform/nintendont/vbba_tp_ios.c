/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_tp_ios: vbba_transport over asynchronous IOS sockets, see vbba_tp_ios.h.  C99, no libc, no heap. */
#include "vbba_tp_ios.h"

#define K_NET 1u
#define K_SOCK 2u
#define K_RX 3u
#define K_TX 4u
#define K_POL 5u
#define K_TCP 6u
#define TAG(k, s, i) (((k) << 16) | ((vbba_u32)(s) << 8) | (vbba_u32)(i))

#define IP_POLL_MS 250u
#ifndef IP_WAIT_MS
#define IP_WAIT_MS 12000u   /* libogc waits 100 x 100 ms for the address after its start-up calls; counted from the return of the start-up request; vbba_glue.c rotates start-up variants per attempt */
#endif
#define RETRY_MS 1000u

const vbba_u32 vbba_tp_hist_edge_us[VBBA_TP_HIST - 1] = { 1000u, 10000u, 50000u, 100000u, 250000u, 500000u, 1000000u };

static void cpy(vbba_u8 *d, const vbba_u8 *s, vbba_u32 n) { while (n--) *d++ = *s++; }
static vbba_u32 now(struct vbba_tp_ios *t) { return t->ios->now_ms(t->ios->ctx); }

static void tx_free(struct vbba_tp_ios *t, vbba_u8 i) { t->txfree[t->nfree++] = i; }

static int submit(struct vbba_tp_ios *t, struct vbba_ios_req *r, int op, vbba_u32 tag)
{
    r->op = op;
    r->tag = tag;
    r->result = 0;
    r->lat_us = r->dwell_us = 0;
    if (t->ios->submit(t->ios->ctx, r) < 0) { r->busy = 0; return -1; }
    r->busy = 1;
#if VBBA_TP_RXPOLL
    if (op == VBBA_IOS_POLL || op == VBBA_IOS_RECVFROM) t->rx_inio++;
#endif
    return 0;
}

static void arm_rx(struct vbba_tp_ios *t, int si, int i)
{
    struct vbba_tp_ios_sock *s = &t->s[si];
    struct vbba_ios_req *r = &s->rx[i];
    if (r->busy || s->state != VBBA_TP_OPEN || s->closing) return;
    r->fd = s->fd; r->buf = s->rbuf[i]; r->len = VBBA_TP_IOS_BUF; r->ip = 0; r->port = 0;
    s->rseq[i] = s->next_seq++; s->gated[i] = 0;
    s->rxidle &= (vbba_u8)~(1u << i);
    (void)submit(t, r, VBBA_IOS_RECVFROM, TAG(K_RX, si, i));
}

#if VBBA_TP_RXPOLL
#define REARM(t, h, i) ((t)->s[h].rx_more = 1)           /* the buffer is free again: the next poll follows at once (rx_step) */
#else
#define REARM(t, h, i) arm_rx(t, h, i)
#endif

static void sock_step(struct vbba_tp_ios *t, int si)
{
    struct vbba_tp_ios_sock *s = &t->s[si];
    if (s->state != VBBA_TP_OPENING || s->ctl.busy || t->net != VBBA_NET_UP) return;
    if (s->fd < 0) { (void)submit(t, &s->ctl, VBBA_IOS_SOCKET, TAG(K_SOCK, si, 0)); }
}

static void free_if_idle(struct vbba_tp_ios_sock *s)
{
    int i;
    if (!s->closing || s->ctl.busy || s->pol.busy || s->tosr.busy) return;
    for (i = 0; i < VBBA_TP_IOS_RXQ; i++) if (s->rx[i].busy) return;
    s->state = VBBA_TP_FREE; s->closing = 0; s->fd = -1; s->nready = 0;
}

#if VBBA_TCP
static void tcp_done(struct vbba_tp_ios *t, struct vbba_ios_req *r);
#endif
static void on_done(struct vbba_tp_ios *t, struct vbba_ios_req *r)
{
    vbba_u32 k = r->tag >> 16, si = (r->tag >> 8) & 0xffu, i = r->tag & 0xffu;
    struct vbba_tp_ios_sock *s = si < VBBA_TP_IOS_SOCKS ? &t->s[si] : 0;
#if VBBA_TCP
    if (k == K_TCP) { tcp_done(t, r); return; }
#endif
    if (k == K_NET) {
        if (r->op == VBBA_IOS_STARTUP) {
            t->net = r->result >= 0 ? VBBA_NET_WAITIP : VBBA_NET_FAILED;
            t->t_mark = now(t) - IP_POLL_MS;
            /* the address wait starts when the start-up call has come back: on our Wii that call alone takes ~20 s (kd/request 6), so
               counting from its submit (as before) made the first poll after it fail at once and cost a repeat start-up of the next variant */
            if (t->net == VBBA_NET_WAITIP) t->t_start = now(t);
        } else if (r->op == VBBA_IOS_GETIP) {
            if (r->result >= 0 && r->ip) { t->ip = r->ip; t->net = VBBA_NET_UP; }
        }
        if (t->net == VBBA_NET_FAILED) t->t_mark = now(t);
    } else if (k == K_SOCK && s && r->op == VBBA_IOS_SETTOS) {      /* the TOS answer only counts; the socket's open sequence does not wait for it */
        t->tos_n++; t->tos_rc = r->result; t->tos_fd = r->fd;
        free_if_idle(s);
    } else if (k == K_SOCK && s) {
        if (s->closing) {                                  /* closed while opening: close the fd once the call is back */
            if (r->op == VBBA_IOS_SOCKET && r->result >= 0) s->fd = r->result;
            if (r->op != VBBA_IOS_CLOSE && s->fd >= 0) { s->ctl.fd = s->fd; (void)submit(t, &s->ctl, VBBA_IOS_CLOSE, TAG(K_SOCK, si, 0)); return; }
            free_if_idle(s);
            return;
        }
        if (r->op == VBBA_IOS_SOCKET) {
            if (r->result < 0) { s->state = VBBA_TP_FAILED; t->st.open_fail++; return; }
            s->fd = r->result;
            s->ctl.fd = s->fd; s->ctl.port = s->port;
            (void)submit(t, &s->ctl, VBBA_IOS_BIND, TAG(K_SOCK, si, 0));
        } else if (r->op == VBBA_IOS_BIND) {
            if (r->result < 0 && !s->tried_any) {               /* port taken on the Wii: let IOS pick one */
                s->tried_any = 1; t->st.bind_fallback++;
                s->ctl.fd = s->fd; s->ctl.port = 0;
                (void)submit(t, &s->ctl, VBBA_IOS_BIND, TAG(K_SOCK, si, 0));
                return;
            }
            if (r->result < 0) { s->state = VBBA_TP_FAILED; t->st.open_fail++; return; }
            s->bound = s->tried_any ? 0 : s->port;              /* 0: unknown (IOS chose) */
            if (t->tos && !s->tosr.busy) { s->tosr.fd = s->fd; s->tosr.len = t->tos; (void)submit(t, &s->tosr, VBBA_IOS_SETTOS, TAG(K_SOCK, si, 0xFEu)); }
#if VBBA_TP_NONBLOCK || VBBA_TP_RXPOLL
            if (r->result >= 0) { s->ctl.fd = s->fd; (void)submit(t, &s->ctl, VBBA_IOS_NONBLOCK, TAG(K_SOCK, si, 0)); return; }
#endif
            s->state = VBBA_TP_OPEN;
            t->st.opens++;
#if VBBA_TP_RXPOLL
            s->rx_more = 0; s->rxp_ms = now(t) - VBBA_TP_RX_POLL_MS;      /* the first poll is due at once */
#else
            for (i = 0; i < VBBA_TP_IOS_RXQ; i++) arm_rx(t, (int)si, (int)i);
#endif
        }
#if VBBA_TP_NONBLOCK || VBBA_TP_RXPOLL
        else if (r->op == VBBA_IOS_NONBLOCK) {                   /* a refused fcntl leaves the socket blocking: counted (rx_err); b14a: receives then work as before; b18a: a poll that reports data is followed by a recvfrom that then cannot block either */
            if (r->result < 0) t->st.rx_err++;
            s->state = VBBA_TP_OPEN;
            t->st.opens++;
#if VBBA_TP_RXPOLL
            s->rx_more = 0; s->rxp_ms = now(t) - VBBA_TP_RX_POLL_MS;
#else
            for (i = 0; i < VBBA_TP_IOS_RXQ; i++) arm_rx(t, (int)si, (int)i);
#endif
        }
#endif
#if VBBA_TP_RXPOLL
    } else if (k == K_POL && s) {                          /* b18a: the poll came back; data -> exactly one recvfrom now, otherwise ask again after VBBA_TP_RX_POLL_MS */
        if (t->rx_inio) t->rx_inio--;
        if (s->closing) { free_if_idle(s); return; }
        t->st.poll_n++; t->st.poll_lat_sum_us += r->lat_us; if (r->lat_us > t->st.poll_lat_max_us) t->st.poll_lat_max_us = r->lat_us;
        s->rxp_ms = now(t);
        if (r->result < 0) { t->st.poll_err++; arm_rx(t, (int)si, 0); }                          /* a poll IOS refuses must not end the receiving: try the (non-blocking) recvfrom anyway */
        else if (r->result > 0) { t->st.poll_ready++; arm_rx(t, (int)si, 0); }
    } else if (k == K_RX && s && i < VBBA_TP_IOS_RXQ) {
        if (t->rx_inio) t->rx_inio--;
        if (s->closing) { free_if_idle(s); return; }
        if (r->result < 0) { t->st.rx_eagain++; s->rxp_ms = now(t); return; }                   /* the socket is non-blocking: nothing to read after all (or an error); asked again after the next poll */
        s->rx_more = 1;                                                                           /* a datagram came: poll again at once once it has been taken */
#else
    } else if (k == K_RX && s && i < VBBA_TP_IOS_RXQ) {
        if (s->closing) { free_if_idle(s); return; }
#if VBBA_TP_NONBLOCK
        if (r->result < 0) { s->rxidle |= (vbba_u8)(1u << i); s->rxp_ms = now(t); return; }   /* no data (EAGAIN): the next poll asks again */
#endif
#endif
        if (r->result < 0) { t->st.rx_err++; arm_rx(t, (int)si, (int)i); return; }
        if (r->result > 0) {                               /* measurements of receives that carried a datagram */
            t->st.rx_lat_n++;
            if (r->lat_us > t->st.rx_lat_max_us) t->st.rx_lat_max_us = r->lat_us;
            if (r->lat_us < 5000u) t->st.rx_fast_n++;
            t->st.rx_dwell_sum_us += r->dwell_us;
            if (r->dwell_us > t->st.rx_dwell_max_us) t->st.rx_dwell_max_us = r->dwell_us;
        }
        if (s->rseq[i] < s->max_done) t->st.rx_inv++; else s->max_done = s->rseq[i];      /* completed behind a younger request */
        if (s->nready < VBBA_TP_IOS_RXQ) {                                               /* kept in request order (rx_order) or in completion order */
            vbba_u32 k = s->nready;
            s->rdy_ms[i] = now(t);
            while (t->rx_order && k > 0 && s->rseq[s->ready[k - 1]] > s->rseq[i]) { s->ready[k] = s->ready[k - 1]; k--; }
            s->ready[k] = (vbba_u8)i; s->nready++;
        }
        else t->st.rx_ready_lost++;
    }
    /* K_TX: slot is free again (busy cleared by the IOS layer) */
    else if (k == K_TX) {
        vbba_u32 b;
        if (t->tx_inflight) t->tx_inflight--;
        tx_free(t, (vbba_u8)i);
        if (r->result < 0) t->st.tx_err++;
        t->st.tx_lat_n++; t->st.tx_lat_sum_us += r->lat_us;
        if (r->lat_us > t->st.tx_lat_max_us) t->st.tx_lat_max_us = r->lat_us;
        for (b = 0; b < VBBA_TP_HIST - 1 && r->lat_us >= vbba_tp_hist_edge_us[b]; b++) { }
        t->st.tx_hist[b]++;
    }
}

#if VBBA_TP_RXPOLL
/* The receive side is due on a socket: open, no datagram waiting to be taken, nothing of the receive side in IOS, and either the last receive carried a datagram or VBBA_TP_RX_POLL_MS passed since the last poll. */
static int rx_due(struct vbba_tp_ios *t, vbba_u32 n)
{
    int i;
    for (i = 0; i < VBBA_TP_IOS_SOCKS; i++) {
        struct vbba_tp_ios_sock *s = &t->s[i];
        if (s->state != VBBA_TP_OPEN || s->closing || s->nready || s->pol.busy || s->rx[0].busy) continue;
        if (s->rx_more || (vbba_u32)(n - s->rxp_ms) >= VBBA_TP_RX_POLL_MS) return 1;
    }
    return 0;
}

/* Submit ONE poll(POLLIN, 0) when the receive side is due and nothing else is in IOS (libogc: net_poll, then net_recvfrom only when it says ready). */
static void rx_step(struct vbba_tp_ios *t)
{
    vbba_u32 n = now(t);
    int i;
    if (t->tx_inflight || t->rx_inio) return;
    for (i = 0; i < VBBA_TP_IOS_SOCKS; i++) {
        struct vbba_tp_ios_sock *s = &t->s[i];
        if (s->state != VBBA_TP_OPEN || s->closing || s->nready || s->pol.busy || s->rx[0].busy) continue;
        if (!(s->rx_more || (vbba_u32)(n - s->rxp_ms) >= VBBA_TP_RX_POLL_MS)) continue;
        s->pol.fd = s->fd; s->pol.buf = 0; s->pol.len = 0; s->pol.ip = 0; s->pol.port = 0;
        s->rx_more = 0;
        if (submit(t, &s->pol, VBBA_IOS_POLL, TAG(K_POL, i, 0)) == 0) { t->last_tx = 0; return; }
        s->rxp_ms = n;                                      /* refused: try again after the poll interval */
    }
}
#endif

#if VBBA_TCP
/* ---- stream sockets (VBBA_TCP): socket -> O_NONBLOCK -> connect -> poll(write) until connected; then poll(read) -> recv, and send from tbuf.
   At most one request per direction (ctl / rx / tx) per socket in IOS; nothing here waits for IOS. ---- */
#define TT_CTL 0u
#define TT_RX 1u
#define TT_TX 2u
static void tcp_free_if_idle(struct vbba_tp_ios_tcp *c)
{
    if (!c->closing || c->ctl.busy || c->rx.busy || c->tx.busy) return;
    if (c->fd >= 0) return;                                  /* the close request goes first */
    c->state = VBBA_TP_FREE; c->closing = 0;
}
static void tcp_fail(struct vbba_tp_ios *t, struct vbba_tp_ios_tcp *c) { if (c->state != VBBA_TP_OPEN) t->tst.open_fail++; c->state = VBBA_TP_FAILED; }

static void tcp_step(struct vbba_tp_ios *t, int i)
{
    struct vbba_tp_ios_tcp *c = &t->tc[i];
    vbba_u32 n = now(t);
    if (c->state == VBBA_TP_FREE) return;
    if (c->closing) {
        if (c->fd >= 0 && !c->ctl.busy) { c->ctl.fd = c->fd; c->fd = -1; t->tst.closes++; (void)submit(t, &c->ctl, VBBA_IOS_CLOSE, TAG(K_TCP, i, TT_CTL)); }
        tcp_free_if_idle(c);
        return;
    }
    if (c->state == VBBA_TP_OPENING) {
        if ((vbba_u32)(n - c->t_open) > VBBA_TP_TCP_CONNECT_MS) { tcp_fail(t, c); return; }
        if (c->ctl.busy || t->net != VBBA_NET_UP) return;
        if (c->stage == VTT_SOCK && c->fd < 0) (void)submit(t, &c->ctl, VBBA_IOS_TCPSOCKET, TAG(K_TCP, i, TT_CTL));
        else if (c->stage == VTT_CPOLL && (vbba_u32)(n - c->rx_ms) >= 10u) {
            c->ctl.fd = c->fd; c->ctl.len = 0x08u | 0x01u | 0x02u; c->ctl.buf = 0;          /* write (connected), read, (errors come back in revents anyway) */
            t->tst.conn_poll++;
            (void)submit(t, &c->ctl, VBBA_IOS_TCPPOLL, TAG(K_TCP, i, TT_CTL));
        }
        return;
    }
    if (c->state != VBBA_TP_OPEN) return;
    /* receive: poll(read) then recv, paced by rx_iv */
    if (!c->rx.busy && !c->eof && c->rn == 0) {
        if (c->rx_stage == 1) {
            c->rx.fd = c->fd; c->rx.buf = c->rbuf; c->rx.len = VBBA_TP_IOS_TCPBUF;
            (void)submit(t, &c->rx, VBBA_IOS_RECV, TAG(K_TCP, i, TT_RX));
        } else if ((vbba_u32)(n - c->rx_ms) >= c->rx_iv) {
            c->rx.fd = c->fd; c->rx.buf = 0; c->rx.len = 0x01u | 0x02u;
            t->tst.rx_poll++;
            (void)submit(t, &c->rx, VBBA_IOS_TCPPOLL, TAG(K_TCP, i, TT_RX));
        }
    }
    /* send */
    if (!c->tx.busy && c->tn && (vbba_s32)(n - c->tx_ms) >= 0) {
        c->tx.fd = c->fd; c->tx.buf = c->tbuf; c->tx.len = c->tn;
        (void)submit(t, &c->tx, VBBA_IOS_SEND, TAG(K_TCP, i, TT_TX));
    }
    if (c->shut_want && !c->shut_sent && !c->tn && !c->tx.busy && !c->ctl.busy) {
        c->shut_sent = 1; t->tst.shut++;
        c->ctl.fd = c->fd; c->ctl.len = 1;                                                  /* SHUT_WR */
        (void)submit(t, &c->ctl, VBBA_IOS_SHUTDOWN, TAG(K_TCP, i, TT_CTL));
    }
}

static void tcp_done(struct vbba_tp_ios *t, struct vbba_ios_req *r)
{
    vbba_u32 si = (r->tag >> 8) & 0xffu, w = r->tag & 0xffu, n = now(t);
    struct vbba_tp_ios_tcp *c;
    if (si >= VBBA_TP_IOS_TCP) return;
    c = &t->tc[si];
    if (c->closing) {
        if (r->op == VBBA_IOS_TCPSOCKET && r->result >= 0) c->fd = r->result;          /* closed while the socket call was out: close what it made */
        tcp_step(t, (int)si);
        return;
    }
    if (w == TT_CTL) {
        switch (r->op) {
        case VBBA_IOS_TCPSOCKET:
            if (r->result < 0) { tcp_fail(t, c); return; }
            c->fd = r->result; c->stage = VTT_NB;
            c->ctl.fd = c->fd; (void)submit(t, &c->ctl, VBBA_IOS_NONBLOCK, TAG(K_TCP, si, TT_CTL));
            return;
        case VBBA_IOS_NONBLOCK:
            c->stage = VTT_CONN;
            c->ctl.fd = c->fd; c->ctl.ip = c->ip; c->ctl.port = c->port;
            (void)submit(t, &c->ctl, VBBA_IOS_CONNECT, TAG(K_TCP, si, TT_CTL));
            return;
        case VBBA_IOS_CONNECT:
            t->tst.last_conn_rc = r->result;
            if (r->result >= 0) { c->state = VBBA_TP_OPEN; c->stage = VTT_UP; t->tst.connected++; c->rx_ms = n - 1000u; c->rx_iv = VBBA_TP_TCP_POLL_MIN_MS; return; }
            c->stage = VTT_CPOLL; c->rx_ms = n;                                         /* in progress (or refused): the poll tells */
            return;
        case VBBA_IOS_TCPPOLL:
            t->tst.last_poll_rev = r->result;
            if (r->result < 0 || (r->result & 0xA0)) { tcp_fail(t, c); return; }        /* error / invalid */
            if ((r->result & 0x40) || ((r->result & 0x03) && !(r->result & 0x08))) {   /* hang-up / readable but not writable: refused, or a peer that already sent and closed: one recv decides */
                c->stage = VTT_CPROBE;
                c->rx.fd = c->fd; c->rx.buf = c->rbuf; c->rx.len = VBBA_TP_IOS_TCPBUF;
                (void)submit(t, &c->rx, VBBA_IOS_RECV, TAG(K_TCP, si, TT_RX));
                return;
            }
            if (r->result & 0x08) {                                                    /* writable = connected */
                c->state = VBBA_TP_OPEN; c->stage = VTT_UP; t->tst.connected++; c->rx_ms = n - 1000u; c->rx_iv = VBBA_TP_TCP_POLL_MIN_MS;
                return;
            }
            c->rx_ms = n;
            return;
        default:
            return;                                                                    /* SHUTDOWN / CLOSE: nothing to do */
        }
    }
    if (w == TT_RX && c->stage == VTT_CPROBE) {                                       /* the recv after an ambiguous connect poll */
        if (r->result < 0) { t->tst.last_poll_rev = r->result; tcp_fail(t, c); return; }
        c->state = VBBA_TP_OPEN; c->stage = VTT_UP; t->tst.connected++; c->rx_ms = n; c->rx_iv = VBBA_TP_TCP_POLL_MIN_MS;
        if (r->result > 0) { c->rn = (vbba_u32)r->result > VBBA_TP_IOS_TCPBUF ? VBBA_TP_IOS_TCPBUF : (vbba_u32)r->result; c->roff = 0; t->tst.rx_n++; t->tst.rx_bytes += c->rn; }
        else { c->eof = 1; t->tst.eof++; }
        return;
    }
    if (w == TT_RX) {
        if (r->op == VBBA_IOS_TCPPOLL) {
            c->rx_ms = n;
            if (r->result < 0 || (r->result & 0xA0)) { t->tst.last_poll_rev = r->result; tcp_fail(t, c); return; }
            c->rx_hup = (r->result & 0x40) ? 1 : 0;
            if (r->result & 0x43) c->rx_stage = 1;                                     /* data or hang-up: the recv says which */
            else if (c->rx_iv < VBBA_TP_TCP_POLL_MAX_MS) c->rx_iv += c->rx_iv / 2u + 1u;
            return;
        }
        c->rx_stage = 0; c->rx_ms = n;
        if (r->result > 0) {
            c->rn = (vbba_u32)r->result > VBBA_TP_IOS_TCPBUF ? VBBA_TP_IOS_TCPBUF : (vbba_u32)r->result; c->roff = 0;
            t->tst.rx_n++; t->tst.rx_bytes += c->rn;
            c->rx_iv = VBBA_TP_TCP_POLL_MIN_MS; c->rx_ms = n - VBBA_TP_TCP_POLL_MIN_MS;    /* more may be there: ask again at once */
        } else if (r->result == 0) { c->eof = 1; t->tst.eof++; }
        else if (c->rx_hup) { t->tst.last_poll_rev = r->result; tcp_fail(t, c); }      /* hung up and the recv fails: reset by the peer */
        return;                                                                         /* < 0 after a ready poll: nothing after all; poll again */
    }
    if (w == TT_TX) {
        t->tst.last_tx_rc = r->result;
        if (r->result > 0) {
            vbba_u32 k, m = (vbba_u32)r->result > c->tn ? c->tn : (vbba_u32)r->result;
            if (m < c->tn) t->tst.tx_short++;
            for (k = 0; k + m < c->tn; k++) c->tbuf[k] = c->tbuf[k + m];
            c->tn -= m; c->tx_fail = 0; t->tst.tx_n++; t->tst.tx_bytes += m;
        } else {                                                                        /* full (EAGAIN) or broken: try again in 10 ms; 300 in a row (~3 s) = dead */
            t->tst.tx_neg++;
            if (++c->tx_fail > 300u) { tcp_fail(t, c); return; }
            c->tx_ms = n + 10u;
        }
    }
}

static int ts_open(void *ctx, vbba_u32 ip, vbba_u16 port)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    int i;
    for (i = 0; i < VBBA_TP_IOS_TCP; i++) {
        struct vbba_tp_ios_tcp *c = &t->tc[i];
        if (c->state != VBBA_TP_FREE) continue;
        c->state = VBBA_TP_OPENING; c->closing = c->eof = c->stage = c->shut_want = c->shut_sent = c->rx_stage = c->rx_hup = 0;
        c->fd = -1; c->ip = ip; c->port = port; c->t_open = now(t); c->rn = c->roff = c->tn = 0; c->tx_fail = 0; c->tx_ms = c->t_open; c->rx_iv = VBBA_TP_TCP_POLL_MIN_MS;
        t->tst.opens++;
        tcp_step(t, i);
        return i;
    }
    return -1;
}
static int ts_state(void *ctx, int h)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    if (h < 0 || h >= VBBA_TP_IOS_TCP || t->tc[h].closing) return VBBA_TP_FREE;
    return t->tc[h].state;
}
static int ts_send(void *ctx, int h, const vbba_u8 *d, vbba_u16 len)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    struct vbba_tp_ios_tcp *c;
    vbba_u32 room, n, k;
    if (h < 0 || h >= VBBA_TP_IOS_TCP) return -1;
    c = &t->tc[h];
    if (c->closing || c->state != VBBA_TP_OPEN) return -1;
    if (c->tx.busy) return 0;                                 /* tbuf belongs to IOS while a send is out */
    room = VBBA_TP_IOS_TCPBUF - c->tn; n = len < room ? len : room;
    for (k = 0; k < n; k++) c->tbuf[c->tn + k] = d[k];
    c->tn += n;
    return (int)n;
}
static int ts_recv(void *ctx, int h, vbba_u8 *buf, vbba_u16 cap)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    struct vbba_tp_ios_tcp *c;
    vbba_u32 n;
    if (h < 0 || h >= VBBA_TP_IOS_TCP) return -1;
    c = &t->tc[h];
    if (c->closing || c->state == VBBA_TP_FAILED || c->state == VBBA_TP_FREE) return -1;
    if (c->rn > c->roff) {
        n = c->rn - c->roff; if (n > cap) n = cap;
        cpy(buf, c->rbuf + c->roff, n);
        c->roff += n;
        if (c->roff >= c->rn) c->rn = c->roff = 0;
        return (int)n;
    }
    return c->eof ? VBBA_TP_EOF : 0;
}
static void ts_close(void *ctx, int h)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    if (h < 0 || h >= VBBA_TP_IOS_TCP || t->tc[h].state == VBBA_TP_FREE || t->tc[h].closing) return;
    t->tc[h].closing = 1;
    tcp_step(t, h);
}
static void ts_shutdown(void *ctx, int h)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    if (h >= 0 && h < VBBA_TP_IOS_TCP) t->tc[h].shut_want = 1;
}
#endif

/* Hand queued datagrams to IOS, oldest first, while fewer than tx_cap requests are outstanding.  The head waits while its socket is still opening;
   a datagram whose socket closed or failed in the meantime is dropped (counted); a refused request stays queued and is tried again a few ms later. */
static void tx_pump(struct vbba_tp_ios *t)
{
    vbba_u32 n = now(t);
    int guard = 0;
#if VBBA_TP_RXPOLL
    if (t->rx_inio || (!t->last_tx && rx_due(t, n))) return;      /* one request at a time: a poll / recvfrom is in IOS, or the receive side is due and had no turn since the last sendto */
#endif
    while (t->qn && t->tx_inflight < t->tx_cap && guard++ < 64) {
        vbba_u8 i = t->txq[t->qh];
        struct vbba_ios_req *r = &t->tx[i];
        struct vbba_tp_ios_sock *s = &t->s[t->txm[i].h];
        if (s->closing || s->gen != t->txm[i].gen || s->state == VBBA_TP_FREE || s->state == VBBA_TP_FAILED) {
            t->qh = (vbba_u8)((t->qh + 1) % VBBA_TP_IOS_TX); t->qn--; tx_free(t, i); t->st.txq_drop_dead++;
            continue;
        }
        if (s->state != VBBA_TP_OPEN) break;
        if ((vbba_s32)(n - t->tx_retry_ms) < 0) break;
        r->fd = s->fd; r->ip = t->txm[i].ip; r->port = t->txm[i].port; r->buf = t->tbuf[i]; r->len = t->txm[i].len;
        if (submit(t, r, VBBA_IOS_SENDTO, TAG(K_TX, t->txm[i].h, i)) < 0) { t->st.tx_refused++; t->tx_retry_ms = n + 2u; break; }
        t->qh = (vbba_u8)((t->qh + 1) % VBBA_TP_IOS_TX); t->qn--;
#if VBBA_TP_RXPOLL
        t->last_tx = 1;
#endif
        t->tx_inflight++; t->st.tx_sent++;
        if (t->tx_inflight > t->st.tx_peak) t->st.tx_peak = t->tx_inflight;
        { vbba_u32 w = n - t->txm[i].t_ms; t->st.txq_wait_n++; t->st.txq_wait_sum_ms += w; if (w > t->st.txq_wait_max_ms) t->st.txq_wait_max_ms = w; }
    }
}

static void tp_poll(void *ctx)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    struct vbba_ios_req *d;
    vbba_u32 n = now(t);
    int i, guard = 0;
    while (guard++ < 64 && t->ios->poll(t->ios->ctx, &d) == 1) { d->busy = 0; on_done(t, d); }
#if VBBA_TP_RXPOLL
    rx_step(t);
#endif
    tx_pump(t);
    if (t->net == VBBA_NET_OFF && !t->ctl.busy) {
        t->st.startups++;
        t->t_start = n;
        t->net = VBBA_NET_STARTING;
        if (submit(t, &t->ctl, VBBA_IOS_STARTUP, TAG(K_NET, 0, 0)) < 0) { t->net = VBBA_NET_FAILED; t->t_mark = n; }   /* back off RETRY_MS: no startup storm (111,462 on the first Wii run) */
    } else if (t->net == VBBA_NET_WAITIP && !t->ctl.busy && (vbba_u32)(n - t->t_mark) >= IP_POLL_MS) {
        t->t_mark = n;
        if ((vbba_u32)(n - t->t_start) > IP_WAIT_MS) t->net = VBBA_NET_FAILED;
        else (void)submit(t, &t->ctl, VBBA_IOS_GETIP, TAG(K_NET, 0, 0));
    } else if (t->net == VBBA_NET_FAILED && (vbba_u32)(n - t->t_mark) >= RETRY_MS) {
        t->net = VBBA_NET_OFF;
    }
    for (i = 0; i < VBBA_TP_IOS_SOCKS; i++) sock_step(t, i);
#if VBBA_TCP
    for (i = 0; i < VBBA_TP_IOS_TCP; i++) tcp_step(t, i);
#endif
#if VBBA_TP_NONBLOCK
    for (i = 0; i < VBBA_TP_IOS_SOCKS; i++) {              /* one receive poll per socket at a time, every VBBA_TP_RX_POLL_MS */
        struct vbba_tp_ios_sock *s = &t->s[i];
        int k, busy = 0;
        if (s->state != VBBA_TP_OPEN || s->closing || !s->rxidle || (vbba_u32)(n - s->rxp_ms) < VBBA_TP_RX_POLL_MS) continue;
        for (k = 0; k < VBBA_TP_IOS_RXQ; k++) if (s->rx[k].busy) busy = 1;
        if (busy) continue;
        for (k = 0; k < VBBA_TP_IOS_RXQ; k++)
            if (s->rxidle & (1u << k)) { s->rxidle &= (vbba_u8)~(1u << k); s->rxp_ms = n; arm_rx(t, i, k); break; }
    }
#endif
}

static vbba_u32 tp_now(void *ctx) { return now((struct vbba_tp_ios *)ctx); }
static void tp_status(void *ctx, struct vbba_tp_status *o)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    o->up = t->net == VBBA_NET_UP; o->ip = o->up ? t->ip : 0; o->mask = 0; o->gw = 0;
}

static int tp_open(void *ctx, vbba_u16 port)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    int i;
    for (i = 0; i < VBBA_TP_IOS_SOCKS; i++) {
        struct vbba_tp_ios_sock *s = &t->s[i];
        if (s->state != VBBA_TP_FREE) continue;
        s->state = VBBA_TP_OPENING; s->closing = 0; s->tried_any = 0; s->fd = -1; s->port = port; s->bound = 0; s->nready = 0; s->gen++;
        sock_step(t, i);
        return i;
    }
    t->st.open_fail++;
    return -1;
}

static int tp_state(void *ctx, int h)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    if (h < 0 || h >= VBBA_TP_IOS_SOCKS || t->s[h].closing) return VBBA_TP_FREE;
    return t->s[h].state;
}
static vbba_u16 tp_bound(void *ctx, int h)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    return (h >= 0 && h < VBBA_TP_IOS_SOCKS) ? t->s[h].bound : 0;
}

static int tp_send(void *ctx, int h, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u16 len)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    vbba_u8 i;
    if (h < 0 || h >= VBBA_TP_IOS_SOCKS || t->s[h].closing || (t->s[h].state != VBBA_TP_OPEN && t->s[h].state != VBBA_TP_OPENING) || len > VBBA_TP_IOS_BUF) { t->st.tx_err++; return VBBA_ERROR; }
    if (!t->nfree) {
        if (!t->qn) { t->st.tx_busy++; return VBBA_WOULD_BLOCK; }                    /* every buffer is in flight in IOS */
        i = t->txq[t->qh]; t->qh = (vbba_u8)((t->qh + 1) % VBBA_TP_IOS_TX); t->qn--; tx_free(t, i);   /* newest wins: the oldest queued datagram goes */
        t->st.txq_drop_old++;
    }
    i = t->txfree[--t->nfree];
    cpy(t->tbuf[i], d, len);
    t->txm[i].ip = ip; t->txm[i].port = port; t->txm[i].len = len; t->txm[i].h = (vbba_u8)h; t->txm[i].gen = t->s[h].gen; t->txm[i].t_ms = now(t);
    t->txq[(t->qh + t->qn) % VBBA_TP_IOS_TX] = i; t->qn++;
    t->st.tx++; t->st.tx_bytes += len;
    tx_pump(t);
    if (t->qn > t->st.txq_peak) t->st.txq_peak = t->qn;          /* datagrams that had to wait (the pump has taken what IOS accepts) */
    return VBBA_OK;
}

static int tp_recv(void *ctx, int h, vbba_u8 *buf, vbba_u16 cap, vbba_u32 *ip, vbba_u16 *port)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    struct vbba_tp_ios_sock *s;
    struct vbba_ios_req *r;
    int i, k, n;
    if (h < 0 || h >= VBBA_TP_IOS_SOCKS) return -1;
    s = &t->s[h];
    if (s->state != VBBA_TP_OPEN || s->closing || !s->nready) return 0;
    i = s->ready[0];
    if (t->rx_order) {                                      /* hand datagrams on in request order: wait (briefly) for an older request that is still outstanding */
        for (k = 0; k < VBBA_TP_IOS_RXQ; k++)
            if (s->rx[k].busy && s->rseq[k] < s->rseq[i]) {
                if ((vbba_u32)(now(t) - s->rdy_ms[i]) < VBBA_TP_RX_GATE_MS) { if (!s->gated[i]) { s->gated[i] = 1; t->st.rx_gated++; } return 0; }
                t->st.rx_gate_timeout++;
                break;
            }
    }
    for (k = 1; k < s->nready; k++) s->ready[k - 1] = s->ready[k];
    s->nready--;
    r = &s->rx[i];
    n = r->result;
    *ip = r->ip; *port = r->port;
    if (n > (int)VBBA_TP_IOS_BUF || n > (int)cap) { t->st.rx_trunc++; REARM(t, h, i); return VBBA_TP_TRUNC; }
    cpy(buf, s->rbuf[i], (vbba_u32)n);
    { vbba_u32 w = now(t) - s->rdy_ms[i]; t->st.rx_wait_n++; t->st.rx_wait_sum_ms += w; if (w > t->st.rx_wait_max_ms) t->st.rx_wait_max_ms = w; }
    t->st.rx++; t->st.rx_bytes += (vbba_u32)n;
    REARM(t, h, i);
    return n > 0 ? n : 0;
}

static void tp_close(void *ctx, int h)
{
    struct vbba_tp_ios *t = (struct vbba_tp_ios *)ctx;
    struct vbba_tp_ios_sock *s;
    if (h < 0 || h >= VBBA_TP_IOS_SOCKS) return;
    s = &t->s[h];
    if (s->state == VBBA_TP_FREE || s->closing) return;
    s->closing = 1;
    if (s->fd >= 0 && !s->ctl.busy) { s->ctl.fd = s->fd; (void)submit(t, &s->ctl, VBBA_IOS_CLOSE, TAG(K_SOCK, h, 0)); }
    free_if_idle(s);
}

void vbba_tp_ios_init(struct vbba_tp_ios *t, const struct vbba_ios_ops *ios)
{
    vbba_u8 *p = (vbba_u8 *)t;
    vbba_u32 n = sizeof *t, i;
    while (n--) *p++ = 0;
    t->ios = ios;
    for (i = 0; i < VBBA_TP_IOS_SOCKS; i++) t->s[i].fd = -1;
    for (i = 0; i < VBBA_TP_IOS_TX; i++) t->txfree[i] = (vbba_u8)(VBBA_TP_IOS_TX - 1u - i);
    t->nfree = VBBA_TP_IOS_TX; t->tx_cap = VBBA_TP_IOS_TX_INFLIGHT; t->rx_order = VBBA_STEP >= 4;
    t->tp.queues_opening = VBBA_STEP >= 3;
    t->tp.ctx = t;
    t->tp.now_ms = tp_now; t->tp.status = tp_status; t->tp.poll = tp_poll; t->tp.open = tp_open; t->tp.state = tp_state;
    t->tp.bound_port = tp_bound; t->tp.send = tp_send; t->tp.recv = tp_recv; t->tp.close = tp_close; t->tp.allow = 0;
#if VBBA_TCP
    for (i = 0; i < VBBA_TP_IOS_TCP; i++) t->tc[i].fd = -1;
    t->tp.s_open = ts_open; t->tp.s_state = ts_state; t->tp.s_send = ts_send; t->tp.s_recv = ts_recv; t->tp.s_close = ts_close; t->tp.s_shutdown = ts_shutdown;
#endif
}

const struct vbba_transport *vbba_tp_ios_transport(struct vbba_tp_ios *t) { return &t->tp; }

void vbba_tp_ios_set_tx_cap(struct vbba_tp_ios *t, vbba_u32 cap)
{
    t->tx_cap = cap < 1u ? 1u : cap > VBBA_TP_IOS_TX ? (vbba_u32)VBBA_TP_IOS_TX : cap;
}
