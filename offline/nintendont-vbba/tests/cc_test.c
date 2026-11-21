/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/* core/vbba_cc.c: the Connection check against a simulated network (loss, reorder, jitter, burst gaps, duplicates, no echo, no network, busy, cancel),
   the mailbox protocol (token edges, seqlock order, which side may write each word), the statistics and the verdict thresholds.  Fake clock, 1 ms steps. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../core/vbba_cc.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

#define SRV_IP 0x5BC0A801u
#define SRV_PORT 43151u
#define MAXQ 512

/* ---- simulated world ---- */
static vbba_u32 now_us;
static vbba_u32 mbm[VBBA_CC_WORDS];                  /* the mailbox memory */
static int pub_n; static struct { vbba_u32 first, n, seq_before, seq_after; } publog[8192];
static int bad_publish;                              /* a publish touched a PPC-owned word */
static vbba_u32 guest_tx_count;
static vbba_u32 arm5, arm20;
static vbba_u32 link_kind = VBBA_CC_L_WIFI, wsig_word;
static vbba_u32 st_n, st_sum, st_max, restored_max, restore_calls;
static int net_up = 1, open_fail, open_delay_ms, closes, opens, handle_open;
static vbba_u32 open_at_us;
static int foreign_ip, wrong_session;

struct pend { vbba_u32 due, len, ip; vbba_u16 port; int used; vbba_u8 b[VBBA_CC_SIZE_MAX]; };
static struct pend pq[MAXQ];
static vbba_u32 rng = 12345;
static vbba_u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* a scenario decides, per sent datagram, how many copies come back and after how long (us) */
typedef int (*plan_fn)(vbba_u32 seq, vbba_u32 t_send_us, vbba_u32 *delay);
static plan_fn plan;

static vbba_u32 now_fn(void *ctx) { (void)ctx; return now_us; }
static void fetch_fn(void *ctx, vbba_u32 first, vbba_u32 n, vbba_u32 *dst) { (void)ctx; memcpy(dst, mbm + first, n * 4u); }
static void publish_fn(void *ctx, vbba_u32 first, vbba_u32 n, const vbba_u32 *src)
{
    (void)ctx;
    if (first + n > VBBA_CC_ARM_WORDS) bad_publish++;                              /* words 24..27 (REQ, CANCEL, IP, PORT) belong to the PPC, in a cache line of their own */
    if (first == CC_W_MAGIC && n != 2) bad_publish++;
    if (first == CC_W_VER) bad_publish++;
    if (pub_n < 8192) { publog[pub_n].first = first; publog[pub_n].n = n; publog[pub_n].seq_before = mbm[CC_W_SEQ]; }
    memcpy(mbm + first, src, n * 4u);
    if (pub_n < 8192) { publog[pub_n].seq_after = mbm[CC_W_SEQ]; pub_n++; }
}
static vbba_u32 gtx_fn(void *ctx) { (void)ctx; return guest_tx_count; }
static void txlat_fn(void *ctx, struct vbba_cc_txlat *o, int reset_max) { (void)ctx; o->n = st_n; o->sum_us = st_sum; o->max_us = st_max; if (reset_max) st_max = 0; }
static void restore_fn(void *ctx, vbba_u32 old) { (void)ctx; restore_calls++; restored_max = old; if (st_max < old) st_max = old; }
static void arm_fn(void *ctx, vbba_u32 *a, vbba_u32 *b) { (void)ctx; *a = arm5; *b = arm20; }
static vbba_u32 link_fn(void *ctx) { (void)ctx; return link_kind; }
static vbba_u32 wsig_fn(void *ctx) { (void)ctx; return wsig_word; }

static void tp_status(void *ctx, struct vbba_tp_status *o) { (void)ctx; o->up = (vbba_u8)net_up; o->ip = 0xC0A80164u; o->mask = o->gw = 0; }
static int tp_open(void *ctx, vbba_u16 p)
{
    (void)ctx; (void)p;
    if (open_fail) return -1;
    opens++; handle_open = 1; open_at_us = now_us + (vbba_u32)open_delay_ms * 1000u;
    return 0;
}
static int tp_state(void *ctx, int h)
{
    (void)ctx; (void)h;
    if (!handle_open) return VBBA_TP_FREE;
    if (open_delay_ms < 0) return VBBA_TP_OPENING;                                /* never opens */
    return (vbba_s32)(now_us - open_at_us) >= 0 ? VBBA_TP_OPEN : VBBA_TP_OPENING;
}
static void enqueue(const vbba_u8 *req, vbba_u32 len, vbba_u32 due, vbba_u32 ip, vbba_u16 port)
{
    int i;
    for (i = 0; i < MAXQ; i++) if (!pq[i].used) {
        memcpy(pq[i].b, req, len); pq[i].len = len; pq[i].due = due; pq[i].ip = ip; pq[i].port = port; pq[i].used = 1;
        pq[i].b[5] = 1;                                                             /* the server turns the request into a reply */
        pq[i].b[20] = 0; pq[i].b[21] = 0; pq[i].b[22] = 0x12; pq[i].b[23] = 0x34;
        if (wrong_session) pq[i].b[11] ^= 0x55;
        return;
    }
}
static int tp_send(void *ctx, int h, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u16 len)
{
    vbba_u32 delay[4], n, i, seq = ((vbba_u32)d[12] << 24) | ((vbba_u32)d[13] << 16) | ((vbba_u32)d[14] << 8) | d[15];
    (void)ctx; (void)h;
    CHECK(ip == SRV_IP && port == SRV_PORT);
    CHECK(len == 80 || len >= VBBA_CC_SIZE_MIN);
    n = (vbba_u32)plan(seq, now_us, delay);
    for (i = 0; i < n; i++) enqueue(d, len, now_us + delay[i], foreign_ip ? 0x01010101u : SRV_IP, SRV_PORT);
    return VBBA_OK;
}
static int tp_recv(void *ctx, int h, vbba_u8 *buf, vbba_u16 cap, vbba_u32 *ip, vbba_u16 *port)
{
    int i, best = -1;
    (void)ctx; (void)h;
    for (i = 0; i < MAXQ; i++) if (pq[i].used && (vbba_s32)(now_us - pq[i].due) >= 0 && (best < 0 || (vbba_s32)(pq[i].due - pq[best].due) < 0)) best = i;
    if (best < 0) return 0;
    pq[best].used = 0;
    if (pq[best].len > cap) return VBBA_TP_TRUNC;
    memcpy(buf, pq[best].b, pq[best].len); *ip = pq[best].ip; *port = pq[best].port;
    return (int)pq[best].len;
}
static void tp_close(void *ctx, int h) { (void)ctx; (void)h; closes++; handle_open = 0; memset(pq, 0, sizeof pq); }

static const struct vbba_transport tp = { 0, now_fn, tp_status, 0, tp_open, tp_state, 0, tp_send, tp_recv, tp_close, 0, 1, 0, 0, 0, 0, 0, 0 };
static const struct vbba_cc_hooks hooks = { 0, now_fn, fetch_fn, publish_fn, gtx_fn, txlat_fn, restore_fn, arm_fn, link_fn, wsig_fn };
static struct vbba_cc cc;

static void reset_world(void)
{
    memset(mbm, 0, sizeof mbm); memset(pq, 0, sizeof pq); pub_n = 0; bad_publish = 0; guest_tx_count = 0; arm5 = arm20 = 0; link_kind = VBBA_CC_L_WIFI; wsig_word = 0x40302010u;
    st_n = st_sum = st_max = 0; restored_max = restore_calls = 0; net_up = 1; open_fail = 0; open_delay_ms = 0; closes = opens = handle_open = 0; foreign_ip = wrong_session = 0; rng = 12345;
    now_us = 1000000u; memset(&cc, 0, sizeof cc);
    vbba_cc_init(&cc, &hooks, &tp);
}
static vbba_u32 tok;
static void request(vbba_u32 ip, vbba_u32 port_word)
{
    mbm[CC_W_IP] = ip; mbm[CC_W_PORT] = port_word; mbm[CC_W_REQ] = ++tok;            /* the PPC writes REQ last */
}
static void run_ms(vbba_u32 ms, int guest_rate_per_s)
{
    vbba_u32 i, acc = 0;
    for (i = 0; i < ms; i++) {
        now_us += 1000u;
        acc += (vbba_u32)guest_rate_per_s;
        while (acc >= 1000u) { acc -= 1000u; guest_tx_count++; }
        vbba_cc_step(&cc);
    }
}
static void run_until_done(vbba_u32 max_ms, int guest_rate)
{
    vbba_u32 i;
    for (i = 0; i < max_ms && mbm[CC_W_VERDICT] <= VBBA_CC_V_RUNNING; i++) run_ms(1, guest_rate);
}
static vbba_u32 hi(vbba_u32 w) { return w >> 16; }
static vbba_u32 lo(vbba_u32 w) { return w & 0xFFFFu; }
static void start_default(void) { request(SRV_IP, SRV_PORT); }

/* ---- plans ---- */
static int p_const20(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; d[0] = 20000u; return 1; }
static int p_loss1(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; if (rnd() % 1000u < 10u) return 0; d[0] = 20000u; return 1; }
static int p_loss3(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; if (rnd() % 1000u < 30u) return 0; d[0] = 20000u; return 1; }
static int p_slow70(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; d[0] = (rnd() % 100u < 10u) ? 80000u : 30000u; return 1; }        /* p95 80 ms */
static int p_slow120(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; d[0] = (rnd() % 100u < 10u) ? 130000u : 30000u; return 1; }
static int p_gap150(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; if (t >= 1000000u + 20000000u && t < 1000000u + 20000000u + 150000u) return 0; d[0] = 20000u; return 1; }
static int p_gap300(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; if (t >= 1000000u + 20000000u && t < 1000000u + 20000000u + 300000u) return 0; d[0] = 20000u; return 1; }
static int p_reorder(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)t; d[0] = (s % 10u == 3u) ? 40000u : 20000u; return 1; }       /* every 10th arrives 20 ms late: overtaken */
static int p_dup(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)t; d[0] = 20000u; if (s % 10u == 0u) { d[1] = 25000u; return 2; } return 1; }
static int p_none(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; (void)t; (void)d; return 0; }
static int p_alt(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)t; d[0] = (s & 1u) ? 40000u : 20000u; return 1; }
static int p_late(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)t; d[0] = (s >= 2690u) ? 2500000u : 20000u; return 1; }              /* the last ones come after the drain time */
static int p_tail_dead(vbba_u32 s, vbba_u32 t, vbba_u32 *d) { (void)s; if (t > 1000000u + 40000000u) return 0; (void)d; d[0] = 20000u; return 1; }   /* the last 5 s are silent */

static const struct { const char *name; plan_fn p; vbba_u32 verdict; } scen[] = {
    { "clean 20 ms", p_const20, VBBA_CC_V_GOOD }, { "loss 1 %", p_loss1, VBBA_CC_V_OKAY }, { "loss 3 %", p_loss3, VBBA_CC_V_POOR },
    { "p95 80 ms", p_slow70, VBBA_CC_V_OKAY }, { "p95 130 ms", p_slow120, VBBA_CC_V_POOR }, { "gap 150 ms", p_gap150, VBBA_CC_V_OKAY },
    { "gap 300 ms", p_gap300, VBBA_CC_V_POOR }, { "reorder", p_reorder, VBBA_CC_V_GOOD }, { "duplicates", p_dup, VBBA_CC_V_GOOD },
};

static void test_scenarios(void)
{
    size_t i;
    for (i = 0; i < sizeof scen / sizeof scen[0]; i++) {
        reset_world(); plan = scen[i].p; start_default(); run_until_done(60000, 0);
        printf("   %-14s verdict %u sent %u recv %u lost %u ooo %u dup %u p50 %u p95 %u p99 %u (0.1 ms) gaps %u/%u\n", scen[i].name, mbm[CC_W_VERDICT], mbm[CC_R_SENT], mbm[CC_R_RECV], mbm[CC_R_LOST],
               hi(mbm[CC_R_OOODUP]), lo(mbm[CC_R_OOODUP]), hi(mbm[CC_R_P50P95]), lo(mbm[CC_R_P50P95]), hi(mbm[CC_R_P99JIT]), hi(mbm[CC_R_GAPS]), lo(mbm[CC_R_GAPS]));
        CHECK(mbm[CC_W_VERDICT] == scen[i].verdict);
        CHECK(mbm[CC_W_ACK] == mbm[CC_W_REQ]);
        CHECK(hi(mbm[CC_W_STATE]) >> 8 == VBBA_CC_P_DONE);
        CHECK(mbm[CC_R_SENT] >= 2690u && mbm[CC_R_SENT] <= 2701u);                  /* 45 s at 60 per second */
        CHECK((mbm[CC_W_SEQ] & 1u) == 0 && !bad_publish && closes == 1);
        CHECK(mbm[CC_R_RECV] + mbm[CC_R_LOST] == mbm[CC_R_SENT]);
    }
}

static void test_details(void)
{
    /* clean run: every number */
    reset_world(); plan = p_const20; arm5 = 100; arm20 = 7; st_n = 1000; st_sum = 4000000u; st_max = 90000u;
    start_default(); run_ms(30, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_RUNNING && mbm[CC_W_ACK] == mbm[CC_W_REQ] && (mbm[CC_W_STATE] >> 24) == VBBA_CC_P_RUN);
    CHECK(st_max == 0);                                                              /* the transport's running max was zeroed for the run */
    st_n += 2700; st_sum += 2700u * 4000u; st_max = 12000u; arm5 += 3; arm20 += 1;   /* what the run adds */
    run_until_done(60000, 0);
    CHECK(mbm[CC_R_LOST] == 0 && mbm[CC_R_OOODUP] == 0 && mbm[CC_R_GAPS] == 0);
    CHECK(hi(mbm[CC_R_P50P95]) >= 199 && hi(mbm[CC_R_P50P95]) <= 205 && lo(mbm[CC_R_P50P95]) >= 199 && lo(mbm[CC_R_P50P95]) <= 205);   /* 20 ms, clamped to min/max */
    CHECK(hi(mbm[CC_R_MINMAX]) >= 199 && lo(mbm[CC_R_MINMAX]) <= 215);
    CHECK(lo(mbm[CC_R_LGAVG]) >= 199 && lo(mbm[CC_R_LGAVG]) <= 205);                 /* mean 20 ms */
    CHECK(lo(mbm[CC_R_P99JIT]) <= 2);                                                /* no jitter */
    CHECK(hi(mbm[CC_R_LGAVG]) >= 16 && hi(mbm[CC_R_LGAVG]) <= 22);                   /* longest gap = the first echo (20 ms), then one send interval */
    CHECK(lo(mbm[CC_R_RUN]) >= 449 && lo(mbm[CC_R_RUN]) <= 470);                     /* 45 s; no drain wait because every echo is in */
    CHECK(hi(mbm[CC_R_SENDTO]) == 40 && lo(mbm[CC_R_SENDTO]) == 120);                /* mean 4.0 ms, max 12.0 ms (the run's own max) */
    CHECK(st_max == 90000u && restore_calls == 1 && restored_max == 90000u);         /* the game's older max is back */
    CHECK(mbm[CC_R_ARM] == ((3u << 16) | 1u));
    CHECK(mbm[CC_R_WS_N] >> 16 >= 170 && mbm[CC_R_WS_N] >> 16 <= 181);               /* every 250 ms for 45 s */
    CHECK(mbm[CC_R_WS_MIN] == 0x40302010u && mbm[CC_R_WS_MEAN] == 0x40302010u && mbm[CC_R_WS_LAST] == 0x40302010u);
    CHECK((mbm[CC_W_STATE] & 0xFFu) == VBBA_CC_L_WIFI && ((mbm[CC_W_STATE] >> 8) & 0xFFu) == 100);
    CHECK(mbm[CC_W_MAGIC] == VBBA_CC_MAGIC && mbm[CC_W_VER] == ((VBBA_CC_ABI << 16) | 28u));

    /* publish order: results under an odd/even seqlock, status (verdict) after the final even seq; the PPC-owned words never written */
    {
        int i, last_res = -1, last_status = -1, odd_seen = 0;
        for (i = 0; i < pub_n; i++) {
            if (publog[i].first == CC_R_SENT && publog[i].n == 18u) { last_res = i; CHECK((publog[i].seq_before & 1u) == 1u); }   /* the seq was odd when the results went out */
            if (publog[i].first == CC_W_SEQ && (publog[i].seq_after & 1u)) odd_seen++;
            if (publog[i].first == CC_W_ACK && publog[i].n == 3u) last_status = i;
        }
        CHECK(last_res > 0 && last_status > last_res);                              /* the final verdict is published after the final results */
        CHECK(publog[last_res + 1].first == CC_W_SEQ && (publog[last_res + 1].seq_after & 1u) == 0);
        CHECK(odd_seen >= 2 && !bad_publish);
    }

    /* jitter: alternating 20 / 40 ms settles near the 20 ms step */
    reset_world(); plan = p_alt; start_default(); run_until_done(60000, 0);
    CHECK(lo(mbm[CC_R_P99JIT]) > 150 && lo(mbm[CC_R_P99JIT]) < 210);
    CHECK(hi(mbm[CC_R_MINMAX]) >= 199 && lo(mbm[CC_R_MINMAX]) >= 399);

    /* reorder and duplicates */
    reset_world(); plan = p_reorder; start_default(); run_until_done(60000, 0);
    CHECK(hi(mbm[CC_R_OOODUP]) >= 200 && hi(mbm[CC_R_OOODUP]) <= 280 && lo(mbm[CC_R_OOODUP]) == 0 && mbm[CC_R_LOST] == 0);
    reset_world(); plan = p_dup; start_default(); run_until_done(60000, 0);
    CHECK(lo(mbm[CC_R_OOODUP]) >= 260 && lo(mbm[CC_R_OOODUP]) <= 280 && mbm[CC_R_LOST] == 0 && mbm[CC_R_RECV] == mbm[CC_R_SENT]);

    /* gaps: 150 ms hole = one >= 100 and no >= 250; 300 ms hole = both */
    reset_world(); plan = p_gap150; start_default(); run_until_done(60000, 0);
    CHECK(hi(mbm[CC_R_GAPS]) == 1 && lo(mbm[CC_R_GAPS]) == 0 && hi(mbm[CC_R_LGAVG]) >= 165 && hi(mbm[CC_R_LGAVG]) <= 185);
    reset_world(); plan = p_gap300; start_default(); run_until_done(60000, 0);
    CHECK(hi(mbm[CC_R_GAPS]) == 1 && lo(mbm[CC_R_GAPS]) == 1 && hi(mbm[CC_R_LGAVG]) >= 315 && hi(mbm[CC_R_LGAVG]) <= 335);

    /* silence at the end of the send window counts as a gap */
    reset_world(); plan = p_tail_dead; start_default(); run_until_done(60000, 0);
    CHECK(hi(mbm[CC_R_GAPS]) == 1 && lo(mbm[CC_R_GAPS]) == 1 && mbm[CC_W_VERDICT] == VBBA_CC_V_POOR);
    CHECK(hi(mbm[CC_R_LGAVG]) >= 4900 && hi(mbm[CC_R_LGAVG]) <= 5100);

    /* echoes after the drain time are lost (and the run does not wait for ever) */
    reset_world(); plan = p_late; start_default(); run_until_done(60000, 0);
    CHECK(mbm[CC_R_LOST] >= 9 && mbm[CC_R_LOST] <= 12 && lo(mbm[CC_R_RUN]) <= 470 + 16);

    /* wired: no signal samples */
    reset_world(); plan = p_const20; link_kind = VBBA_CC_L_WIRED; start_default(); run_until_done(60000, 0);
    CHECK((mbm[CC_W_STATE] & 0xFFu) == VBBA_CC_L_WIRED && mbm[CC_R_WS_N] >> 16 == 0 && mbm[CC_R_WS_MIN] == 0 && mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD);

    /* foreign datagrams (wrong source, wrong session) are counted and never counted as echoes */
    reset_world(); plan = p_const20; foreign_ip = 1; start_default(); run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && mbm[CC_R_RECV] == 0 && (mbm[CC_R_LATE] >> 16) > 100 && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_NOECHO);
    reset_world(); plan = p_const20; wrong_session = 1; start_default(); run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && mbm[CC_R_RECV] == 0 && (mbm[CC_R_LATE] >> 16) > 100);

    /* send schedule slips: the kernel is away for 200 ms */
    reset_world(); plan = p_const20; start_default(); run_ms(20000, 0); now_us += 200000u; run_until_done(60000, 0);
    CHECK(lo(mbm[CC_R_TXRX]) == 1 && hi(mbm[CC_R_RUN]) >= 200 && mbm[CC_R_SENT] < 2700u - 9u && hi(mbm[CC_R_GAPS]) == 1 && mbm[CC_W_VERDICT] == VBBA_CC_V_OKAY);   /* the 240 ms silence is a >= 100 ms gap, below 250 */
}

static void test_failures_and_mailbox(void)
{
    vbba_u32 t0;
    /* no echo at all: FAILED after about 10 s, not 45 */
    reset_world(); plan = p_none; start_default(); run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFFu) == VBBA_CC_R_NOECHO && mbm[CC_R_RECV] == 0);
    CHECK(mbm[CC_R_SENT] >= 590u && mbm[CC_R_SENT] <= 610u && lo(mbm[CC_R_RUN]) <= 110 + 16);
    CHECK(closes == 1);
    /* no network */
    reset_world(); net_up = 0; start_default(); run_ms(100, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_NONET && opens == 0 && mbm[CC_W_ACK] == mbm[CC_W_REQ]);
    /* bad address / port */
    reset_world(); request(0, SRV_PORT); run_ms(100, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_BADADDR);
    reset_world(); request(SRV_IP, 0x00300000u); run_ms(100, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_BADADDR);
    /* no socket: open fails, or never opens */
    reset_world(); open_fail = 1; start_default(); run_ms(100, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_NOSOCK);
    reset_world(); open_delay_ms = -1; plan = p_const20; start_default(); run_ms(4900, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_RUNNING && (mbm[CC_W_STATE] >> 24) == VBBA_CC_P_OPEN);
    run_ms(300, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_NOSOCK && closes == 1);
    /* a slow open (2 s) still runs */
    reset_world(); open_delay_ms = 2000; plan = p_const20; start_default(); run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD);
    /* busy: the guest sends 60 datagrams per second (a match); the history is there after one second */
    reset_world(); plan = p_const20; run_ms(1500, 60); start_default(); run_ms(100, 60);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_BUSY && opens == 0);
    /* a quiet menu (5 per second) is no objection */
    reset_world(); plan = p_const20; run_ms(1500, 5); start_default(); run_until_done(60000, 5);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD);
    /* the game starts a match during the run */
    reset_world(); plan = p_const20; start_default(); run_ms(10000, 0); run_ms(2000, 60);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_FAILED && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_GAME && closes == 1 && restore_calls == 1);

    /* cancel */
    reset_world(); plan = p_const20; st_max = 55000u; start_default(); run_ms(5000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_RUNNING && mbm[CC_R_SENT] == 0);            /* results are only published at the end (cleared at the start) */
    mbm[CC_W_CANCEL] = 77;
    run_ms(30, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_NOTRUN && ((mbm[CC_W_STATE] >> 16) & 0xFF) == VBBA_CC_R_CANCEL && closes == 1 && st_max == 55000u && (mbm[CC_W_STATE] >> 24) == VBBA_CC_P_DONE);
    /* a cancel written while idle is forgotten: the next test is not cancelled */
    mbm[CC_W_CANCEL] = 78; run_ms(100, 0);
    start_default(); run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD);

    /* a stale request token at kernel start is not a request; a new token starts a second run, whose result replaces the first */
    memset(mbm, 0, sizeof mbm); mbm[CC_W_REQ] = 999; mbm[CC_W_IP] = SRV_IP; mbm[CC_W_PORT] = SRV_PORT; mbm[CC_W_CANCEL] = 5; closes = opens = 0; handle_open = 0; memset(&cc, 0, sizeof cc);
    plan = p_const20; vbba_cc_init(&cc, &hooks, &tp); run_ms(200, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_NOTRUN && opens == 0 && mbm[CC_W_ACK] == 999u && mbm[CC_W_MAGIC] == VBBA_CC_MAGIC);
    mbm[CC_W_REQ] = 1000; run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD && mbm[CC_W_ACK] == 1000u);
    t0 = mbm[CC_R_SENT];
    plan = p_loss3; mbm[CC_W_REQ] = 1001; run_ms(100, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_RUNNING && mbm[CC_R_SENT] == 0 && t0 > 2000u);   /* the old result is gone as soon as the new test is taken */
    run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_POOR);
    /* a REQ written during a run is ignored until it has finished (documented: do not) */
    reset_world(); plan = p_const20; start_default(); run_ms(2000, 0); mbm[CC_W_REQ] = tok + 5;
    run_until_done(60000, 0);
    CHECK(mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD && closes == 1);
    run_ms(100, 0);
    CHECK(mbm[CC_W_ACK] == mbm[CC_W_REQ] && opens == 2);                             /* then it is taken as a new request */
    run_until_done(60000, 0);

    /* parameters: duration and datagram size, clamped */
    reset_world(); plan = p_const20; request(SRV_IP, SRV_PORT | (12u << 16) | (120u << 24)); run_until_done(60000, 0);
    CHECK(mbm[CC_R_SENT] >= 715u && mbm[CC_R_SENT] <= 725u && mbm[CC_W_VERDICT] == VBBA_CC_V_GOOD);
    reset_world(); plan = p_const20; request(SRV_IP, SRV_PORT | (1u << 16) | (255u << 24)); run_until_done(60000, 0);
    CHECK(mbm[CC_R_SENT] >= 595u && mbm[CC_R_SENT] <= 605u);                         /* 1 s clamps to 10 s */
}

static void test_units(void)
{
    struct vbba_cc_result r;
    vbba_u16 h[VBBA_CC_HIST_N];
    vbba_u8 p[100]; vbba_u32 sess, seq, t, srv;
    memset(&r, 0, sizeof r);
    /* verdict boundaries */
    r.sent = 2700; r.recv = 2700; r.p95 = 59999; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_GOOD);
    r.p95 = 60000; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_OKAY);
    r.p95 = 99999; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_OKAY);
    r.p95 = 100000; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_POOR);
    r.p95 = 20000; r.recv = 2700 - 13; r.lost = 13; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_GOOD);            /* 0.48 % */
    r.recv = 2700 - 14; r.lost = 14; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_OKAY);                           /* 0.52 % */
    r.recv = 2700 - 53; r.lost = 53; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_OKAY);                           /* 1.96 % */
    r.recv = 2700 - 54; r.lost = 54; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_POOR);                           /* 2.00 % */
    r.recv = 2700; r.lost = 0; r.gap100 = 1; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_OKAY);
    r.gap250 = 1; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_POOR);
    r.gap100 = r.gap250 = 0; r.sent = 2400; r.recv = 2400; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_POOR);       /* only 88.9 % of the schedule */
    r.sent = 2430; r.recv = 2430; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_GOOD);                              /* 90 % */
    r.recv = 0; CHECK(vbba_cc_verdict_of(&r, 2700) == VBBA_CC_V_FAILED);
    /* histogram index and percentiles */
    CHECK(vbba_cc_hist_idx(0) == 0 && vbba_cc_hist_idx(999) == 0 && vbba_cc_hist_idx(1000) == 1 && vbba_cc_hist_idx(99999) == 99);
    CHECK(vbba_cc_hist_idx(100000) == 100 && vbba_cc_hist_idx(109999) == 100 && vbba_cc_hist_idx(110000) == 101 && vbba_cc_hist_idx(2999999) == 118 && vbba_cc_hist_idx(3000000) == 119 && vbba_cc_hist_idx(60000000) == 119);
    memset(h, 0, sizeof h);
    h[10] = 90; h[50] = 9; h[110] = 1;                                              /* 90 at 10 ms, 9 at 50 ms, 1 in 300..350 ms */
    CHECK(vbba_cc_hist_pct(h, 100, 50, 10000, 330000) == 10500);
    CHECK(vbba_cc_hist_pct(h, 100, 90, 10000, 330000) == 10500);
    CHECK(vbba_cc_hist_pct(h, 100, 95, 10000, 330000) == 50500);
    CHECK(vbba_cc_hist_pct(h, 100, 99, 10000, 330000) == 50500);
    CHECK(vbba_cc_hist_pct(h, 100, 100, 10000, 330000) == 325000);
    CHECK(vbba_cc_hist_pct(h, 100, 100, 10000, 320000) == 320000);                  /* clamped to the exact max */
    CHECK(vbba_cc_hist_pct(h, 0, 50, 0, 0) == 0);
    /* packet */
    vbba_cc_pkt_build(p, 80, 0xA1B2C3D4u, 1234u, 987654u);
    CHECK(p[0] == 'M' && p[1] == 'S' && p[2] == 'E' && p[3] == 'C' && p[4] == 1 && p[5] == 0 && p[24] == 0 && p[25] == 80);
    CHECK(!vbba_cc_pkt_parse(p, 80, &sess, &seq, &t, &srv));                        /* a request is not a reply */
    p[5] = 1; p[20] = 0; p[21] = 0; p[22] = 1; p[23] = 2;
    CHECK(vbba_cc_pkt_parse(p, 80, &sess, &seq, &t, &srv) && sess == 0xA1B2C3D4u && seq == 1234u && t == 987654u && srv == 0x102u);
    CHECK(!vbba_cc_pkt_parse(p, 79, &sess, &seq, &t, &srv));                        /* length field differs */
    CHECK(!vbba_cc_pkt_parse(p, 20, &sess, &seq, &t, &srv) && !vbba_cc_pkt_parse(p, 201, &sess, &seq, &t, &srv));
    p[4] = 2; CHECK(!vbba_cc_pkt_parse(p, 80, &sess, &seq, &t, &srv)); p[4] = 1;
    p[0] = 'X'; CHECK(!vbba_cc_pkt_parse(p, 80, &sess, &seq, &t, &srv));
    /* layout constants the document and the PPC side rely on */
    CHECK(CC_W_MAGIC == 0 && CC_W_VER == 1 && CC_W_ACK == 2 && CC_W_VERDICT == 3 && CC_W_STATE == 4 && CC_W_SEQ == 5 && CC_R_SENT == 6 && CC_R_LATE == 23 && CC_W_REQ == 24 && CC_W_CANCEL == 25 && CC_W_IP == 26 && CC_W_PORT == 27 && VBBA_CC_WORDS == 28 && VBBA_CC_ARM_WORDS == 24 && (VBBA_CC_BASE & 31u) == 0 && ((VBBA_CC_BASE + 4u * CC_W_REQ) & 31u) == 0);
    CHECK(VBBA_CC_MAGIC == 0x4343484Bu && VBBA_CC_SEQ_MAX >= VBBA_CC_DURATION_MAX_S * 1000000u / VBBA_CC_INTERVAL_US);
}

int main(void)
{
    printf("== scenarios (45 s each, simulated)\n");
    test_scenarios();
    test_details();
    test_failures_and_mailbox();
    test_units();
    printf("cc_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
