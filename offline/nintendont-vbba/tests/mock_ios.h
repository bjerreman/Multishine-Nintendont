/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* mock_ios: the vbba_ios.h hooks for host tests.  MOCK_MEM: an in-memory network (clock driven by the test, latency,
   loss, an echo service, unsolicited injection, faults).  MOCK_UDP: real non-blocking sockets on 127.0.0.1. */
#ifndef MOCK_IOS_H
#define MOCK_IOS_H

#include "../platform/nintendont/vbba_ios.h"

#define MOCK_MEM 0
#define MOCK_UDP 1
#define MOCK_FDS 16
#define MOCK_RXQ 32
#define MOCK_PEND 64

struct mock_dgram { vbba_u32 ip; vbba_u16 port; vbba_u16 len; vbba_u8 d[2048]; };
struct mock_sock { int used, osfd, nb; vbba_u32 tos; vbba_u16 port; int head, cnt; struct mock_dgram q[MOCK_RXQ]; };   /* nb: made non-blocking by the fcntl op (a receive without data then fails at once with -11) */

struct mock_ios {
    struct vbba_ios_ops ops;
    int mode;
    vbba_u32 now;                     /* MOCK_MEM clock (ms), advanced by the test */
    vbba_u32 ip;                      /* our address once "DHCP" finished */
    vbba_u32 dhcp_ms;                 /* GETIP returns 0 before this time */
    vbba_u32 latency_ms;              /* completion delay of every request */
    int fail_startup, fail_socket, port_taken, loss_pct;
    int drop_first;                   /* MOCK_MEM: silently drop the next N sends (retry tests) */
    vbba_u32 echo_ip;                 /* MOCK_MEM: datagrams to echo_ip:7 come back from it */
    int echo_copies;                  /* extra copies of every echo */
    int echo_stamp;                   /* the echo server stamps 64-byte 'BR' datagrams whose byte 15 is 1: arrival index and time */
    unsigned stamp_cnt; vbba_u32 stamp_first_ms, stamp_last_ms;
    vbba_u32 rng;
    void (*tx_hook)(vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u32 len);   /* every accepted sendto (MOCK_MEM), optional */
    struct vbba_ios_req *pend[MOCK_PEND];
    vbba_u32 due[MOCK_PEND];
    vbba_u32 sub[MOCK_PEND];          /* submit time of each pending request (lat_us / dwell_us of the request) */
    vbba_u32 send_latency_ms;         /* extra completion delay of sendto requests only (a slow IOS sendto), on top of latency_ms */
    int refuse_send;                  /* the next N sendto submissions are refused (IOS_IoctlvAsync < 0: the transport must keep the datagram and retry) */
    int fail_poll;                    /* every poll request fails with -1 (b18a: the receive side must still work through the non-blocking recvfrom) */
    int rx_rev;                       /* completions of two receives that finish in the same poll come back in reverse order (replies reordered in IPC; datagram assignment stays first come first served) */
    struct vbba_ios_req *stash, *stash_out;
    vbba_u32 startup_latency_ms;      /* extra completion delay of the start-up request (kd/request 6 takes ~20 s on our Wii) */
    int npend;
    struct mock_sock s[MOCK_FDS];
    unsigned submitted, completed, sent, echoed, lost, refused, polls, recv_submitted, net_pend_max;   /* net_pend_max: most sendto/poll/recvfrom requests pending at once (libogc: 1) */
    unsigned char last_tx[2048]; unsigned last_tx_len; vbba_u32 last_tx_ip; vbba_u16 last_tx_port;
    unsigned tcp_ops, tcp_chunk;          /* VBBA_TCP: stream requests served; at most tcp_chunk bytes per send/recv (0 = no limit) */
};

void mock_ios_init(struct mock_ios *m, int mode, vbba_u32 ip);
/* MOCK_MEM: deliver a datagram from ip:port to whichever socket is bound to dport (returns 0 if none) */
int mock_ios_inject(struct mock_ios *m, vbba_u16 dport, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u32 len);
void mock_ios_close_all(struct mock_ios *m);

#endif
