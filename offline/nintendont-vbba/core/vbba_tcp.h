/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_tcp: the router's TCP splice (outbound client connections only).  The guest's TCP stack (offline/net/sdtcp.c)
 * talks to the router as if it were the remote peer; the router carries the BYTES over a platform stream socket
 * (vbba_transport s_open/s_send/s_recv, IOS SOCK_STREAM on the Wii).  TLS stays end-to-end in the guest.
 *
 *   SYN from the guest      allow-list + anti-spoof as UDP; a free slot; s_open(dst).  No answer until the platform
 *                           connect succeeded: SYN-ACK (MSS option) then, or RST|ACK when it failed (the guest sees a
 *                           refused connect within a few hundred ms instead of a SYN timeout).
 *   guest -> socket         in order only (anything else: duplicate ACK); window = free space of the to-socket buffer.
 *   socket -> guest         go-back-N, at most VBBA_TCP_INFLIGHT bytes in flight and never more than the guest's window,
 *                           fixed RTO with backoff (the guest link is the BBA ring: loss means a full ring).
 *   FIN / RST               guest FIN is ACKed; the socket's EOF becomes our FIN after the data; guest RST closes the socket;
 *                           a dead socket becomes RST|ACK.  Segments for no connection get RST (RFC 793 rules).
 * No heap, no libc; state lives in struct vbba_router (VBBA_TCP_CONNS x 2 x VBBA_TCP_BUF bytes).
 * Compiled into vbba_router.c (included at its end when VBBA_TCP is 1).
 */
#ifndef VBBA_TCP_H
#define VBBA_TCP_H

#include "vbba_types.h"

#ifndef VBBA_TCP
#define VBBA_TCP 0
#endif
#ifndef VBBA_TCP_CONNS
#define VBBA_TCP_CONNS 2               /* concurrent guest connections (matchmaker + one more) */
#endif
#ifndef VBBA_TCP_BUF
#define VBBA_TCP_BUF 2048u             /* bytes per direction per connection (power of two) */
#endif
#ifndef VBBA_TCP_MSS
#define VBBA_TCP_MSS 1400u             /* largest segment toward the guest (and the MSS we announce) */
#endif
#ifndef VBBA_TCP_INFLIGHT
#define VBBA_TCP_INFLIGHT 2800u        /* bytes in flight toward the guest (2 segments; the BBA ring holds ~3840 B) */
#endif
#ifndef VBBA_TCP_RTO_MS
#define VBBA_TCP_RTO_MS 300u
#endif
#define VBBA_TCP_RTO_MAX_MS 3000u
#define VBBA_TCP_TRIES 12u             /* retransmissions without progress before the connection is reset */
#define VBBA_TCP_SYNACK_TRIES 6u       /* SYN-ACK repeats (~10 s) before an unanswered connection is dropped */
#define VBBA_TCP_CONNECT_MS 15000u     /* platform connect deadline */
#define VBBA_TCP_IDLE_MS 600000u       /* an established connection with no segment from the guest for 10 min is reset (a guest that restarted) */
#define VBBA_TCP_EVICT_MS 30000u       /* a new SYN with every slot taken may take the slot of a connection idle this long */
#define VBBA_TCP_LINGER_MS 10000u      /* half-closed / closing states give up after this */

enum { VTC_FREE = 0, VTC_CONNECTING, VTC_SYNACK, VTC_EST, VTC_RST };

struct vbba_tcp_conn {
    vbba_u8 st, ack_due, synack_due, guest_fin, sock_eof, fin_sent, fin_acked, shut;
    vbba_u16 gport, rport, mss;
    vbba_u32 rip;
    int h;
    vbba_u32 irs, rcv_nxt, wnd_adv;                 /* guest -> router */
    vbba_u32 iss, snd_una, snd_nxt, snd_max, snd_wnd;  /* router -> guest; snd_una = sequence of tg[tg_head] */
    vbba_u32 t_open, t_send, t_last, rto, tries;
    vbba_u32 tg_head, tg_n;                         /* socket -> guest bytes not yet ACKed by the guest */
    vbba_u32 ts_head, ts_n;                         /* guest -> socket bytes not yet taken by the socket */
    vbba_u8 tg[VBBA_TCP_BUF];
    vbba_u8 ts[VBBA_TCP_BUF];
};

struct vbba_tcp_stats {
    vbba_u32 syn, opened, open_fail, noslot, denied, closed, rst_tx, rst_rx, orphan_rst;
    vbba_u32 seg_rx, seg_tx, retrans, ooo, bytes_up, bytes_down, bad, sock_err, synack_rtx;
    vbba_u32 abort_linger, abort_tries, abort_connect, sock_eof, shutdowns, abort_idle, evicted;
};

struct vbba_tcp {
    struct vbba_tcp_conn c[VBBA_TCP_CONNS];
    vbba_u8 rr;
    vbba_u8 orst;                                   /* one RST for a segment that matched no connection */
    vbba_u16 orst_sp, orst_dp;
    vbba_u32 orst_ip, orst_seq, orst_ack;
    vbba_u8 orst_flags;
    vbba_u32 isn_ctr;
    struct vbba_tcp_stats st;
};

#endif
