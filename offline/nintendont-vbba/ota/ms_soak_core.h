/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Wi-Fi SOAK mode, the pure part (the loop, the sockets and the screen are in ms_ota_loader.c; tests/soak_test.c + tests/test_soak.py).
 * A time series, not a verdict: every 10 s a burst of SK_BURST_N probes (50 ms apart) to three legs (router guess, 1.1.1.1, 8.8.8.8), every 60 s a TLS GET and a network-state line,
 * events (drop, up, restart, watchdog) as they happen, and every 5 minutes the queued lines go to the server inside the existing opt-in diagnostics body (MSDIAG1, POST /v1/update/diag/shell).
 * Nothing here knows a Wi-Fi name, a password or a MAC.
 *
 * WIRE / CSV LINE:
 *   MSK1,S,<session>,<seq>,<up_s>,<wall>,<net_st>,<drops>,<restarts>,<ul_ok>,<ul_fail>,<sd_fail>, then per leg R, C, G: sent,recv,min,p50,p95,p99,max,jitter,gap_ms   (RTTs in microseconds)
 *   MSK1,T,<session>,<seq>,<up_s>,<wall>,<ok>,<total_ms>,<conn_ms>,<hs_ms>,<http>,<why>        TLS GET of the update 'latest' endpoint
 *   MSK1,N,<session>,<seq>,<up_s>,<wall>,<net_st>,<link_rc>,<link_word>,<drops>,<restarts>,<blackouts>,<wd>,<down_total_s>,<down_max_s>,<tls_ok>,<tls_fail>,<ul_ok>,<ul_fail>,<q_drop>,<sd_fail>
 *   MSK1,E,<session>,<seq>,<up_s>,<wall>,<type>,<a>,<b>    type: start stop drop up blackout restart wd   (a, b: see the caller)
 *   MSK1,U,<session>,<seq>,<up_s>,<wall>,<ok>,<total_ms>,<conn_ms>,<hs_ms>,<http>,<bytes>,<lines>,<skipped>,<why>   the upload itself
 * seq counts every line of the session (one counter), so a gap in what reached the server is measurable. */
#ifndef MS_SOAK_CORE_H
#define MS_SOAK_CORE_H

typedef unsigned char sk_u8;
typedef unsigned int sk_u32;

#define SK_VER "MSK1"
#define SK_LEGS 3u
#define SK_BURST_N 20u                       /* probes per leg per burst, 50 ms apart */
#define SK_BURST_GAP_US 50000u
#define SK_PERIOD_MS 10000u
#define SK_MINUTE_MS 60000u
#define SK_UPLOAD_MS 300000u                 /* one POST every 5 minutes (server: KEEP_N 200 stored files, 12 per 10 minutes per address) */
#define SK_UPLOAD_FIRST_MS 45000u            /* the first one early, so the first night's path is proven within a minute */
#define SK_WATCHDOG_MS 60000u
#define SK_RESTART_AFTER_BAD 3u              /* silent bursts in a row (all legs, network "up") before the stack is restarted */
#define SK_RESTART_GAP_MS 5000u
#define SK_CAP_S (12u * 3600u)
#define SK_ROTATE_BYTES 2097152u
#define SK_LINE_MAX 400u                     /* the server refuses a longer line */
#define SK_QMAX 360u
#define SK_BODY_MAX 15000u                   /* of the server's 16384: header, proof and slack */
#define SK_O_LINK 1u                         /* soak_on contains the word "link": also read ncd GetLinkStatus once a minute */
#define SK_O_NOUP 2u                         /* ... "noupload": SD log only */

/* ---- one burst: SK_BURST_N probes to one leg ---- */
struct sk_burst {
    sk_u32 ip, sent, recv, first_tx_us, last_tx_us;
    sk_u32 tx_us[SK_BURST_N], rx_us[SK_BURST_N];
    sk_u8 got[SK_BURST_N];
};
struct sk_stats { sk_u32 sent, recv, min, p50, p95, p99, max, jitter, gap_ms; };

sk_u32 sk_txid(sk_u32 serial, sk_u32 leg, sk_u32 idx);                          /* 16 bit DNS transaction id: serial 8 bits, leg 3 bits, probe index 5 bits */
int sk_txid_parse(sk_u32 txid, sk_u32 serial, sk_u32 *leg, sk_u32 *idx);        /* 1 = a reply of THIS burst (an older burst's late reply is refused) */
void sk_burst_init(struct sk_burst *b, sk_u32 ip);
sk_u32 sk_burst_send(struct sk_burst *b, sk_u32 now_us);                         /* -> the probe index, or 0xFFFFFFFF when the burst is full */
int sk_burst_reply(struct sk_burst *b, sk_u32 idx, sk_u32 now_us);               /* 1 = accepted (valid index, sent, not yet answered) */
void sk_burst_stats(const struct sk_burst *b, struct sk_stats *s);

/* ---- running averages for the screen (a window: the last minute, the last hour) ---- */
struct sk_agg { sk_u32 bursts, sent[SK_LEGS], recv[SK_LEGS], p50_sum[SK_LEGS], p95_sum[SK_LEGS], ok_bursts[SK_LEGS], gap_max[SK_LEGS], p95_max[SK_LEGS]; };
void sk_agg_reset(struct sk_agg *a);
void sk_agg_add(struct sk_agg *a, const struct sk_stats st[SK_LEGS]);
sk_u32 sk_agg_loss_x100(const struct sk_agg *a, sk_u32 leg);                    /* 1/100 percent */
sk_u32 sk_agg_p50(const struct sk_agg *a, sk_u32 leg);                           /* mean of the bursts' p50, microseconds */
sk_u32 sk_agg_p95(const struct sk_agg *a, sk_u32 leg);

/* ---- link health: drops, durations, when to restart the stack, the watchdog ---- */
enum { SK_ACT_NONE = 0, SK_ACT_RESTART = 1 };
enum { SK_EV_NONE = 0, SK_EV_DROP = 1, SK_EV_UP = 2 };
struct sk_health {
    sk_u32 drops, restarts, blackouts, wd, down, down_since_ms, down_total_ms, down_max_ms, bad_bursts, last_sample_ms, last_restart_ms, in_blackout;
    sk_u32 ev, ev_a, ev_b;                                                       /* the event the last call raised: SK_EV_*; DROP a = reason (1 status, 2 silent), UP a = down ms, b = bad bursts */
};
void sk_health_init(struct sk_health *h, sk_u32 now_ms);
/* net_ok: net_get_status()==0 and an address.  total_recv / total_sent: over all legs of the burst.  -> SK_ACT_*; h->ev says whether a drop began or ended */
int sk_health_burst(struct sk_health *h, sk_u32 now_ms, int net_ok, sk_u32 total_sent, sk_u32 total_recv);
int sk_health_watchdog(struct sk_health *h, sk_u32 now_ms);                      /* 1 = no sample for SK_WATCHDOG_MS: the caller restarts the stack (counted in wd) */
void sk_health_restarted(struct sk_health *h, sk_u32 now_ms);                    /* the restart attempt is over (up or not): the gap before the next one counts from here */

/* ---- lines ---- */
struct sk_ctx { char session[9]; sk_u32 seq; sk_u32 up_s, wall; };
void sk_session_id(char out[9], sk_u32 a, sk_u32 b);                             /* 8 chars [a-z0-9] */
sk_u32 sk_fmt_sample(char *o, sk_u32 cap, struct sk_ctx *c, sk_u32 net_st, const struct sk_health *h, sk_u32 ul_ok, sk_u32 ul_fail, sk_u32 sd_fail, const struct sk_stats st[SK_LEGS]);
sk_u32 sk_fmt_tls(char *o, sk_u32 cap, struct sk_ctx *c, int ok, sk_u32 total_ms, sk_u32 conn_ms, sk_u32 hs_ms, sk_u32 http, const char *why);
sk_u32 sk_fmt_net(char *o, sk_u32 cap, struct sk_ctx *c, sk_u32 net_st, int link_rc, sk_u32 link_word, const struct sk_health *h, sk_u32 tls_ok, sk_u32 tls_fail, sk_u32 ul_ok, sk_u32 ul_fail, sk_u32 q_drop, sk_u32 sd_fail);
sk_u32 sk_fmt_event(char *o, sk_u32 cap, struct sk_ctx *c, const char *type, sk_u32 a, sk_u32 b);
sk_u32 sk_fmt_upload(char *o, sk_u32 cap, struct sk_ctx *c, int ok, sk_u32 total_ms, sk_u32 conn_ms, sk_u32 hs_ms, sk_u32 http, sk_u32 bytes, sk_u32 lines, sk_u32 skipped, const char *why);
void sk_clean(char *dst, sk_u32 cap, const char *src);                           /* printable ASCII only, no comma / newline, at most 40 chars: for the free-text field */

/* ---- the queue of lines for the upload (RAM; the SD csv has them all) ---- */
struct sk_queue { char l[SK_QMAX][SK_LINE_MAX]; sk_u8 sent[SK_QMAX]; sk_u32 head, count, dropped, skipped, unsent; };
void sk_q_init(struct sk_queue *q);
void sk_q_push(struct sk_queue *q, const char *line);                            /* full: the oldest goes, counted in dropped when it was never sent */
/* the newest unsent lines that fit in `cap` bytes (oldest first, each '\n' terminated) into out; *n = their count, *skipped = older unsent lines that did not fit (they stay on the SD only).  -> bytes */
sk_u32 sk_q_build(const struct sk_queue *q, char *out, sk_u32 cap, sk_u32 *n, sk_u32 *skipped);
void sk_q_done(struct sk_queue *q, int ok, sk_u32 skipped);                      /* ok: every unsent line counts as sent (the older ones that did not fit as skipped); not ok: all stay queued */

/* ---- files ---- */
sk_u32 sk_part_name(char *o, sk_u32 cap, const char *session, sk_u32 part);      /* sd:/multishine/soak/soak-<session>.csv, part >= 2: soak-<session>.<part>.csv */
int sk_need_rotate(sk_u32 cur_size, sk_u32 add);                                 /* 1 = the line would take the file over SK_ROTATE_BYTES (an empty file never rotates) */
sk_u32 sk_parse_opts(const char *txt, sk_u32 len);                               /* SK_O_* from the marker file's text */

/* the diagnostics body up to the lines: MSDIAG1 / console / soak header / reason; the caller adds the lines and "proof <16 hex>\n" (msd_proof).  -> length (0 = does not fit) */
sk_u32 sk_body_head(char *o, sk_u32 cap, const char *cid, const char *session, sk_u32 shell_seq, sk_u32 n_lines, sk_u32 skipped, sk_u32 up_s);

#endif
