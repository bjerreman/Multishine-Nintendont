/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_cc: the Connection check, see vbba_cc.h.  C99, no libc, no heap, no 64-bit arithmetic. */
#include "vbba_cc.h"

#define POLL_IDLE_US 25000u
#define POLL_RUN_US 10000u
#define TG_PERIOD_US 500000u
#define WS_PERIOD_US 250000u
#define NOECHO_US 10000000u
#define RTT_BOGUS_US 30000000u

static const vbba_u16 coarse_ms[20] = { 100, 110, 120, 130, 140, 150, 175, 200, 225, 250, 300, 350, 400, 500, 600, 750, 1000, 1500, 2000, 3000 };

static void zero(void *p, vbba_u32 n) { vbba_u8 *b = (vbba_u8 *)p; while (n--) *b++ = 0; }
static vbba_u32 get32(const vbba_u8 *p) { return ((vbba_u32)p[0] << 24) | ((vbba_u32)p[1] << 16) | ((vbba_u32)p[2] << 8) | p[3]; }
static void put32(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }
static vbba_s32 dt(vbba_u32 a, vbba_u32 b) { return (vbba_s32)(a - b); }   /* a - b, wrap-safe */
static vbba_u32 x10(vbba_u32 us) { vbba_u32 v = (us + 50u) / 100u; return v > 0xFFFFu ? 0xFFFFu : v; }   /* microseconds -> 0.1 ms, saturating at 16 bits */
static vbba_u32 ms16(vbba_u32 ms) { return ms > 0xFFFFu ? 0xFFFFu : ms; }
static vbba_u32 cnt16(vbba_u32 n) { return n > 0xFFFFu ? 0xFFFFu : n; }

vbba_u32 vbba_cc_hist_idx(vbba_u32 rtt_us)
{
    vbba_u32 ms = rtt_us / 1000u, k;
    if (ms < 100u) return ms;
    for (k = 19; k > 0; k--) if (ms >= coarse_ms[k]) break;
    return 100u + k;
}

static vbba_u32 hist_mid_us(vbba_u32 idx)
{
    if (idx < 100u) return idx * 1000u + 500u;
    idx -= 100u;
    if (idx >= 19u) return (vbba_u32)coarse_ms[19] * 1000u + 500000u;
    return ((vbba_u32)coarse_ms[idx] + coarse_ms[idx + 1]) * 500u;
}

vbba_u32 vbba_cc_hist_pct(const vbba_u16 *hist, vbba_u32 n, vbba_u32 pct, vbba_u32 vmin, vbba_u32 vmax)
{
    vbba_u32 rank, acc = 0, i, v = vmax;
    if (!n) return 0;
    rank = (n * pct + 99u) / 100u;
    if (rank < 1u) rank = 1u;
    for (i = 0; i < VBBA_CC_HIST_N; i++) { acc += hist[i]; if (acc >= rank) { v = hist_mid_us(i); break; } }
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return v;
}

void vbba_cc_pkt_build(vbba_u8 *p, vbba_u32 len, vbba_u32 session, vbba_u32 seq, vbba_u32 t_us)
{
    vbba_u32 i;
    put32(p, VBBA_CC_PKT_MAGIC);
    p[4] = VBBA_CC_PKT_VER; p[5] = 0; p[6] = 0; p[7] = 0;
    put32(p + 8, session); put32(p + 12, seq); put32(p + 16, t_us); put32(p + 20, 0);
    p[24] = (vbba_u8)(len >> 8); p[25] = (vbba_u8)len; p[26] = 0; p[27] = 0;
    for (i = VBBA_CC_HDR; i < len; i++) p[i] = (vbba_u8)(seq * 31u + i * 7u + session);
}

int vbba_cc_pkt_parse(const vbba_u8 *p, vbba_u32 len, vbba_u32 *session, vbba_u32 *seq, vbba_u32 *t_us, vbba_u32 *srv_us)
{
    if (len < VBBA_CC_HDR + 4u || len > VBBA_CC_SIZE_MAX) return 0;
    if (get32(p) != VBBA_CC_PKT_MAGIC || p[4] != VBBA_CC_PKT_VER || p[5] != 1u) return 0;
    if ((((vbba_u32)p[24] << 8) | p[25]) != len) return 0;
    *session = get32(p + 8); *seq = get32(p + 12); *t_us = get32(p + 16); *srv_us = get32(p + 20);
    return 1;
}

vbba_u32 vbba_cc_verdict_of(const struct vbba_cc_result *r, vbba_u32 expected_sends)
{
    vbba_u32 loss, sent_ok;
    if (!r->recv || !r->sent) return VBBA_CC_V_FAILED;
    loss = r->lost * 10000u / r->sent;
    sent_ok = r->sent * 100u >= expected_sends * VBBA_CC_MIN_SENT_PCT;
    if (sent_ok && loss < VBBA_CC_GOOD_LOSS_X100 && r->p95 < VBBA_CC_GOOD_P95_US && r->gap100 <= VBBA_CC_GOOD_GAP100_MAX) return VBBA_CC_V_GOOD;
    if (sent_ok && loss < VBBA_CC_OKAY_LOSS_X100 && r->p95 < VBBA_CC_OKAY_P95_US && r->gap250 <= VBBA_CC_OKAY_GAP250_MAX) return VBBA_CC_V_OKAY;
    return VBBA_CC_V_POOR;
}

/* ---- mailbox ---- */
static void pub_status(struct vbba_cc *c)
{
    c->mb[CC_W_ACK] = c->ack; c->mb[CC_W_VERDICT] = c->verdict;
    c->mb[CC_W_STATE] = (c->phase << 24) | (c->reason << 16) | (c->progress << 8) | c->link;
    c->h.mb_publish(c->h.ctx, CC_W_ACK, 3u, c->mb + CC_W_ACK);   /* words 2, 3, 4 */
}

static void pub_seq(struct vbba_cc *c) { c->mb[CC_W_SEQ] = ++c->seq; c->h.mb_publish(c->h.ctx, CC_W_SEQ, 1u, c->mb + CC_W_SEQ); }

static void pub_results(struct vbba_cc *c)
{
    const struct vbba_cc_result *r = &c->r;
    vbba_u32 *m = c->mb;
    pub_seq(c);                                              /* odd: being written */
    m[CC_R_SENT] = r->sent; m[CC_R_RECV] = r->recv; m[CC_R_LOST] = r->lost;
    m[CC_R_OOODUP] = (cnt16(r->ooo) << 16) | cnt16(r->dup);
    m[CC_R_P50P95] = (x10(r->p50) << 16) | x10(r->p95);
    m[CC_R_P99JIT] = (x10(r->p99) << 16) | x10(r->jitter);
    m[CC_R_MINMAX] = (x10(r->rtt_min) << 16) | x10(r->rtt_max);
    m[CC_R_GAPS] = (cnt16(r->gap100) << 16) | cnt16(r->gap250);
    m[CC_R_LGAVG] = (ms16(r->gap_max_ms) << 16) | x10(r->rtt_mean);
    m[CC_R_SENDTO] = (x10(r->sendto_mean) << 16) | x10(r->sendto_max);
    m[CC_R_ARM] = (cnt16(r->arm5) << 16) | cnt16(r->arm20);
    m[CC_R_RUN] = (ms16(r->step_gap_max_ms) << 16) | cnt16(r->run_ds);
    m[CC_R_WS_MIN] = r->ws_min; m[CC_R_WS_MEAN] = r->ws_mean; m[CC_R_WS_LAST] = r->ws_last;
    m[CC_R_WS_N] = (cnt16(r->ws_n) << 16) | (r->loss_x100 & 0xFFFFu);
    m[CC_R_TXRX] = (cnt16(r->tx_refused) << 16) | cnt16(r->slips);
    m[CC_R_LATE] = (cnt16(r->late) << 16);
    c->h.mb_publish(c->h.ctx, CC_R_SENT, VBBA_CC_ARM_WORDS - CC_R_SENT, m + CC_R_SENT);
    pub_seq(c);                                              /* even: consistent */
}

static vbba_u32 guest_rate(const struct vbba_cc *c, vbba_u32 now)   /* guest UDP datagrams per second over the last 0.5..1 s (0 = not known yet) */
{
    vbba_u32 ms;
    if (!c->tg_a_us || dt(now, c->tg_a_us) < (vbba_s32)TG_PERIOD_US) return 0;
    ms = (now - c->tg_a_us) / 1000u;
    return (c->h.guest_tx(c->h.ctx) - c->tg_a_n) * 1000u / ms;
}

static void close_sock(struct vbba_cc *c)
{
    if (c->handle >= 0) { c->tp->close(c->tp->ctx, c->handle); c->handle = -1; }
}

static void finish_fail(struct vbba_cc *c, vbba_u32 reason)
{
    if (c->phase == VBBA_CC_P_RUN || c->phase == VBBA_CC_P_DRAIN) c->h.txlat_restore(c->h.ctx, c->txlat_old_max);   /* the game's own statistic is not lost */
    close_sock(c);
    c->verdict = reason == VBBA_CC_R_CANCEL ? VBBA_CC_V_NOTRUN : VBBA_CC_V_FAILED;
    c->phase = VBBA_CC_P_DONE; c->reason = reason; c->progress = 0;
    pub_results(c);
    pub_status(c);
}

static void start_run(struct vbba_cc *c, vbba_u32 now)
{
    struct vbba_cc_txlat t;
    c->phase = VBBA_CC_P_RUN;
    c->t0_us = c->next_send_us = c->last_rx_us = c->last_step_us = c->last_prog_us = c->last_ws_us = now;
    c->h.txlat(c->h.ctx, &t, 1);
    c->txlat_old_max = t.max_us; c->tx0_n = t.n; c->tx0_sum = t.sum_us;
    c->h.arm_passes(c->h.ctx, &c->a0_5, &c->a0_20);
    c->progress = 0;
    pub_status(c);
}

static void handle_request(struct vbba_cc *c, vbba_u32 now)
{
    vbba_u32 port = c->mb[CC_W_PORT], dur = (port >> 16) & 0xFFu, sz = port >> 24, i;
    struct vbba_tp_status st;
    c->ack = c->mb[CC_W_REQ];
    c->ip = c->mb[CC_W_IP]; c->port = (vbba_u16)(port & 0xFFFFu);
    if (!dur) dur = VBBA_CC_DURATION_S;
    if (dur < VBBA_CC_DURATION_MIN_S) dur = VBBA_CC_DURATION_MIN_S;
    if (dur > VBBA_CC_DURATION_MAX_S) dur = VBBA_CC_DURATION_MAX_S;
    if (!sz) sz = VBBA_CC_SIZE_DEFAULT;
    if (sz < VBBA_CC_SIZE_MIN) sz = VBBA_CC_SIZE_MIN;
    if (sz > VBBA_CC_SIZE_MAX) sz = VBBA_CC_SIZE_MAX;
    c->duration_us = dur * 1000000u; c->size = sz;
    c->link = c->h.link_type(c->h.ctx);
    zero(&c->r, sizeof c->r); zero(c->seen, sizeof c->seen); zero(c->hist, sizeof c->hist);
    c->hi_seq = 0; c->prev_rtt = 0;                           /* (the seqlock counter keeps counting up) */
    c->rtt_sum_hi = c->rtt_sum_lo = 0;
    for (i = 0; i < 4u; i++) { c->ws_min[i] = 255u; c->ws_sum[i] = 0; }
    c->ws_n = 0; c->ws_last = 0;
    c->reason = VBBA_CC_R_NONE; c->verdict = VBBA_CC_V_NOTRUN;
    pub_results(c);                                          /* the old result is gone before anything else is said */
    if (!c->ip || !c->port) { finish_fail(c, VBBA_CC_R_BADADDR); return; }
    c->tp->status(c->tp->ctx, &st);
    if (!st.up) { finish_fail(c, VBBA_CC_R_NONET); return; }
    if (guest_rate(c, now) > VBBA_CC_BUSY_TX_PER_S) { finish_fail(c, VBBA_CC_R_BUSY); return; }
    c->handle = c->tp->open(c->tp->ctx, 0);
    if (c->handle < 0) { finish_fail(c, VBBA_CC_R_NOSOCK); return; }
    c->session = (now * 2654435761u) ^ c->ack ^ 0x4D534543u;
    c->open_us = now;
    c->phase = VBBA_CC_P_OPEN; c->verdict = VBBA_CC_V_RUNNING;
    pub_status(c);
}

static void finalize(struct vbba_cc *c, vbba_u32 now)
{
    struct vbba_cc_result *r = &c->r;
    struct vbba_cc_txlat t;
    vbba_u32 p5, p20, i, last_send = c->t0_us + c->duration_us, expected;
    if (r->sent) last_send = c->next_send_us - VBBA_CC_INTERVAL_US;
    if (r->sent && dt(last_send, c->last_rx_us) > 0) {           /* the silence before the end of the send window counts like any other gap */
        vbba_u32 g = last_send - c->last_rx_us;
        if (g >= 100000u) r->gap100++;
        if (g >= 250000u) r->gap250++;
        if (g / 1000u > r->gap_max_ms) r->gap_max_ms = g / 1000u;
    }
    r->lost = r->sent - r->recv;
    r->loss_x100 = r->sent ? r->lost * 10000u / r->sent : 10000u;
    r->p50 = vbba_cc_hist_pct(c->hist, r->recv, 50u, r->rtt_min, r->rtt_max);
    r->p95 = vbba_cc_hist_pct(c->hist, r->recv, 95u, r->rtt_min, r->rtt_max);
    r->p99 = vbba_cc_hist_pct(c->hist, r->recv, 99u, r->rtt_min, r->rtt_max);
    c->h.txlat(c->h.ctx, &t, 0);
    r->sendto_mean = t.n != c->tx0_n ? (t.sum_us - c->tx0_sum) / (t.n - c->tx0_n) : 0;
    r->sendto_max = t.max_us;
    c->h.txlat_restore(c->h.ctx, c->txlat_old_max);
    c->h.arm_passes(c->h.ctx, &p5, &p20);
    r->arm5 = p5 - c->a0_5; r->arm20 = p20 - c->a0_20;
    r->run_ds = (vbba_u32)(now - c->t0_us) / 100000u;
    if (c->link == VBBA_CC_L_WIFI && c->ws_n) {
        r->ws_n = c->ws_n; r->ws_last = c->ws_last;
        for (i = 0; i < 4u; i++) { r->ws_min |= c->ws_min[i] << (24u - 8u * i); r->ws_mean |= (c->ws_sum[i] / c->ws_n) << (24u - 8u * i); }
    }
    expected = c->duration_us / VBBA_CC_INTERVAL_US;
    close_sock(c);
    c->verdict = vbba_cc_verdict_of(r, expected);
    c->reason = c->verdict == VBBA_CC_V_FAILED ? VBBA_CC_R_NOECHO : VBBA_CC_R_NONE;
    c->phase = VBBA_CC_P_DONE; c->progress = 100;
    pub_results(c);
    pub_status(c);
}

static void rx_one(struct vbba_cc *c, const vbba_u8 *buf, vbba_u32 n, vbba_u32 ip, vbba_u32 port, vbba_u32 now)
{
    struct vbba_cc_result *r = &c->r;
    vbba_u32 sess, seq, t_us, srv, rtt;
    vbba_s32 d;
    if (ip != c->ip || port != c->port || !vbba_cc_pkt_parse(buf, n, &sess, &seq, &t_us, &srv) || sess != c->session || n != c->size || seq >= r->sent) { r->late++; return; }
    rtt = now - t_us;
    if (rtt > RTT_BOGUS_US) { r->late++; return; }
    if (c->seen[seq >> 3] & (1u << (seq & 7u))) { r->dup++; return; }
    c->seen[seq >> 3] |= (vbba_u8)(1u << (seq & 7u));
    if (r->recv && seq < c->hi_seq) r->ooo++;
    if (seq > c->hi_seq || !r->recv) c->hi_seq = seq;
    r->recv++;
    c->hist[vbba_cc_hist_idx(rtt)]++;
    if (r->recv == 1u || rtt < r->rtt_min) r->rtt_min = rtt;
    if (rtt > r->rtt_max) r->rtt_max = rtt;
    d = (vbba_s32)rtt - (vbba_s32)r->rtt_mean;               /* running mean: no 64-bit sum */
    r->rtt_mean = (vbba_u32)((vbba_s32)r->rtt_mean + d / (vbba_s32)r->recv);
    if (r->recv > 1u) {
        vbba_s32 dd = (vbba_s32)rtt - (vbba_s32)c->prev_rtt;
        if (dd < 0) dd = -dd;
        r->jitter = (vbba_u32)((vbba_s32)r->jitter + (dd - (vbba_s32)r->jitter) / 16);
    }
    c->prev_rtt = rtt;
    {
        vbba_s32 g = dt(now, c->last_rx_us);
        if (g >= 100000) r->gap100++;
        if (g >= 250000) r->gap250++;
        if (g > 0 && (vbba_u32)g / 1000u > r->gap_max_ms) r->gap_max_ms = (vbba_u32)g / 1000u;
        c->last_rx_us = now;
    }
}

static void send_one(struct vbba_cc *c, vbba_u32 now)
{
    struct vbba_cc_result *r = &c->r;
    vbba_cc_pkt_build(c->pkt, c->size, c->session, r->sent, now);
    if (c->tp->send(c->tp->ctx, c->handle, c->ip, c->port, c->pkt, (vbba_u16)c->size) != VBBA_OK) r->tx_refused++;
    r->sent++;
}

static void run_step(struct vbba_cc *c, vbba_u32 now)
{
    struct vbba_cc_result *r = &c->r;
    vbba_u32 k, ip;
    vbba_u16 port;
    vbba_u8 buf[VBBA_CC_SIZE_MAX + 8];
    vbba_s32 gap = dt(now, c->last_step_us);
    if (gap > 0 && (vbba_u32)gap / 1000u > r->step_gap_max_ms && c->phase == VBBA_CC_P_RUN) r->step_gap_max_ms = (vbba_u32)gap / 1000u;
    c->last_step_us = now;
    for (k = 0; k < 8u; k++) {                               /* receive */
        int got = c->tp->recv(c->tp->ctx, c->handle, buf, sizeof buf, &ip, &port);
        if (got == VBBA_TP_TRUNC) { r->late++; continue; }
        if (got <= 0) break;
        rx_one(c, buf, (vbba_u32)got, ip, port, now);
    }
    if (c->phase == VBBA_CC_P_RUN) {                         /* send on the 60 Hz schedule */
        vbba_s32 behind = dt(now, c->next_send_us);
        if (behind > (vbba_s32)(2u * VBBA_CC_INTERVAL_US)) { r->slips++; c->next_send_us = now; behind = 0; }
        for (k = 0; k < 2u && dt(now, c->next_send_us) >= 0 && r->sent < VBBA_CC_SEQ_MAX && dt(now, c->t0_us) < (vbba_s32)c->duration_us; k++) {
            send_one(c, now);
            c->next_send_us += VBBA_CC_INTERVAL_US;
        }
        if (c->link == VBBA_CC_L_WIFI && dt(now, c->last_ws_us) >= (vbba_s32)WS_PERIOD_US) {
            vbba_u32 w = c->h.wsig(c->h.ctx), i;
            c->last_ws_us = now; c->ws_n++; c->ws_last = w;
            for (i = 0; i < 4u; i++) { vbba_u32 b = (w >> (24u - 8u * i)) & 255u; if (b < c->ws_min[i]) c->ws_min[i] = b; c->ws_sum[i] += b; }
        }
        if (dt(now, c->last_prog_us) >= 1000000) {
            vbba_u32 el = now - c->t0_us;
            c->last_prog_us = now;
            c->progress = el >= c->duration_us ? 99u : el / (c->duration_us / 100u);
            pub_status(c);
        }
        if (!r->recv && dt(now, c->t0_us) >= (vbba_s32)NOECHO_US) { finalize(c, now); return; }     /* no echo at all: do not make the player wait */
        if (dt(now, c->t0_us) >= (vbba_s32)c->duration_us) { c->phase = VBBA_CC_P_DRAIN; pub_status(c); }
    } else if (c->phase == VBBA_CC_P_DRAIN) {
        if (dt(now, c->t0_us) >= (vbba_s32)(c->duration_us + VBBA_CC_DRAIN_US) || r->recv == r->sent) finalize(c, now);
    }
}

static void tg_sample(struct vbba_cc *c, vbba_u32 now)
{
    if (!c->tg_b_us) { c->tg_b_us = now; c->tg_b_n = c->h.guest_tx(c->h.ctx); return; }
    if (dt(now, c->tg_b_us) >= (vbba_s32)TG_PERIOD_US) {     /* tg_a = the sample 0.5..1 s ago */
        c->tg_a_us = c->tg_b_us; c->tg_a_n = c->tg_b_n;
        c->tg_b_us = now; c->tg_b_n = c->h.guest_tx(c->h.ctx);
    }
}

void vbba_cc_init(struct vbba_cc *c, const struct vbba_cc_hooks *h, const struct vbba_transport *tp)
{
    zero(c, sizeof *c);
    c->h = *h; c->tp = tp; c->handle = -1;
    c->h.mb_fetch(c->h.ctx, CC_W_REQ, 4u, c->mb + CC_W_REQ);
    c->ack = c->mb[CC_W_REQ];                                 /* a stale token from an earlier run of the kernel is not a request */
    c->cancel_seen = c->mb[CC_W_CANCEL];
    c->mb[CC_W_VER] = (VBBA_CC_ABI << 16) | VBBA_CC_WORDS;
    c->mb[CC_W_MAGIC] = VBBA_CC_MAGIC;
    c->link = c->h.link_type(c->h.ctx);
    pub_results(c);
    pub_status(c);
    c->h.mb_publish(c->h.ctx, CC_W_MAGIC, 2u, c->mb);         /* last: the block is valid when the magic is there */
}

int vbba_cc_running(const struct vbba_cc *c) { return c->phase == VBBA_CC_P_OPEN || c->phase == VBBA_CC_P_RUN || c->phase == VBBA_CC_P_DRAIN; }
const struct vbba_cc_result *vbba_cc_result(const struct vbba_cc *c) { return &c->r; }

void vbba_cc_step(struct vbba_cc *c)
{
    vbba_u32 now = c->h.now_us(c->h.ctx);
    tg_sample(c, now);
    if (!vbba_cc_running(c)) {
        if (dt(now, c->nextpoll_us) < 0) return;
        c->nextpoll_us = now + POLL_IDLE_US;
        c->h.mb_fetch(c->h.ctx, CC_W_REQ, 4u, c->mb + CC_W_REQ);
        c->cancel_seen = c->mb[CC_W_CANCEL];                  /* a cancel with nothing running is forgotten */
        if (c->mb[CC_W_REQ] != c->ack) handle_request(c, now);
        return;
    }
    if (dt(now, c->nextpoll_us) >= 0) {                       /* running: the cancel word, and the guest starting a match */
        c->nextpoll_us = now + POLL_RUN_US;
        c->h.mb_fetch(c->h.ctx, CC_W_CANCEL, 1u, c->mb + CC_W_CANCEL);
        if (c->mb[CC_W_CANCEL] != c->cancel_seen) {
            c->cancel_seen = c->mb[CC_W_CANCEL];
            finish_fail(c, VBBA_CC_R_CANCEL);
            return;
        }
        if (guest_rate(c, now) > 2u * VBBA_CC_BUSY_TX_PER_S) { finish_fail(c, VBBA_CC_R_GAME); return; }
    }
    if (c->phase == VBBA_CC_P_OPEN) {
        int s = c->tp->state(c->tp->ctx, c->handle);
        if (s == VBBA_TP_OPEN) start_run(c, now);
        else if (s == VBBA_TP_FAILED || s == VBBA_TP_FREE || dt(now, c->open_us) >= (vbba_s32)VBBA_CC_OPEN_TIMEOUT_US) finish_fail(c, VBBA_CC_R_NOSOCK);
        return;
    }
    run_step(c, now);
}
