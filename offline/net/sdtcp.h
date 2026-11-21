/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SDTCP_H
#define SDTCP_H

/*
 * sdtcp: small TCP for the console, on top of sdnet.
 * Strict C89, no heap, no libc.  Non-blocking, single-threaded, driven from sdnet_poll (sdnet calls sdtcp_rx / sdtcp_poll
 * when built with -DSDNET_ENABLE_TCP=1 and sdtcp_init has attached a context).
 *
 * Scope: active open (client) is the product; passive open (SDTCP_LISTEN=1) exists for the host test rig only.
 * Reno/NewReno congestion control, RFC 6298 RTO with Karn, fast retransmit/recovery, delayed ACK, receive-side SACK
 * block + one out-of-order interval buffered inside the ring, zero-window persist, optional keepalive, RFC 5961 RST
 * and SYN hardening, keyed-hash ISN.  No window scaling (window <= 64 KB), no timestamps, no urgent data, no
 * sender-side SACK processing, no ECN, IPv4 only, no fragments (sdnet drops them).
 *
 * Memory is owned by the caller: a connection is struct sdtcp_conn plus an rx ring and a tx ring of any power-of-two size
 * (sdtcp_conn_init).  Bytes are stored at ring[seq & (size-1)], so out-of-order data lands where it belongs.
 */

#include "sdnet.h"

#ifndef SDTCP_MSS                 /* largest segment payload we offer (BBA/Ethernet MTU 1500 - 40); use sdtcp_set_mss for WAN */
#define SDTCP_MSS 1460
#endif
#ifndef SDTCP_TX_BUDGET           /* segments sent per sdtcp_poll (frame-time slice); API calls spend from the same pool */
#ifdef SDNET_CONSOLE
#define SDTCP_TX_BUDGET 2
#else
#define SDTCP_TX_BUDGET 16
#endif
#endif
#ifndef SDTCP_RTO_INIT_MS
#define SDTCP_RTO_INIT_MS 1000u
#endif
#ifndef SDTCP_RTO_MIN_MS
#define SDTCP_RTO_MIN_MS 200u
#endif
#ifndef SDTCP_RTO_MAX_MS
#define SDTCP_RTO_MAX_MS 8000u
#endif
#ifndef SDTCP_SYN_TRIES           /* SYN transmissions before ETIMEDOUT (1,2,4,8,8 s ...) */
#define SDTCP_SYN_TRIES 5
#endif
#ifndef SDTCP_DATA_TRIES          /* consecutive RTOs before ETIMEDOUT */
#define SDTCP_DATA_TRIES 8
#endif
#ifndef SDTCP_DELACK_MS
#define SDTCP_DELACK_MS 40u
#endif
#ifndef SDTCP_TW_MS               /* TIME_WAIT (shortened: the console reconnects with a fresh random port and ISN) */
#define SDTCP_TW_MS 2000u
#endif
#ifndef SDTCP_FW2_MS              /* FIN_WAIT_2 / LAST_ACK / CLOSING give-up */
#define SDTCP_FW2_MS 10000u
#endif
#ifndef SDTCP_CONNECT_MS          /* whole-connect deadline */
#define SDTCP_CONNECT_MS 30000u
#endif
#ifndef SDTCP_KEEP_PROBES
#define SDTCP_KEEP_PROBES 3
#endif
#ifndef SDTCP_LISTEN
#define SDTCP_LISTEN 0
#endif

/* ---- errors (negative returns) ---- */
#define SDTCP_E_PARAM (-1)
#define SDTCP_E_NOSLOT (-2)
#define SDTCP_E_STATE (-3)
#define SDTCP_E_RESET (-4)        /* peer sent RST */
#define SDTCP_E_TIMEOUT (-5)      /* retransmission or keepalive gave up */
#define SDTCP_E_REFUSED (-6)      /* RST in answer to our SYN */
#define SDTCP_E_EOF (-7)          /* recv: peer closed and everything was read */
#define SDTCP_E_NET (-8)          /* no route / not configured */

/* ---- states ---- */
#define SDTCP_CLOSED 0
#define SDTCP_LISTEN_ST 1
#define SDTCP_SYN_SENT 2
#define SDTCP_SYN_RCVD 3
#define SDTCP_ESTABLISHED 4
#define SDTCP_FIN_WAIT_1 5
#define SDTCP_FIN_WAIT_2 6
#define SDTCP_CLOSE_WAIT 7
#define SDTCP_CLOSING 8
#define SDTCP_LAST_ACK 9
#define SDTCP_TIME_WAIT 10

/* ---- run modes (rollback interplay) ---- */
#define SDTCP_RUN 0               /* normal */
#define SDTCP_QUIESCE 1           /* advertise window 0, send no new data; ACKs and retransmits still flow */
#define SDTCP_PAUSED 2            /* every TCP segment dropped on entry, no timers; resume rebases timers */

typedef sdnet_u8 sdtcp_u8;
typedef sdnet_u16 sdtcp_u16;
typedef sdnet_u32 sdtcp_u32;

struct sdtcp_conn {
    sdtcp_u8 state, err_flag, dupacks, retries, keep_cnt, pad8;
    sdtcp_u16 flags;
    sdtcp_u16 lport, rport, mss, pad16;
    sdtcp_u32 rip;
    sdtcp_u32 iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, wr_seq;
    sdtcp_u32 irs, rcv_nxt, rd_seq, ooo_lo, ooo_hi, rcv_adv;
    sdtcp_u32 cwnd, ssthresh, recover;
    sdtcp_u32 srtt, rttvar, rto, rtt_seq, rtt_t0;      /* srtt, rttvar in 1/8 ms */
    sdtcp_u32 t_rto, t_ack, t_persist, t_keep, t_tw, t_last_rx, t_last_tx;
    sdtcp_u32 keep_ms;                                  /* 0 = off */
    sdtcp_u32 unacked_bytes;                            /* received in order, not yet ACKed */
    sdtcp_u8 *rxbuf, *txbuf;
    sdtcp_u32 rxmask, txmask;                           /* size - 1 */
    int err;                                            /* SDTCP_E_* once failed (sticky) */
};

struct sdtcp_stats {
    sdtcp_u32 seg_tx, seg_rx, data_tx_bytes, data_rx_bytes;
    sdtcp_u32 retrans, fast_retrans, rto_fires, dup_acks_rx, ooo_stored, ooo_dropped;
    sdtcp_u32 bad_csum, bad_hdr, rst_tx, rst_rx, challenge_acks, paused_drop, no_conn, arp_wait;
};

struct sdtcp {
    struct sdnet *net;
    struct sdtcp_conn *conn;
    int nconn;
    sdtcp_u32 secret[4];          /* ISN / port keying material */
    sdtcp_u32 ctr;
    sdtcp_u8 mode;
    sdtcp_u32 pause_t0;
    sdtcp_u32 last_rst;
    sdtcp_u16 mss_adv;
    sdtcp_u32 wnd_cap;            /* advertised window never exceeds this (0xffff = no cap); see sdtcp_set_window_cap */
    int budget;                   /* segments left in this slice */
#ifdef SDTCP_TEST_HOOKS
    sdtcp_u32 test_isn;
    sdtcp_u8 test_isn_on;
#endif
    struct sdtcp_stats st;
};

/* conn memory: rx/tx sizes must be powers of two (2 KB..32 KB each); returns 0 or SDTCP_E_PARAM */
int sdtcp_conn_init(struct sdtcp_conn *c, void *rx, sdtcp_u32 rxlen, void *tx, sdtcp_u32 txlen);
/* attach to the stack (sets net->tcp).  seed: 16 bytes of the best entropy available. */
int sdtcp_init(struct sdtcp *t, struct sdnet *net, struct sdtcp_conn *conns, int nconns, const sdtcp_u8 seed[16]);
void sdtcp_set_mss(struct sdtcp *t, sdtcp_u16 mss);
void sdtcp_set_mode(struct sdtcp *t, int mode);
/* Cap the receive window we advertise.  The BBA's receive ring holds about 2 full frames (3840 bytes): when the console cannot poll the NIC every
   1-2 ms, a window above ~2 segments invites bursts the chip drops.  Default: no cap. */
void sdtcp_set_window_cap(struct sdtcp *t, sdtcp_u32 cap);

/* client API; handle = index.  connect returns the handle or a negative error (ARP is retried internally). */
int sdtcp_connect(struct sdtcp *t, sdtcp_u32 ip, sdtcp_u16 port);
int sdtcp_state(const struct sdtcp *t, int h);
int sdtcp_error(const struct sdtcp *t, int h);          /* 0 or sticky SDTCP_E_* */
int sdtcp_established(const struct sdtcp *t, int h);    /* 1 when data can flow either way */
int sdtcp_send(struct sdtcp *t, int h, const void *buf, sdtcp_u32 len);   /* bytes queued (may be < len, 0 = full) or error */
int sdtcp_recv(struct sdtcp *t, int h, void *buf, sdtcp_u32 max);         /* bytes, 0 nothing yet, SDTCP_E_EOF, or error */
sdtcp_u32 sdtcp_rx_avail(const struct sdtcp *t, int h);
sdtcp_u32 sdtcp_tx_free(const struct sdtcp *t, int h);
sdtcp_u32 sdtcp_tx_pending(const struct sdtcp *t, int h);   /* queued, not yet acknowledged */
int sdtcp_close(struct sdtcp *t, int h);                    /* graceful: FIN after the queued data */
void sdtcp_abort(struct sdtcp *t, int h);                   /* RST + free */
void sdtcp_set_keepalive(struct sdtcp *t, int h, sdtcp_u32 idle_ms);
#if SDTCP_LISTEN
int sdtcp_listen(struct sdtcp *t, sdtcp_u16 port);          /* returns the listener handle */
int sdtcp_accept(struct sdtcp *t, int lh);                  /* handle of a newly established child, or -1 */
#endif

#ifdef SDTCP_TEST_HOOKS
void sdtcp_test_set_iw(sdtcp_u32 segs);    /* host rig: initial window in segments for every connection (Linux servers use 10); 0 = RFC 5681 */
#endif

/* called by sdnet */
void sdtcp_rx(struct sdtcp *t, sdtcp_u32 src, sdtcp_u32 dst, const sdtcp_u8 *seg, sdtcp_u32 len);
void sdtcp_poll(struct sdtcp *t);

#endif
