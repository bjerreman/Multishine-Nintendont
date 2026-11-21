/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Connection check, the pure part.  See ms_probe_core.h. */
#include "ms_probe_core.h"

#define RTT_BOGUS_US 30000000u

static const msp_u16 coarse_ms[20] = { 100, 110, 120, 130, 140, 150, 175, 200, 225, 250, 300, 350, 400, 500, 600, 750, 1000, 1500, 2000, 3000 };
/* the query after the 2 id bytes: flags RD, QDCOUNT 1, AN/NS/AR 0, "localhost", A, IN */
static const msp_u8 dns_q[MSP_DNS_LEN - 2u] = { 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 9, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0, 0, 1, 0, 1 };

static void zero(void *p, msp_u32 n) { msp_u8 *b = (msp_u8 *)p; while (n--) *b++ = 0; }
static msp_s32 dt(msp_u32 a, msp_u32 b) { return (msp_s32)(a - b); }   /* a - b, wrap-safe */

int msp_is_private(msp_u32 ip)
{
    return (ip >> 24) == 10u || (ip >> 20) == 0xAC1u || (ip >> 16) == 0xC0A8u || (ip >> 16) == 0xA9FEu;
}

msp_u32 msp_dns_build(msp_u8 *p, msp_u32 txid)
{
    msp_u32 i;
    p[0] = (msp_u8)(txid >> 8); p[1] = (msp_u8)txid;
    for (i = 0; i < sizeof dns_q; i++) p[2u + i] = dns_q[i];
    return MSP_DNS_LEN;
}

int msp_dns_parse(const msp_u8 *p, msp_u32 len, msp_u32 *txid)
{
    msp_u32 i;
    if (len < MSP_DNS_LEN) return 0;
    if (!(p[2] & 0x80u) || (p[2] & 0x78u)) return 0;          /* QR set, opcode 0 */
    if (p[4] != 0 || p[5] != 1) return 0;                     /* one question, repeated */
    for (i = 12; i < MSP_DNS_LEN; i++) if (p[i] != dns_q[i - 2u]) return 0;
    *txid = ((msp_u32)p[0] << 8) | p[1];
    return 1;
}

msp_u32 msp_hist_idx(msp_u32 rtt_us)
{
    msp_u32 ms = rtt_us / 1000u, k;
    if (ms < 100u) return ms;
    for (k = 19; k > 0; k--) if (ms >= coarse_ms[k]) break;
    return 100u + k;
}

static msp_u32 hist_mid_us(msp_u32 idx)
{
    if (idx < 100u) return idx * 1000u + 500u;
    idx -= 100u;
    if (idx >= 19u) return (msp_u32)coarse_ms[19] * 1000u + 500000u;
    return ((msp_u32)coarse_ms[idx] + coarse_ms[idx + 1]) * 500u;
}

msp_u32 msp_hist_pct(const msp_u16 *hist, msp_u32 n, msp_u32 pct, msp_u32 vmin, msp_u32 vmax)
{
    msp_u32 rank, acc = 0, i, v = vmax;
    if (!n) return 0;
    rank = (n * pct + 99u) / 100u;
    if (rank < 1u) rank = 1u;
    for (i = 0; i < MSP_HIST_N; i++) { acc += hist[i]; if (acc >= rank) { v = hist_mid_us(i); break; } }
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return v;
}

/* ---- verdict ---- */
static msp_u32 loss_of(const struct msp_result *r) { return r->sent ? r->lost * 10000u / r->sent : 10000u; }
static msp_u32 sent_ok(const struct msp_result *r, msp_u32 expected) { return r->sent * 100u >= expected * MSP_MIN_SENT_PCT; }

msp_u32 msp_verdict_hop(const struct msp_result *r, msp_u32 expected)
{
    msp_u32 loss;
    if (!r->recv || !r->sent) return MSP_V_FAILED;
    loss = loss_of(r);
    if (sent_ok(r, expected) && loss < MSP_H_GOOD_LOSS && r->p95 < MSP_H_GOOD_P95 && r->gap100 == 0) return MSP_V_GOOD;
    if (sent_ok(r, expected) && loss < MSP_H_OKAY_LOSS && r->p95 < MSP_H_OKAY_P95 && r->gap250 <= MSP_OKAY_GAPS_MAX) return MSP_V_OKAY;
    return MSP_V_POOR;
}

msp_u32 msp_verdict_internet(const struct msp_result *r, msp_u32 expected)
{
    msp_u32 loss, s95;
    if (!r->recv || !r->sent) return MSP_V_FAILED;
    loss = loss_of(r);
    s95 = r->p95 > r->p50 ? r->p95 - r->p50 : 0u;            /* the spread does not depend on the distance */
    if (sent_ok(r, expected) && loss < MSP_N_GOOD_LOSS && r->p95 < MSP_N_GOOD_P95 && s95 < MSP_N_GOOD_SPREAD && r->gap100 == 0) return MSP_V_GOOD;
    if (sent_ok(r, expected) && loss < MSP_N_OKAY_LOSS && r->p95 < MSP_N_OKAY_P95 && s95 < MSP_N_OKAY_SPREAD && r->gap250 <= MSP_OKAY_GAPS_MAX) return MSP_V_OKAY;
    return MSP_V_POOR;
}

msp_u32 msp_verdict(const struct msp_result *r, msp_u32 leg, msp_u32 expected)
{
    if (!r->recv || !r->sent) return MSP_V_FAILED;
    if (r->noisy) {                                          /* IOS socket calls were slow: absolute times, gaps and the cadence say nothing about the link; only loss counts and Good cannot be proven */
        msp_u32 loss = loss_of(r);
        return loss < MSP_H_OKAY_LOSS ? MSP_V_OKAY : MSP_V_POOR;
    }
    return leg >= MSP_LEG_CF ? msp_verdict_internet(r, expected) : msp_verdict_hop(r, expected);
}

/* ---- ladder ---- */
static void leg_begin(struct msp_ladder *L, msp_u32 now)
{
    zero(&L->s, sizeof L->s);
    L->session = (L->seed * 2654435761u) ^ ((L->legi + 1u) * 0x9E3779B1u);
    L->t_leg_us = now; L->next_send_us = now; L->last_send_us = now;
}

void msp_init(struct msp_ladder *L, msp_u32 now_us, msp_u32 seed, msp_u32 duration_us, msp_u32 interval_us)
{
    zero(L, sizeof *L);
    L->seed = seed; L->duration_us = duration_us ? duration_us : MSP_DURATION_US; L->interval_us = interval_us ? interval_us : MSP_INTERVAL_US;
    if (L->duration_us / L->interval_us > MSP_SEQ_MAX) L->duration_us = L->interval_us * MSP_SEQ_MAX;
    L->t_leg_us = now_us;
    L->verdict = MSP_V_NOTRUN;
}

int msp_add_leg(struct msp_ladder *L, msp_u32 leg, msp_u32 ip)
{
    if (L->nleg >= MSP_MAX_LEGS || L->started) return 0;
    if ((leg == MSP_LEG_ROUTER || leg == MSP_LEG_RESOLVER) && !msp_is_private(ip)) return 0;   /* a LAN leg is never a public address */
    if (leg == MSP_LEG_CF && ip != 0x01010101u) return 0;
    if (leg == MSP_LEG_GOOGLE && ip != 0x08080808u) return 0;
    if (leg < MSP_LEG_ROUTER || leg > MSP_LEG_GOOGLE) return 0;
    L->leg_id[L->nleg] = leg; L->leg_ip[L->nleg] = ip; L->nleg++;
    return 1;
}

msp_u32 msp_default_legs(struct msp_ladder *L, msp_u32 own_ip, msp_u32 gateway, msp_u32 dns)
{
    if (gateway && gateway != own_ip) (void)msp_add_leg(L, MSP_LEG_ROUTER, gateway);
    if (dns && dns != gateway && dns != own_ip && ((dns ^ own_ip) & 0xFFFFFF00u) == 0) (void)msp_add_leg(L, MSP_LEG_RESOLVER, dns);
    (void)msp_add_leg(L, MSP_LEG_CF, 0x01010101u);
    (void)msp_add_leg(L, MSP_LEG_GOOGLE, 0x08080808u);
    return L->nleg;
}

static void finalize(struct msp_ladder *L, msp_u32 now)
{
    struct msp_stats *s = &L->s;
    struct msp_result *r = &L->res;
    msp_u32 expected;
    (void)now;
    zero(r, sizeof *r);
    r->leg = L->leg_id[L->legi]; r->leg_ip = L->leg_ip[L->legi]; r->router_silent = L->router_silent;
    r->sent = s->sent; r->recv = s->recv; r->lost = s->sent - s->recv; r->loss_x100 = loss_of(r);
    r->dup = s->dup; r->ooo = s->ooo; r->late = s->late;
    r->rtt_min = s->rtt_min; r->rtt_max = s->rtt_max; r->jitter = s->jitter;
    r->p50 = msp_hist_pct(s->hist, s->recv, 50u, s->rtt_min, s->rtt_max);
    r->p95 = msp_hist_pct(s->hist, s->recv, 95u, s->rtt_min, s->rtt_max);
    r->p99 = msp_hist_pct(s->hist, s->recv, 99u, s->rtt_min, s->rtt_max);
    r->gap100 = s->gap100; r->gap250 = s->gap250; r->gap_max_ms = s->gap_max_ms;
    if (s->sent && dt(L->last_send_us, s->last_rx_us) > 0) {   /* the silence before the end of the send window counts like any other gap */
        msp_u32 g = L->last_send_us - s->last_rx_us;
        if (g >= MSP_GOOD_GAP) r->gap100++;
        if (g >= MSP_OKAY_GAP) r->gap250++;
        if (g / 1000u > r->gap_max_ms) r->gap_max_ms = g / 1000u;
    }
    r->slow_calls = s->slow_calls; r->call_max_us = s->call_max_us; r->send_fail = s->send_fail;
    r->noisy = s->sent && s->slow_calls * 100u > s->sent * MSP_NOISY_PCT;
    r->run_ms = (L->last_send_us - L->t_leg_us) / 1000u;
    expected = L->duration_us / L->interval_us;
    r->verdict = msp_verdict(r, r->leg, expected);
    r->reason = r->verdict == MSP_V_FAILED ? MSP_R_NOANSWER : MSP_R_NONE;
    L->verdict = r->verdict; L->reason = r->reason; L->finished = 1;
}

static void fail_all(struct msp_ladder *L, msp_u32 reason)
{
    zero(&L->res, sizeof L->res);
    L->res.verdict = MSP_V_FAILED; L->res.reason = reason; L->res.router_silent = L->router_silent;
    L->verdict = MSP_V_FAILED; L->reason = reason; L->finished = 1;
}

int msp_poll(struct msp_ladder *L, msp_u32 now)
{
    struct msp_stats *s = &L->s;
    if (L->finished) return MSP_ACT_DONE;
    if (!L->started) {
        if (!L->nleg) { fail_all(L, MSP_R_NOLEG); return MSP_ACT_DONE; }
        L->started = 1; L->legi = 0; L->verdict = MSP_V_RUNNING;
        leg_begin(L, now);
    }
    if (!s->recv && s->sent && dt(now, L->t_leg_us) >= (msp_s32)MSP_LEG_WAIT_US) {    /* this leg is silent: the next one */
        if (L->leg_id[L->legi] == MSP_LEG_ROUTER) L->router_silent = 1;
        if (L->legi + 1u >= L->nleg) { fail_all(L, MSP_R_NOANSWER); return MSP_ACT_DONE; }
        L->legi++;
        leg_begin(L, now);
        return MSP_ACT_WAIT;
    }
    if (dt(now, L->t_leg_us) >= (msp_s32)L->duration_us || s->sent >= MSP_SEQ_MAX) {   /* the send window is over: drain */
        if (dt(now, L->t_leg_us) >= (msp_s32)(L->duration_us + MSP_DRAIN_US) || s->recv == s->sent) { finalize(L, now); return MSP_ACT_DONE; }
        return MSP_ACT_WAIT;
    }
    if (dt(now, L->next_send_us) >= 0) return MSP_ACT_SEND;
    return MSP_ACT_WAIT;
}

msp_u32 msp_make_query(struct msp_ladder *L, msp_u8 *buf, msp_u32 now, msp_u32 *ip, msp_u32 *port)
{
    struct msp_stats *s = &L->s;
    msp_u32 n = msp_dns_build(buf, (s->sent + L->session) & 0xFFFFu);
    if (!s->sent) { L->t_leg_us = now; s->last_rx_us = now; }   /* the leg's clock starts at its first send */
    s->sent_us[s->sent % MSP_RING] = now;
    s->sent++;
    L->last_send_us = now;
    if (dt(now, L->next_send_us) > (msp_s32)(2u * L->interval_us)) L->next_send_us = now + L->interval_us;   /* the caller was away: no catching up */
    else L->next_send_us += L->interval_us;
    *ip = L->leg_ip[L->legi]; *port = 53u;
    return n;
}

void msp_note_call(struct msp_ladder *L, msp_u32 call_us)
{
    if (call_us >= MSP_SLOW_CALL_US) L->s.slow_calls++;
    if (call_us > L->s.call_max_us) L->s.call_max_us = call_us;
}

void msp_finish_now(struct msp_ladder *L, msp_u32 now)
{
    if (L->finished) return;
    if (!L->started) { fail_all(L, MSP_R_NOLEG); return; }
    if (L->s.recv) finalize(L, now);          /* the running leg has answered: the verdict of what was measured (too few sends = Poor at best) */
    else fail_all(L, MSP_R_NOANSWER);
}

void msp_note_send_fail(struct msp_ladder *L) { L->s.send_fail++; }

int msp_on_reply(struct msp_ladder *L, const msp_u8 *buf, msp_u32 len, msp_u32 from_ip, msp_u32 from_port, msp_u32 now)
{
    struct msp_stats *s = &L->s;
    msp_u32 id, seq, rtt;
    msp_s32 d, g;
    if (!L->started || L->finished || !s->sent) return 0;
    if (from_ip != L->leg_ip[L->legi] || from_port != 53u) { s->late++; return 0; }
    if (!msp_dns_parse(buf, len, &id)) { s->late++; return 0; }
    seq = (id - L->session) & 0xFFFFu;
    if (seq >= s->sent || s->sent - seq > MSP_RING) { s->late++; return 0; }
    rtt = now - s->sent_us[seq % MSP_RING];
    if (rtt > RTT_BOGUS_US) { s->late++; return 0; }
    if (s->seen[seq >> 3] & (1u << (seq & 7u))) { s->dup++; return 0; }
    s->seen[seq >> 3] |= (msp_u8)(1u << (seq & 7u));
    if (s->recv && seq < s->hi_seq) s->ooo++;
    if (seq > s->hi_seq || !s->recv) s->hi_seq = seq;
    s->recv++;
    s->hist[msp_hist_idx(rtt)]++;
    if (s->recv == 1u || rtt < s->rtt_min) s->rtt_min = rtt;
    if (rtt > s->rtt_max) s->rtt_max = rtt;
    d = (msp_s32)rtt - (msp_s32)s->rtt_mean;                 /* running mean: no 64-bit sum */
    s->rtt_mean = (msp_u32)((msp_s32)s->rtt_mean + d / (msp_s32)s->recv);
    if (s->recv > 1u) {
        msp_s32 dd = (msp_s32)rtt - (msp_s32)s->prev_rtt;
        if (dd < 0) dd = -dd;
        s->jitter = (msp_u32)((msp_s32)s->jitter + (dd - (msp_s32)s->jitter) / 16);
    }
    s->prev_rtt = rtt;
    g = dt(now, s->last_rx_us);
    if (s->recv == 1u) g -= (msp_s32)rtt;                    /* the wait for the first reply is one round trip by design */
    if (g >= (msp_s32)MSP_GOOD_GAP) s->gap100++;
    if (g >= (msp_s32)MSP_OKAY_GAP) s->gap250++;
    if (g > 0 && (msp_u32)g / 1000u > s->gap_max_ms) s->gap_max_ms = (msp_u32)g / 1000u;
    s->last_rx_us = now;
    return 1;
}

int msp_done(const struct msp_ladder *L) { return L->finished; }

void msp_result(const struct msp_ladder *L, struct msp_result *out)
{
    const struct msp_stats *s = &L->s;
    if (L->finished) { *out = L->res; return; }
    zero(out, sizeof *out);
    out->verdict = L->started ? MSP_V_RUNNING : MSP_V_NOTRUN;
    if (!L->started) return;
    out->leg = L->leg_id[L->legi]; out->leg_ip = L->leg_ip[L->legi]; out->router_silent = L->router_silent;
    out->sent = s->sent; out->recv = s->recv; out->lost = s->sent - s->recv; out->loss_x100 = loss_of(out);
    out->slow_calls = s->slow_calls; out->call_max_us = s->call_max_us; out->jitter = s->jitter;
}

int msp_should_run(int kind, int adapter, int *hint)
{
    if (hint) *hint = (kind == MSP_NET_WIRELESS && adapter == 1) ? 1 : 0;
    return kind == MSP_NET_WIRED ? 0 : 1;
}

int msp_cfg_kind(const msp_u8 *cfg, msp_u32 len, msp_u32 profile, int *link)
{
    msp_u32 k, sel = 0;
    int lk = MSP_NET_UNKNOWN;
    if (link) *link = MSP_NET_UNKNOWN;
    if (!cfg || len < 0x1B5Cu) return MSP_NET_UNKNOWN;
    if (cfg[4] == 1) lk = MSP_NET_WIRELESS; else if (cfg[4] == 2) lk = MSP_NET_WIRED;
    if (link) *link = lk;
    if (profile >= 1u && profile <= 3u) return (cfg[8u + (profile - 1u) * 0x91Cu] & 1u) ? MSP_NET_WIRED : MSP_NET_WIRELESS;
    for (k = 0; k < 3u; k++) if (cfg[8u + k * 0x91Cu] & 0x80u) { sel = k + 1u; break; }
    if (lk != MSP_NET_UNKNOWN) return lk;
    if (sel) return (cfg[8u + (sel - 1u) * 0x91Cu] & 1u) ? MSP_NET_WIRED : MSP_NET_WIRELESS;
    return MSP_NET_UNKNOWN;
}
