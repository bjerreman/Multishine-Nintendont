/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Wi-Fi SOAK mode, the pure part.  See ms_soak_core.h. */
#include <stdio.h>
#include <string.h>
#include "ms_soak_core.h"

#define UL(x) ((unsigned long)(x))

sk_u32 sk_txid(sk_u32 serial, sk_u32 leg, sk_u32 idx) { return ((serial & 0xFFu) << 8) | ((leg & 7u) << 5) | (idx & 31u); }
int sk_txid_parse(sk_u32 txid, sk_u32 serial, sk_u32 *leg, sk_u32 *idx)
{
    if (((txid >> 8) & 0xFFu) != (serial & 0xFFu)) return 0;
    *leg = (txid >> 5) & 7u; *idx = txid & 31u;
    return 1;
}

void sk_burst_init(struct sk_burst *b, sk_u32 ip) { memset(b, 0, sizeof *b); b->ip = ip; }
sk_u32 sk_burst_send(struct sk_burst *b, sk_u32 now_us)
{
    sk_u32 i;
    if (b->sent >= SK_BURST_N) return 0xFFFFFFFFu;
    i = b->sent++;
    b->tx_us[i] = now_us;
    if (!i) b->first_tx_us = now_us;
    b->last_tx_us = now_us;
    return i;
}
int sk_burst_reply(struct sk_burst *b, sk_u32 idx, sk_u32 now_us)
{
    if (idx >= b->sent || b->got[idx]) return 0;
    if ((sk_u32)(now_us - b->tx_us[idx]) > 30000000u) return 0;      /* not a reply of ours (clock oddity) */
    b->got[idx] = 1; b->rx_us[idx] = now_us; b->recv++;
    return 1;
}

static void sort_u32(sk_u32 *a, sk_u32 n)
{
    sk_u32 i, j, v;
    for (i = 1; i < n; i++) { v = a[i]; for (j = i; j > 0 && a[j - 1] > v; j--) a[j] = a[j - 1]; a[j] = v; }
}
static sk_u32 nearest_rank(const sk_u32 *sorted, sk_u32 n, sk_u32 pct)
{
    sk_u32 r;
    if (!n) return 0;
    r = (pct * n + 99u) / 100u;                                           /* ceil(pct * n / 100) */
    if (r < 1u) r = 1u;
    if (r > n) r = n;
    return sorted[r - 1u];
}

void sk_burst_stats(const struct sk_burst *b, struct sk_stats *s)
{
    sk_u32 rtt[SK_BURST_N], rxs[SK_BURST_N], i, n = 0, m = 0, prev = 0, have_prev = 0, jsum = 0, jn = 0, t, end, gap = 0;
    memset(s, 0, sizeof *s);
    s->sent = b->sent; s->recv = b->recv;
    for (i = 0; i < b->sent; i++) {
        if (!b->got[i]) { have_prev = 0; continue; }
        t = b->rx_us[i] - b->tx_us[i];
        rtt[n++] = t;
        rxs[m++] = b->rx_us[i] - b->first_tx_us;                          /* relative to the first send: small numbers, wrap safe */
        if (have_prev) { jsum += t > prev ? t - prev : prev - t; jn++; }
        prev = t; have_prev = 1;
    }
    if (n) {
        sort_u32(rtt, n);
        s->min = rtt[0]; s->max = rtt[n - 1];
        s->p50 = nearest_rank(rtt, n, 50); s->p95 = nearest_rank(rtt, n, 95); s->p99 = nearest_rank(rtt, n, 99);
        s->jitter = jn ? jsum / jn : 0;
    }
    if (b->sent) {                                                        /* the longest silence between the first send, each reply (arrival order) and the end of the window */
        sort_u32(rxs, m);
        end = b->last_tx_us - b->first_tx_us;
        if (m && rxs[m - 1] > end) end = rxs[m - 1];
        t = 0;
        for (i = 0; i < m; i++) { if (rxs[i] - t > gap) gap = rxs[i] - t; t = rxs[i]; }
        if (end - t > gap) gap = end - t;
        s->gap_ms = gap / 1000u;
    }
}

void sk_agg_reset(struct sk_agg *a) { memset(a, 0, sizeof *a); }
void sk_agg_add(struct sk_agg *a, const struct sk_stats st[SK_LEGS])
{
    sk_u32 l;
    a->bursts++;
    for (l = 0; l < SK_LEGS; l++) {
        a->sent[l] += st[l].sent; a->recv[l] += st[l].recv;
        if (st[l].recv) { a->ok_bursts[l]++; a->p50_sum[l] += st[l].p50; a->p95_sum[l] += st[l].p95; if (st[l].p95 > a->p95_max[l]) a->p95_max[l] = st[l].p95; }
        if (st[l].gap_ms > a->gap_max[l]) a->gap_max[l] = st[l].gap_ms;
    }
}
sk_u32 sk_agg_loss_x100(const struct sk_agg *a, sk_u32 l) { return a->sent[l] ? (sk_u32)(((a->sent[l] - a->recv[l]) * 10000ull) / a->sent[l]) : 0; }
sk_u32 sk_agg_p50(const struct sk_agg *a, sk_u32 l) { return a->ok_bursts[l] ? a->p50_sum[l] / a->ok_bursts[l] : 0; }
sk_u32 sk_agg_p95(const struct sk_agg *a, sk_u32 l) { return a->ok_bursts[l] ? a->p95_sum[l] / a->ok_bursts[l] : 0; }

void sk_health_init(struct sk_health *h, sk_u32 now_ms)
{
    memset(h, 0, sizeof *h);
    h->last_sample_ms = now_ms;
    h->last_restart_ms = now_ms - SK_RESTART_GAP_MS;
}
int sk_health_burst(struct sk_health *h, sk_u32 now_ms, int net_ok, sk_u32 total_sent, sk_u32 total_recv)
{
    const int silent = total_sent > 0 && total_recv == 0, bad = !net_ok || silent;
    h->ev = SK_EV_NONE; h->ev_a = h->ev_b = 0;
    h->last_sample_ms = now_ms;
    if (bad) {
        sk_u32 need = net_ok ? SK_RESTART_AFTER_BAD : 1u;
        h->bad_bursts++;
        if (!h->down) { h->down = 1; h->down_since_ms = now_ms; h->drops++; h->ev = SK_EV_DROP; h->ev_a = net_ok ? 2u : 1u; }
        if (silent && net_ok && !h->in_blackout) { h->in_blackout = 1; h->blackouts++; }
        if (h->bad_bursts >= need && (sk_u32)(now_ms - h->last_restart_ms) >= SK_RESTART_GAP_MS) { h->restarts++; return SK_ACT_RESTART; }
        return SK_ACT_NONE;
    }
    if (h->down) {
        sk_u32 d = now_ms - h->down_since_ms;
        h->down = 0; h->down_total_ms += d; if (d > h->down_max_ms) h->down_max_ms = d;
        h->ev = SK_EV_UP; h->ev_a = d; h->ev_b = h->bad_bursts;
    }
    h->bad_bursts = 0; h->in_blackout = 0;
    return SK_ACT_NONE;
}
int sk_health_watchdog(struct sk_health *h, sk_u32 now_ms)
{
    if ((sk_u32)(now_ms - h->last_sample_ms) < SK_WATCHDOG_MS) return 0;
    h->wd++; h->restarts++;
    h->last_sample_ms = now_ms;                                           /* one trigger per silent minute, not one per call */
    return 1;
}
void sk_health_restarted(struct sk_health *h, sk_u32 now_ms) { h->last_restart_ms = now_ms; h->last_sample_ms = now_ms; }

void sk_session_id(char out[9], sk_u32 a, sk_u32 b)
{
    static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    sk_u32 x = a * 2654435761u + b * 40503u + 0x9E3779B9u, i;
    for (i = 0; i < 8; i++) { x = x * 1664525u + 1013904223u; out[i] = dig[(x >> 16) % 36u]; }
    out[8] = 0;
}

static sk_u32 fin(int n, sk_u32 cap) { return (n > 0 && (sk_u32)n < cap) ? (sk_u32)n : 0; }

sk_u32 sk_fmt_sample(char *o, sk_u32 cap, struct sk_ctx *c, sk_u32 net_st, const struct sk_health *h, sk_u32 ul_ok, sk_u32 ul_fail, sk_u32 sd_fail, const struct sk_stats st[SK_LEGS])
{
    sk_u32 l;
    int n = snprintf(o, cap, SK_VER ",S,%s,%lu,%lu,%lu,%d,%lu,%lu,%lu,%lu,%lu", c->session, UL(c->seq), UL(c->up_s), UL(c->wall), (int)net_st, UL(h->drops), UL(h->restarts), UL(ul_ok), UL(ul_fail), UL(sd_fail));
    for (l = 0; l < SK_LEGS && n > 0 && (sk_u32)n < cap; l++)
        n += snprintf(o + n, cap - (sk_u32)n, ",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", UL(st[l].sent), UL(st[l].recv), UL(st[l].min), UL(st[l].p50), UL(st[l].p95), UL(st[l].p99), UL(st[l].max), UL(st[l].jitter), UL(st[l].gap_ms));
    c->seq++;
    return fin(n, cap);
}
sk_u32 sk_fmt_tls(char *o, sk_u32 cap, struct sk_ctx *c, int ok, sk_u32 total_ms, sk_u32 conn_ms, sk_u32 hs_ms, sk_u32 http, const char *why)
{
    char w[48];
    int n;
    sk_clean(w, sizeof w, why);
    n = snprintf(o, cap, SK_VER ",T,%s,%lu,%lu,%lu,%d,%lu,%lu,%lu,%lu,%s", c->session, UL(c->seq), UL(c->up_s), UL(c->wall), ok ? 1 : 0, UL(total_ms), UL(conn_ms), UL(hs_ms), UL(http), w);
    c->seq++;
    return fin(n, cap);
}
sk_u32 sk_fmt_net(char *o, sk_u32 cap, struct sk_ctx *c, sk_u32 net_st, int link_rc, sk_u32 link_word, const struct sk_health *h, sk_u32 tls_ok, sk_u32 tls_fail, sk_u32 ul_ok, sk_u32 ul_fail, sk_u32 q_drop, sk_u32 sd_fail)
{
    int n = snprintf(o, cap, SK_VER ",N,%s,%lu,%lu,%lu,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", c->session, UL(c->seq), UL(c->up_s), UL(c->wall), (int)net_st, link_rc, UL(link_word),
                     UL(h->drops), UL(h->restarts), UL(h->blackouts), UL(h->wd), UL((h->down_total_ms + 500u) / 1000u), UL((h->down_max_ms + 500u) / 1000u), UL(tls_ok), UL(tls_fail), UL(ul_ok), UL(ul_fail), UL(q_drop), UL(sd_fail));
    c->seq++;
    return fin(n, cap);
}
sk_u32 sk_fmt_event(char *o, sk_u32 cap, struct sk_ctx *c, const char *type, sk_u32 a, sk_u32 b)
{
    char t[16];
    int n;
    sk_clean(t, sizeof t, type);
    n = snprintf(o, cap, SK_VER ",E,%s,%lu,%lu,%lu,%s,%lu,%lu", c->session, UL(c->seq), UL(c->up_s), UL(c->wall), t, UL(a), UL(b));
    c->seq++;
    return fin(n, cap);
}
sk_u32 sk_fmt_upload(char *o, sk_u32 cap, struct sk_ctx *c, int ok, sk_u32 total_ms, sk_u32 conn_ms, sk_u32 hs_ms, sk_u32 http, sk_u32 bytes, sk_u32 lines, sk_u32 skipped, const char *why)
{
    char w[48];
    int n;
    sk_clean(w, sizeof w, why);
    n = snprintf(o, cap, SK_VER ",U,%s,%lu,%lu,%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s", c->session, UL(c->seq), UL(c->up_s), UL(c->wall), ok ? 1 : 0, UL(total_ms), UL(conn_ms), UL(hs_ms), UL(http), UL(bytes), UL(lines), UL(skipped), w);
    c->seq++;
    return fin(n, cap);
}
void sk_clean(char *dst, sk_u32 cap, const char *src)
{
    sk_u32 i = 0;
    if (!cap) return;
    if (!src) src = "";
    for (; src[i] && i < 40u && i + 1u < cap; i++) { unsigned char ch = (unsigned char)src[i]; dst[i] = (ch >= 0x20 && ch < 0x7F && ch != ',') ? (char)ch : '_'; }
    dst[i] = 0;
}

void sk_q_init(struct sk_queue *q) { memset(q, 0, sizeof *q); }
void sk_q_push(struct sk_queue *q, const char *line)
{
    sk_u32 at, n = (sk_u32)strlen(line);
    if (n >= SK_LINE_MAX) n = SK_LINE_MAX - 1u;
    if (q->count == SK_QMAX) {
        if (!q->sent[q->head]) { q->dropped++; q->unsent--; }
        q->head = (q->head + 1u) % SK_QMAX; q->count--;
    }
    at = (q->head + q->count) % SK_QMAX;
    memcpy(q->l[at], line, n); q->l[at][n] = 0; q->sent[at] = 0;
    q->count++; q->unsent++;
}
sk_u32 sk_q_build(const struct sk_queue *q, char *out, sk_u32 cap, sk_u32 *n_out, sk_u32 *skipped)
{
    sk_u32 take = 0, bytes = 0, i, first, at, len;
    while (take < q->unsent) {                                            /* newest first: the unsent lines are the newest suffix of the ring */
        at = (q->head + q->count - 1u - take) % SK_QMAX;
        len = (sk_u32)strlen(q->l[at]) + 1u;
        if (bytes + len > cap) break;
        bytes += len; take++;
    }
    first = q->count - take;
    bytes = 0;
    for (i = 0; i < take; i++) {
        at = (q->head + first + i) % SK_QMAX;
        len = (sk_u32)strlen(q->l[at]);
        memcpy(out + bytes, q->l[at], len); bytes += len; out[bytes++] = '\n';
    }
    *n_out = take; *skipped = q->unsent - take;
    return bytes;
}
void sk_q_done(struct sk_queue *q, int ok, sk_u32 skipped)
{
    sk_u32 i;
    if (!ok) return;
    for (i = 0; i < q->count; i++) q->sent[(q->head + i) % SK_QMAX] = 1;
    q->unsent = 0; q->skipped += skipped;
}

sk_u32 sk_part_name(char *o, sk_u32 cap, const char *session, sk_u32 part)
{
    int n;
    if (part <= 1u) n = snprintf(o, cap, "sd:/multishine/soak/soak-%s.csv", session);
    else n = snprintf(o, cap, "sd:/multishine/soak/soak-%s.%lu.csv", session, UL(part));
    return fin(n, cap);
}
int sk_need_rotate(sk_u32 cur_size, sk_u32 add) { return cur_size > 0 && (unsigned long long)cur_size + add > SK_ROTATE_BYTES; }

static int has_word(const char *t, sk_u32 len, const char *w)
{
    sk_u32 i, k, m = (sk_u32)strlen(w);
    for (i = 0; i + m <= len; i++) {
        for (k = 0; k < m; k++) { char ch = t[i + k]; if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32); if (ch != w[k]) break; }
        if (k == m) return 1;
    }
    return 0;
}
sk_u32 sk_parse_opts(const char *txt, sk_u32 len)
{
    sk_u32 f = 0;
    if (!txt) return 0;
    if (has_word(txt, len, "link")) f |= SK_O_LINK;
    if (has_word(txt, len, "noupload")) f |= SK_O_NOUP;
    return f;
}

sk_u32 sk_body_head(char *o, sk_u32 cap, const char *cid, const char *session, sk_u32 shell_seq, sk_u32 n_lines, sk_u32 skipped, sk_u32 up_s)
{
    int n = snprintf(o, cap, "MSDIAG1\nconsole %s\nsoak 1 session %s shell %lu lines %lu skipped %lu up_s %lu\nreason soak\n", cid, session, UL(shell_seq), UL(n_lines), UL(skipped), UL(up_s));
    return fin(n, cap);
}
