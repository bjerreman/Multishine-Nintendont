/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Connection check, the pure part: the statistics, the UDP DNS probe packet and the verdict, for a caller that owns the sockets (the Nintendont loader on the PowerPC, libogc).
 * No sockets, no clock of its own, no heap, no libc beyond nothing (memcpy/memset are not even needed), no 64-bit arithmetic.  The caller passes the time in microseconds (any wrapping
 * 32-bit clock) with every call.  Tested by tests/probe_test.c (ASan + UBSan, strict C89-ish compile).
 *
 * What it measures: the player's OWN link, independent of where our server is.  A ladder of legs, each one UDP DNS queries for one fixed harmless name ("localhost", type A) to:
 *   ROUTER    the real default gateway, port 53 (only when the caller knows it and it is a private address),
 *   RESOLVER  the DHCP resolver, only when it is a different private address on the console's /24,
 *   CF        1.1.1.1 (Cloudflare anycast: the nearest point of presence for every player),
 *   GOOGLE    8.8.8.8, only when 1.1.1.1 gave nothing.
 * A leg that gets no reply in LEG_WAIT_US gives way to the next one.  The first leg that answers runs the whole duration and decides the verdict.  Any DNS response counts
 * (answer, NXDOMAIN, SERVFAIL, REFUSED): only the round trip matters.  Nothing else is ever sent and no address but the four above is ever probed.
 *
 * Calling pattern (the caller's loop, one thread):
 *   ms_probe_init(&L, now, seed, 0, 0); ms_probe_default_legs(&L, own_ip, gateway_or_0, dns_or_0);
 *   for (;;) { act = ms_probe_poll(&L, now);
 *       if (act == MSP_ACT_SEND) { n = ms_probe_make_query(&L, buf, now, &ip, &port); sendto(...);  ms_probe_note_call(&L, call_duration_us); }
 *       else if (act == MSP_ACT_DONE) break;
 *       while (recvfrom(...) gave a datagram) ms_probe_on_reply(&L, buf, len, from_ip, from_port, now);  }
 *   ms_probe_result(&L, &res);
 */
#ifndef MS_PROBE_CORE_H
#define MS_PROBE_CORE_H

typedef unsigned char msp_u8;
typedef unsigned short msp_u16;
typedef unsigned int msp_u32;     /* 32 bits on every target of this project (PowerPC, ARM, the host test checks it) */
typedef int msp_s32;

enum { MSP_LEG_NONE = 0, MSP_LEG_ROUTER = 1, MSP_LEG_RESOLVER = 2, MSP_LEG_CF = 3, MSP_LEG_GOOGLE = 4 };
enum { MSP_V_NOTRUN = 0, MSP_V_RUNNING = 1, MSP_V_GOOD = 2, MSP_V_OKAY = 3, MSP_V_POOR = 4, MSP_V_FAILED = 5 };   /* same numbers as the kernel mailbox verdict word */
enum { MSP_R_NONE = 0, MSP_R_NOLEG = 1, MSP_R_NOANSWER = 2 };   /* NOANSWER: no leg answered (verdict Failed); the router leg's silence alone is res.router_silent */
enum { MSP_ACT_WAIT = 0, MSP_ACT_SEND = 1, MSP_ACT_DONE = 2 };

#define MSP_MAX_LEGS 4u
#define MSP_DNS_LEN 27u
#define MSP_RING 128u                 /* send times kept for matching (a DNS reply carries no time) */
#define MSP_SEQ_MAX 5400u             /* per leg */
#define MSP_HIST_N 120u               /* 1 ms buckets to 100 ms, then 20 coarse ones to 3 s */
#define MSP_SEEN_BYTES ((MSP_SEQ_MAX + 7u) / 8u)

/* defaults (the caller may pass its own to ms_probe_init) */
#define MSP_DURATION_US 20000000u     /* 20 s on the deciding leg: a loader has time, the player is watching a progress bar */
#define MSP_INTERVAL_US 50000u        /* 20 probes a second: IOS sendto/recvfrom on the PowerPC can take 100+ ms per call, so 60 a second is not sustainable there */
#define MSP_LEG_WAIT_US 3000000u      /* a leg with no reply this long after its first send gives way */
#define MSP_DRAIN_US 1500000u         /* after the last send, replies are still accepted this long */
#define MSP_SLOW_CALL_US 50000u       /* a sendto/recvfrom call this slow is IOS noise, not the link (note_call) */
#define MSP_MIN_SENT_PCT 85u          /* fewer sends than this share of the schedule (the caller could not keep the cadence): Poor at best */

/* provisional thresholds, justified   Loss in 1/100 percent, times in microseconds. */
#define MSP_H_GOOD_LOSS 100u          /* one LAN hop (router / resolver): Good below 1 % ... */
#define MSP_H_GOOD_P95 25000u         /* ... p95 below 25 ms (one game frame is 16.7 ms; home Wi-Fi is 2 to 10 ms) ... */
#define MSP_H_OKAY_LOSS 300u          /* Okay below 3 % ... */
#define MSP_H_OKAY_P95 100000u        /* ... p95 below 100 ms (was 60: the 12 h Wi-Fi soak of sat at 58-90 ms and held the link; policy: permissive, Poor only for a sustained problem) */
#define MSP_N_GOOD_LOSS 100u          /* anycast Internet leg: the RTT contains the access network and the provider, so wider, and spread counts */
#define MSP_N_GOOD_P95 80000u
#define MSP_N_GOOD_SPREAD 30000u      /* p95 - p50 */
#define MSP_N_OKAY_LOSS 300u
#define MSP_N_OKAY_P95 200000u
#define MSP_N_OKAY_SPREAD 100000u
#define MSP_GOOD_GAP 100000u          /* no receive gap >= 100 ms for Good, at most MSP_OKAY_GAPS_MAX of >= 250 ms for Okay */
#define MSP_OKAY_GAP 250000u
#define MSP_OKAY_GAPS_MAX 1u          /* Okay forgives ONE silence of 250 ms or more in the check (one spike is not a verdict); two or more = Poor */
#define MSP_NOISY_PCT 5u              /* more than this share of probes with a slow socket call: the run is "noisy" (see ms_probe_verdict) */

struct msp_result {
    msp_u32 verdict;                  /* MSP_V_* */
    msp_u32 reason;                   /* MSP_R_* */
    msp_u32 leg;                      /* MSP_LEG_*: the leg that decided (0 if none answered) */
    msp_u32 leg_ip;                   /* its address */
    msp_u32 router_silent;            /* 1: a ROUTER leg was tried and never answered (the player may be told "your router does not answer DNS") */
    msp_u32 noisy;                    /* 1: slow socket calls made absolute times and gaps unreliable; the verdict used loss only and is capped at Okay */
    msp_u32 sent, recv, lost, loss_x100, dup, ooo, late;
    msp_u32 rtt_min, rtt_max, p50, p95, p99, jitter;                /* microseconds; jitter RFC 3550 */
    msp_u32 gap100, gap250, gap_max_ms;
    msp_u32 slow_calls, call_max_us, send_fail;
    msp_u32 run_ms;                   /* time since the deciding leg's first send */
};

struct msp_stats {                    /* the statistics of one leg */
    msp_u32 sent, recv, dup, ooo, late, hi_seq, prev_rtt, jitter, rtt_min, rtt_max, rtt_mean;
    msp_u32 gap100, gap250, gap_max_ms, last_rx_us, first_tx_us, slow_calls, call_max_us, send_fail;
    msp_u8 seen[MSP_SEEN_BYTES];
    msp_u16 hist[MSP_HIST_N];
    msp_u32 sent_us[MSP_RING];
};

struct msp_ladder {
    msp_u32 nleg, legi, leg_ip[MSP_MAX_LEGS], leg_id[MSP_MAX_LEGS];
    msp_u32 duration_us, interval_us, session, seed, started, finished, t_leg_us, next_send_us, last_send_us, router_silent;
    msp_u32 verdict, reason;
    struct msp_stats s;
    struct msp_result res;
};

int msp_is_private(msp_u32 ip);       /* 10/8, 172.16/12, 192.168/16, 169.254/16 */
msp_u32 msp_dns_build(msp_u8 *p, msp_u32 txid);                 /* the 27 byte query; returns its length */
int msp_dns_parse(const msp_u8 *p, msp_u32 len, msp_u32 *txid); /* 1: a response (QR set, opcode 0) that repeats our question; any rcode */
msp_u32 msp_hist_idx(msp_u32 rtt_us);
msp_u32 msp_hist_pct(const msp_u16 *hist, msp_u32 n, msp_u32 pct, msp_u32 vmin, msp_u32 vmax);

/* pure verdict rules.  expected = the number of probes the schedule would have sent in the time the leg ran.  noisy: see struct msp_result. */
msp_u32 msp_verdict_hop(const struct msp_result *r, msp_u32 expected);        /* router / resolver legs */
msp_u32 msp_verdict_internet(const struct msp_result *r, msp_u32 expected);   /* CF / GOOGLE legs */
msp_u32 msp_verdict(const struct msp_result *r, msp_u32 leg, msp_u32 expected);   /* dispatches on the leg and applies the noisy rule */

void msp_init(struct msp_ladder *L, msp_u32 now_us, msp_u32 seed, msp_u32 duration_us, msp_u32 interval_us);   /* 0 = the defaults */
int msp_add_leg(struct msp_ladder *L, msp_u32 leg, msp_u32 ip);                 /* appended in call order; 0 = refused (full, or a non-private ROUTER/RESOLVER address) */
msp_u32 msp_default_legs(struct msp_ladder *L, msp_u32 own_ip, msp_u32 gateway, msp_u32 dns);   /* gateway / dns 0 = unknown.  Adds ROUTER (private, not own), RESOLVER (private, own /24, not the gateway), CF, GOOGLE; returns the number of legs */
int msp_poll(struct msp_ladder *L, msp_u32 now_us);                             /* MSP_ACT_*; advances legs and finishes the run */
msp_u32 msp_make_query(struct msp_ladder *L, msp_u8 *buf, msp_u32 now_us, msp_u32 *ip, msp_u32 *port);   /* buf >= 27 bytes; call only after poll returned SEND; counts the send */
void msp_note_call(struct msp_ladder *L, msp_u32 call_us);                      /* duration of a socket call; slow ones mark the run noisy */
void msp_note_send_fail(struct msp_ladder *L);                                  /* the send call itself failed (counted; the probe stays counted as sent and lost) */
int msp_on_reply(struct msp_ladder *L, const msp_u8 *buf, msp_u32 len, msp_u32 from_ip, msp_u32 from_port, msp_u32 now_us);   /* 1 = accepted as a new reply of the running leg */
void msp_finish_now(struct msp_ladder *L, msp_u32 now_us);                 /* the caller's hard time limit: ends the run now (an answering leg gets its verdict from what was measured, otherwise Failed / NOANSWER) */
int msp_done(const struct msp_ladder *L);
void msp_result(const struct msp_ladder *L, struct msp_result *out);            /* a snapshot (running: verdict RUNNING with the numbers so far) */


/* Should the check run at all?  Decided from the console's active connection.  Pure, no I/O.
 *   kind: MSP_NET_UNKNOWN / MSP_NET_WIRELESS / MSP_NET_WIRED  (the connection profile the game will use: Nintendont's Network Profile 1-3, or on Auto the Wii's selected one)
 *   adapter: MSP_NET_UNKNOWN / 0 (none) / 1 (a USB LAN adapter is plugged in)
 * Skipped ONLY when the active connection is known to be wired.  Anything else runs (never skip on a guess; an adapter alone never switches it off: the profile may be Wi-Fi).
 * *hint = 1 when it runs although an adapter is plugged in and the profile is known to be Wi-Fi (the screen then says so).  Returns 1 = run. */
enum { MSP_NET_UNKNOWN = -1, MSP_NET_WIRELESS = 0, MSP_NET_WIRED = 1 };
int msp_should_run(int kind, int adapter, int *hint);

/* The Wii's stored network config (ncd GetConfig, ioctlv 3: 0x1B5C bytes; layout of config.dat, see ota/ms_ota_loader.c and patches/0017): byte 4 = the interface IOS brings up (1 wireless, 2 wired),
 * three connection entries of 0x91C bytes from offset 8, entry byte 0 flags: 0x80 selected, 0x01 wired.  profile 1..3 = Nintendont's Network Profile row (that entry), 0 = Auto (the selected one).
 * Returns MSP_NET_* ; unknown when the buffer is short or nothing is consistent.  *link (may be 0) = the kind IOS brings up (header byte), for the log. */
int msp_cfg_kind(const msp_u8 *cfg, msp_u32 len, msp_u32 profile, int *link);

#endif
