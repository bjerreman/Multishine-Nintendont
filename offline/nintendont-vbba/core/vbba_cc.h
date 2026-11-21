/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_cc: the Connection check (Wi-Fi qualification run inside the Nintendont kernel).
 *
 * The PowerPC (the Melee menu overlay, later) writes a request into a small mailbox in MEM2; the ARM (Starlet) then sends a steady stream of
 * game-sized UDP datagrams (default 60 per second, 80 bytes, 45 s) through the SAME vbba_transport as the game's traffic (IOS sendto /
 * recvfrom on /dev/net/ip/top) to the server's echo endpoint, counts what comes back, and writes a compact result block and a verdict word.
 * Document:  (mailbox layout, packet format, UI text, record block 4).
 *
 * Portable core: C99, no libc, no heap, aligned 32-bit words only in the mailbox (the platform's publish hook does the stores).  All
 * state lives in struct vbba_cc.  vbba_cc_step is called by whoever drives the transport (the thread or main loop that polls the chip /
 * router), under the same mutual exclusion as the router: it touches the transport.
 */
#ifndef VBBA_CC_H
#define VBBA_CC_H

#include "vbba_types.h"
#include "vbba_transport.h"

#define VBBA_CC_ABI 1u
#define VBBA_CC_MAGIC 0x4343484Bu          /* 'CCHK' */

/* ---- mailbox: 28 words at ARM 0x13026600 (PPC uncached 0xD3026600), all 32-bit, big-endian.
   Cache rule (ARM926 write-back data cache, 32-byte lines): the words the ARM writes (0..23, three whole lines) share no cache line with the words the PPC writes (24..27, the
   line at 0x13026660, which the ARM only invalidates and reads).  A clean of an ARM line can then never write a stale copy of a PPC word back over the PPC's store. ---- */
#define VBBA_CC_WORDS 28u
#define VBBA_CC_ARM_WORDS 24u              /* words 0..23 are written by the ARM, 24..27 by the PPC */
#define VBBA_CC_BASE 0x13026600u           /* ARM address of word 0 (PPC 0xD3026600) */
enum {
    CC_W_MAGIC = 0,    /* ARM: 'CCHK' once the kernel has initialised the block (feature present) */
    CC_W_VER = 1,      /* ARM: layout version << 16 | size in words (VBBA_CC_ABI << 16 | 28) */
    CC_W_ACK = 2,      /* ARM: the REQ value the kernel has taken (equals REQ once the request was looked at, accepted or refused) */
    CC_W_VERDICT = 3,  /* ARM: VBBA_CC_V_* */
    CC_W_STATE = 4,    /* ARM: bits 31..24 phase (VBBA_CC_P_*), 23..16 reason (VBBA_CC_R_*), 15..8 progress 0..100, 7..0 link type (VBBA_CC_L_*) */
    CC_W_SEQ = 5,      /* ARM: seqlock of the result words 6..23: odd while they are being written, even and unchanged around a read = consistent */
    CC_R_SENT = 6,     /* datagrams sent (attempts) */
    CC_R_RECV = 7,     /* distinct valid echoes received */
    CC_R_LOST = 8,     /* sent - received */
    CC_R_OOODUP = 9,   /* hi16 out-of-order, lo16 duplicates */
    CC_R_P50P95 = 10,  /* hi16 RTT p50, lo16 RTT p95, in 0.1 ms (0xFFFF = 6553.5 ms or more) */
    CC_R_P99JIT = 11,  /* hi16 RTT p99, lo16 jitter (RFC 3550 style, 1/16 smoothing), 0.1 ms */
    CC_R_MINMAX = 12,  /* hi16 RTT min, lo16 RTT max, 0.1 ms */
    CC_R_GAPS = 13,    /* hi16 receive gaps >= 100 ms, lo16 receive gaps >= 250 ms (the second count is included in the first) */
    CC_R_LGAVG = 14,   /* hi16 longest receive gap in ms, lo16 RTT mean in 0.1 ms */
    CC_R_SENDTO = 15,  /* hi16 kernel-side sendto latency mean, lo16 max, 0.1 ms (IOS submit -> completion, all sendto of the transport during the run) */
    CC_R_ARM = 16,     /* hi16 ARM main-loop passes >= 5 ms during the run, lo16 passes >= 20 ms */
    CC_R_RUN = 17,     /* hi16 longest gap between two kernel steps in ms, lo16 run length in 0.1 s */
    CC_R_WS_MIN = 18,  /* Wi-Fi signal word (MEM1 0x31A0, format NOT verified): per-byte minimum over the run, bytes packed like the word */
    CC_R_WS_MEAN = 19, /* per-byte mean */
    CC_R_WS_LAST = 20, /* the newest raw word */
    CC_R_WS_N = 21,    /* hi16 signal samples, lo16 loss in 1/100 percent (0..10000) */
    CC_R_TXRX = 22,    /* hi16 sends the transport refused, lo16 send-schedule slips (a send later than 2 intervals) */
    CC_R_LATE = 23,    /* hi16 foreign / stale / malformed datagrams */
    CC_W_REQ = 24,     /* PPC: request token.  A value different from CC_W_ACK starts a test (write the other PPC words first, REQ last) */
    CC_W_CANCEL = 25,  /* PPC: cancel token.  A change (any new value) aborts a running test */
    CC_W_IP = 26,      /* PPC: server echo IPv4, host order */
    CC_W_PORT = 27     /* PPC: bits 15..0 UDP port (host order), 23..16 duration in s (0 = 45; 10..90), 31..24 datagram size in bytes (0 = 80; 32..200) */
};

enum { VBBA_CC_V_NOTRUN = 0, VBBA_CC_V_RUNNING = 1, VBBA_CC_V_GOOD = 2, VBBA_CC_V_OKAY = 3, VBBA_CC_V_POOR = 4, VBBA_CC_V_FAILED = 5 };
enum { VBBA_CC_P_IDLE = 0, VBBA_CC_P_OPEN = 1, VBBA_CC_P_RUN = 2, VBBA_CC_P_DRAIN = 3, VBBA_CC_P_DONE = 4 };
enum { VBBA_CC_R_NONE = 0, VBBA_CC_R_NONET = 1, VBBA_CC_R_NOSOCK = 2, VBBA_CC_R_BUSY = 3, VBBA_CC_R_NOECHO = 4, VBBA_CC_R_BADADDR = 5, VBBA_CC_R_CANCEL = 6, VBBA_CC_R_GAME = 7 };
enum { VBBA_CC_L_UNKNOWN = 0, VBBA_CC_L_WIRED = 1, VBBA_CC_L_WIFI = 2 };

/* ---- the verdict thresholds: ONE place.  PROVISIONAL, to be calibrated from server-side results ( 7) ---- */
#define VBBA_CC_GOOD_LOSS_X100 50u         /* Good: loss below 0.5 % (1/100 percent units) ... */
#define VBBA_CC_GOOD_P95_US 60000u         /* ... and RTT p95 below 60 ms ... */
#define VBBA_CC_GOOD_GAP100_MAX 0u         /* ... and no receive gap >= 100 ms */
#define VBBA_CC_OKAY_LOSS_X100 200u        /* Okay: loss below 2 % ... */
#define VBBA_CC_OKAY_P95_US 100000u        /* ... and RTT p95 below 100 ms ... */
#define VBBA_CC_OKAY_GAP250_MAX 0u         /* ... and no receive gap >= 250 ms; anything else is Poor */
#define VBBA_CC_MIN_SENT_PCT 90u           /* fewer sends than this share of the schedule (the kernel could not keep the cadence) is Poor at best */

/* ---- run parameters ---- */
#define VBBA_CC_DURATION_S 45u
#define VBBA_CC_DURATION_MIN_S 10u
#define VBBA_CC_DURATION_MAX_S 90u
#define VBBA_CC_INTERVAL_US 16667u         /* 60 per second */
#define VBBA_CC_SIZE_DEFAULT 80u
#define VBBA_CC_SIZE_MIN 32u
#define VBBA_CC_SIZE_MAX 200u
#define VBBA_CC_DRAIN_US 1500000u          /* after the last send, echoes are still accepted this long */
#define VBBA_CC_OPEN_TIMEOUT_US 5000000u
#define VBBA_CC_BUSY_TX_PER_S 20u          /* more guest UDP datagrams per second than this = a match (or the lobby) is going on: refuse / stop */
#define VBBA_CC_SEQ_MAX 5400u              /* 90 s at 60 per second */
#define VBBA_CC_HIST_N 120u

/* ---- packet format (request and reply have the same length; the server changes byte 5 and bytes 20..23 only) ---- */
#define VBBA_CC_HDR 28u
#define VBBA_CC_PKT_MAGIC 0x4D534543u      /* 'MSEC' at bytes 0..3 */
#define VBBA_CC_PKT_VER 1u

/* ---- platform hooks (the Nintendont glue fills them; the host test supplies a simulator) ---- */
struct vbba_cc_txlat { vbba_u32 n, sum_us, max_us; };
struct vbba_cc_hooks {
    void *ctx;
    vbba_u32 (*now_us)(void *ctx);                                      /* wrapping microsecond clock */
    void (*mb_fetch)(void *ctx, vbba_u32 first, vbba_u32 n, vbba_u32 *dst);   /* read mailbox words first..first+n-1 (the platform invalidates its cache first) */
    void (*mb_publish)(void *ctx, vbba_u32 first, vbba_u32 n, const vbba_u32 *src);   /* write words first..first+n-1 with aligned 32-bit stores and flush */
    vbba_u32 (*guest_tx)(void *ctx);                                    /* running count of UDP datagrams the guest (the game) sent */
    void (*txlat)(void *ctx, struct vbba_cc_txlat *o, int reset_max);   /* the transport's sendto latency counters; reset_max: zero the running max after reading it */
    void (*txlat_restore)(void *ctx, vbba_u32 old_max_us);              /* put max(old, run max) back so the game's own statistic is not lost */
    void (*arm_passes)(void *ctx, vbba_u32 *p5, vbba_u32 *p20);         /* running counts of ARM main-loop passes >= 5 ms / >= 20 ms */
    vbba_u32 (*link_type)(void *ctx);                                   /* VBBA_CC_L_* */
    vbba_u32 (*wsig)(void *ctx);                                        /* the raw Wi-Fi signal word (only asked for when link_type is Wi-Fi) */
};

struct vbba_cc_result {
    vbba_u32 sent, recv, lost, ooo, dup, p50, p95, p99, jitter, rtt_min, rtt_max, rtt_mean;   /* times in microseconds */
    vbba_u32 gap100, gap250, gap_max_ms, sendto_mean, sendto_max, arm5, arm20, step_gap_max_ms, run_ds;
    vbba_u32 ws_min, ws_mean, ws_last, ws_n, loss_x100, tx_refused, slips, late;
};

struct vbba_cc {
    struct vbba_cc_hooks h;
    const struct vbba_transport *tp;
    vbba_u32 mb[VBBA_CC_WORDS];        /* shadow of the mailbox */
    vbba_u32 ack, cancel_seen, verdict, phase, reason, progress, seq;
    vbba_u32 nextpoll_us, tg_a_us, tg_a_n, tg_b_us, tg_b_n;      /* guest tx history: counter value ~1 s ago */
    int handle;
    /* run */
    vbba_u32 ip, session, duration_us, size, link;
    vbba_u32 open_us, t0_us, next_send_us, last_step_us, last_rx_us, last_prog_us, last_ws_us, hi_seq, prev_rtt;
    vbba_u32 rtt_sum_hi, rtt_sum_lo;   /* 64-bit sum of RTTs in microseconds (two words) */
    vbba_u32 ws_min[4], ws_sum[4], ws_n, ws_last;
    vbba_u32 txlat_old_max, tx0_n, tx0_sum, a0_5, a0_20;
    vbba_u16 port;
    vbba_u8 seen[(VBBA_CC_SEQ_MAX + 7u) / 8u];
    vbba_u16 hist[VBBA_CC_HIST_N];
    struct vbba_cc_result r;
    vbba_u8 pkt[VBBA_CC_SIZE_MAX];
};

void vbba_cc_init(struct vbba_cc *c, const struct vbba_cc_hooks *h, const struct vbba_transport *tp);
void vbba_cc_step(struct vbba_cc *c);                                   /* never blocks; cheap while idle */
int vbba_cc_running(const struct vbba_cc *c);
const struct vbba_cc_result *vbba_cc_result(const struct vbba_cc *c);   /* of the last finished run (zero before the first) */
vbba_u32 vbba_cc_verdict_of(const struct vbba_cc_result *r, vbba_u32 expected_sends);   /* pure: VBBA_CC_V_GOOD / OKAY / POOR / FAILED from the numbers */
vbba_u32 vbba_cc_hist_pct(const vbba_u16 *hist, vbba_u32 n, vbba_u32 pct, vbba_u32 vmin, vbba_u32 vmax);   /* percentile (1..100) of the RTT histogram in microseconds */
vbba_u32 vbba_cc_hist_idx(vbba_u32 rtt_us);
void vbba_cc_pkt_build(vbba_u8 *p, vbba_u32 len, vbba_u32 session, vbba_u32 seq, vbba_u32 t_us);
int vbba_cc_pkt_parse(const vbba_u8 *p, vbba_u32 len, vbba_u32 *session, vbba_u32 *seq, vbba_u32 *t_us, vbba_u32 *srv_us);   /* 1 = a valid reply */

#endif
