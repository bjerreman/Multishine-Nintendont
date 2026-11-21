/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_tp_ios: vbba_transport over asynchronous IOS sockets (Wii Starlet, Nintendont kernel). */
#ifndef VBBA_TP_IOS_H
#define VBBA_TP_IOS_H

#include "../../core/vbba_transport.h"
#include "vbba_ios.h"

#ifndef VBBA_TP_IOS_SOCKS
#define VBBA_TP_IOS_SOCKS 4
#endif
#ifndef VBBA_TP_NONBLOCK
#define VBBA_TP_NONBLOCK 0               /* 1 (b14a): sockets are made non-blocking and receives are polled every VBBA_TP_RX_POLL_MS, so no blocking recvfrom sits in IOS in front of the sendtos */
#endif
#ifndef VBBA_TP_RXPOLL
#define VBBA_TP_RXPOLL 0                 /* 1 (b18a): the libogc receive pattern.  No standing recvfrom: the socket is non-blocking and, every VBBA_TP_RX_POLL_MS (at once again after a datagram), ONE poll(POLLIN, timeout 0) is
                                            submitted; only when it reports data one recvfrom follows.  Requests to /dev/net/ip/top are strictly sequential (a sendto, a poll or a recvfrom, never two at once), as in libogc
                                            .  Excludes VBBA_TP_NONBLOCK (the b14a pattern). */
#endif
#if VBBA_TP_RXPOLL && VBBA_TP_NONBLOCK
#error "VBBA_TP_RXPOLL and VBBA_TP_NONBLOCK are two different receive patterns"
#endif
#ifndef VBBA_TP_RX_POLL_MS
#define VBBA_TP_RX_POLL_MS 2u
#endif
#ifndef VBBA_TP_IOS_RXQ
#define VBBA_TP_IOS_RXQ 2                /* recvfrom requests kept outstanding per socket */
#endif
/* VBBA_STEP (diagnostic builds, 9 = everything): 3 = send FIFO (24 buffers, 16 in IOS, queue while opening), 4 = receive order; below that the 8 slots of build 5c5ae7b */
#ifndef VBBA_STEP
#define VBBA_STEP 9
#endif
#ifndef VBBA_TP_IOS_TX
#if VBBA_STEP >= 3
#define VBBA_TP_IOS_TX 24
#else
#define VBBA_TP_IOS_TX 8
#endif
#endif
#ifdef NEVER_DEFINED
#define VBBA_TP_IOS_TX 24                /* send buffers: datagrams in flight in IOS plus datagrams queued behind them (was 8 slots and no queue) */
#endif
#ifndef VBBA_TP_IOS_TX_INFLIGHT
#if VBBA_STEP >= 3
#define VBBA_TP_IOS_TX_INFLIGHT 16
#else
#define VBBA_TP_IOS_TX_INFLIGHT 8
#endif
#endif
#ifdef NEVER_DEFINED
#define VBBA_TP_IOS_TX_INFLIGHT 16       /* sendto requests outstanding in IOS at most; at 60 datagrams/s this carries up to 530 ms of IOS latency without queueing */
#endif
#define VBBA_TP_RX_GATE_MS 0u            /* a finished receive waits this long for an OLDER outstanding one before it is handed on anyway (IOS serving requests in order never makes it wait) */
#define VBBA_TP_IOS_BUF 1504u            /* 1472 payload rounded up to 32 */
#define VBBA_TP_HIST 8                   /* sendto latency buckets: < 1, 10, 50, 100, 250, 500, 1000 ms and >= 1000 ms (vbba_tp_hist_edge_us) */

enum { VBBA_NET_OFF = 0, VBBA_NET_STARTING, VBBA_NET_WAITIP, VBBA_NET_UP, VBBA_NET_FAILED };

struct vbba_tp_ios_stats {
    vbba_u32 tx, tx_bytes, tx_busy, tx_err, rx, rx_bytes, rx_err, rx_trunc, opens, bind_fallback, open_fail, startups;
    /* path measurements (added after the fifth Wii run: 29 of 100 burst datagrams never reached the Mac, ~600 ms round trips) */
    vbba_u32 tx_peak;                                   /* most sendto slots in flight at once (of VBBA_TP_IOS_TX) */
    vbba_u32 tx_lat_n, tx_lat_sum_us, tx_lat_max_us;    /* sendto: submit -> IOS completion */
    vbba_u32 tx_hist[VBBA_TP_HIST];
    vbba_u32 rx_lat_n, rx_lat_max_us, rx_fast_n;        /* recvfrom with data: submit -> completion; fast = under 5 ms (a datagram was already queued in IOS) */
    vbba_u32 rx_dwell_sum_us, rx_dwell_max_us;          /* recvfrom completed -> collected by the thread that owns the transport */
    vbba_u32 rx_wait_n, rx_wait_sum_ms, rx_wait_max_ms; /* collected -> taken by the router (waiting for room in the chip's RX ring shows here) */
    vbba_u32 rx_ready_lost;                             /* a finished receive found no place in the ready list (cannot happen with RXQ requests per socket; guard) */
    /* send queue (replaces "8 slots, drop when full"): datagrams wait in a FIFO for a free in-flight place, newest wins when the buffers run out */
    vbba_u32 tx_sent;                                   /* sendto requests handed to IOS */
    vbba_u32 txq_peak, txq_drop_old, txq_drop_dead;     /* most datagrams queued at once; oldest queued datagram dropped because all buffers were taken; queued for a socket that closed or failed */
    vbba_u32 txq_wait_n, txq_wait_sum_ms, txq_wait_max_ms;   /* queued -> handed to IOS */
    vbba_u32 tx_refused;                                /* IOS refused a request: the datagram stays queued and is tried again */
    /* receive order: requests are numbered; a datagram is handed on in request order (a younger finished request waits <= VBBA_TP_RX_GATE_MS for an older one) */
    vbba_u32 poll_n, poll_ready, poll_err, rx_eagain;   /* VBBA_TP_RXPOLL: polls completed, of them with data / failed; recvfrom after a ready poll that still found nothing (result < 0) */
    vbba_u32 poll_lat_sum_us, poll_lat_max_us;          /* VBBA_TP_RXPOLL: poll submit -> IOS completion (microseconds) */
    vbba_u32 rx_inv;                                    /* completions that came back behind a younger request's completion */
    vbba_u32 rx_gated, rx_gate_timeout;                 /* datagrams held back for an older request; handed on after the wait ran out */
};
extern const vbba_u32 vbba_tp_hist_edge_us[VBBA_TP_HIST - 1];

struct vbba_tp_ios_sock {
    vbba_u8 state, closing, tried_any;
    vbba_s32 fd;
    vbba_u16 port, bound;
    struct vbba_ios_req ctl;
    struct vbba_ios_req tosr;            /* the IP_TOS setsockopt after the bind (only when tos is set) */
    struct vbba_ios_req pol;             /* VBBA_TP_RXPOLL: the poll request (one per socket, at most one request of the receive side in IOS) */
    struct vbba_ios_req rx[VBBA_TP_IOS_RXQ];
    vbba_u8 ready[VBBA_TP_IOS_RXQ];      /* finished receives, ordered by request number */
    vbba_u32 rdy_ms[VBBA_TP_IOS_RXQ];    /* time each finished receive was collected (indexed like rx[]) */
    vbba_u32 rseq[VBBA_TP_IOS_RXQ];      /* request numbers (submit order) */
    vbba_u32 next_seq, max_done;
    vbba_u8 gated[VBBA_TP_IOS_RXQ];
    vbba_u8 rxidle;                      /* VBBA_TP_NONBLOCK: bit i = receive slot i found no data and waits for the next poll */
    vbba_u8 rx_more;                     /* VBBA_TP_RXPOLL: the last receive carried a datagram: poll again at once */
    vbba_u32 rxp_ms;                     /* VBBA_TP_NONBLOCK / VBBA_TP_RXPOLL: time of the last receive poll */
    vbba_u8 gen;                         /* incremented at every open: queued datagrams of an earlier use of this handle are dropped */
    vbba_u8 nready;
    vbba_u8 rbuf[VBBA_TP_IOS_RXQ][VBBA_TP_IOS_BUF] __attribute__((aligned(32)));
};

#ifndef VBBA_TCP
#define VBBA_TCP 0
#endif
#if VBBA_TCP
#ifndef VBBA_TP_IOS_TCP
#define VBBA_TP_IOS_TCP 2                /* stream sockets (the router's VBBA_TCP_CONNS) */
#endif
#define VBBA_TP_IOS_TCPBUF 1024u         /* per direction, 32-byte multiple */
#define VBBA_TP_TCP_POLL_MIN_MS 4u       /* receive poll interval right after data ... */
#define VBBA_TP_TCP_POLL_MAX_MS 40u      /* ... growing to this while idle (IOS load while a connection sits open during a match) */
#define VBBA_TP_TCP_CONNECT_MS 12000u
enum { VTT_SOCK = 0, VTT_NB, VTT_CONN, VTT_CPOLL, VTT_CPROBE, VTT_UP };
struct vbba_tp_ios_tcp {
    vbba_u8 state, closing, eof, stage, shut_want, shut_sent, rx_stage, rx_hup;   /* rx_stage 0 = poll next, 1 = recv next; rx_hup: the last poll said hung up */
    vbba_s32 fd;
    vbba_u32 ip;
    vbba_u16 port;
    vbba_u32 t_open, rx_ms, rx_iv, tx_ms, tx_fail;
    vbba_u32 rn, roff, tn;                /* rbuf: bytes [roff, rn) wait for the router; tbuf: tn bytes wait for / are with IOS */
    struct vbba_ios_req ctl, rx, tx;
    vbba_u8 rbuf[VBBA_TP_IOS_TCPBUF] __attribute__((aligned(32)));
    vbba_u8 tbuf[VBBA_TP_IOS_TCPBUF] __attribute__((aligned(32)));
};
struct vbba_tp_ios_tcp_stats {
    vbba_u32 opens, open_fail, connected, conn_poll, eof, rx_bytes, tx_bytes, rx_n, tx_n, rx_poll, tx_short, tx_neg, closes, shut;
    vbba_s32 last_conn_rc, last_tx_rc, last_poll_rev;
};
#endif

struct vbba_tp_ios {
    const struct vbba_ios_ops *ios;
    vbba_u32 tos, tos_n;                 /* (NIN_MS_TOS): IP TOS byte set on every socket after its bind (0 = off, the default); setsockopt answers seen */
    vbba_s32 tos_rc, tos_fd;             /* the last answer and its socket */
    struct vbba_transport tp;
    int net;
    vbba_u32 ip, t_mark, t_start;
    struct vbba_ios_req ctl;
    struct vbba_tp_ios_sock s[VBBA_TP_IOS_SOCKS];
    struct vbba_ios_req tx[VBBA_TP_IOS_TX];
    vbba_u8 tbuf[VBBA_TP_IOS_TX][VBBA_TP_IOS_BUF] __attribute__((aligned(32)));
    struct { vbba_u32 ip, t_ms; vbba_u16 port, len; vbba_u8 h, gen; } txm[VBBA_TP_IOS_TX];   /* per buffer: destination, socket (+ its generation), queue time */
    vbba_u8 txfree[VBBA_TP_IOS_TX], nfree;                 /* free buffers */
    vbba_u8 txq[VBBA_TP_IOS_TX], qh, qn;                   /* FIFO of queued (not yet submitted) buffers */
    vbba_u32 tx_inflight, tx_cap, tx_retry_ms;             /* requests outstanding in IOS, their limit, earliest next try after a refusal */
    vbba_u32 rx_inio;                                      /* VBBA_TP_RXPOLL: poll / recvfrom requests in IOS (0 or 1); a sendto is handed over only while this is 0 and vice versa */
    int last_tx;                                           /* VBBA_TP_RXPOLL: the last request handed to IOS was a sendto (the receive side is served next when both are due) */
    int rx_order;                                          /* 1: hand receives on in request order (default) */
    struct vbba_tp_ios_stats st;
#if VBBA_TCP
    struct vbba_tp_ios_tcp tc[VBBA_TP_IOS_TCP];
    struct vbba_tp_ios_tcp_stats tst;
#endif
};

void vbba_tp_ios_init(struct vbba_tp_ios *t, const struct vbba_ios_ops *ios);
void vbba_tp_ios_set_tx_cap(struct vbba_tp_ios *t, vbba_u32 cap);   /* tests and experiments: sendto requests in flight at most (1..VBBA_TP_IOS_TX) */
const struct vbba_transport *vbba_tp_ios_transport(struct vbba_tp_ios *t);

#endif
