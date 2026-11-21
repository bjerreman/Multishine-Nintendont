/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/* ota/ms_probe_core.c: DNS packet, matching by transaction id, statistics, legs and fallback, a router that never answers, noisy IOS calls, verdict boundaries.
   A simulated network with a fake microsecond clock; the caller loop is the one in the header comment. */
#include <stdio.h>
#include <string.h>
#include "../ms_probe_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

#define OWN 0xC0A80164u
#define GW 0xC0A80101u
#define DNS2 0xC0A80102u
#define CF 0x01010101u
#define GOOG 0x08080808u
#define SRV 0x5BC0A801u

struct tgt { msp_u32 ip; int answers; msp_u32 delay, delay_hi, hi_permille, loss_permille, hole_at, hole_len, t_first; int noqr; };
static struct tgt tg[6]; static int ntg;
struct pend { msp_u32 due, len, ip; int used; msp_u8 b[64]; };
static struct pend pq[256];
static msp_u32 rng = 7, now_us;
static msp_u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static msp_u32 first_ip, to_srv, sends_to[8][2];
static msp_u32 call_cost_us;                                /* simulated cost of one socket call, reported through note_call */

static struct tgt *find(msp_u32 ip) { int i; for (i = 0; i < ntg; i++) if (tg[i].ip == ip) return &tg[i]; return 0; }
static void add(msp_u32 ip, msp_u32 delay) { memset(&tg[ntg], 0, sizeof tg[ntg]); tg[ntg].ip = ip; tg[ntg].answers = 1; tg[ntg].delay = tg[ntg].delay_hi = delay; ntg++; }
static void silent(msp_u32 ip) { memset(&tg[ntg], 0, sizeof tg[ntg]); tg[ntg].ip = ip; ntg++; }
static void net_send(msp_u32 ip, const msp_u8 *d, msp_u32 len)
{
    struct tgt *t = find(ip); int i;
    if (!first_ip) first_ip = ip;
    if (ip == SRV) to_srv++;
    for (i = 0; i < 8; i++) if (sends_to[i][0] == ip || !sends_to[i][0]) { sends_to[i][0] = ip; sends_to[i][1]++; break; }
    if (!t || !t->answers) return;
    if (!t->t_first) t->t_first = now_us;
    if (rnd() % 1000u < t->loss_permille) return;
    if (t->hole_len && now_us - t->t_first >= t->hole_at && now_us - t->t_first < t->hole_at + t->hole_len) return;
    for (i = 0; i < 256; i++) if (!pq[i].used) {
        msp_u32 dl = (rnd() % 1000u < t->hi_permille) ? t->delay_hi : t->delay;
        memcpy(pq[i].b, d, len); pq[i].len = len; pq[i].due = now_us + dl; pq[i].ip = ip; pq[i].used = 1;
        if (!t->noqr) pq[i].b[2] |= 0x80u;
        pq[i].b[3] |= 0x80u;
        return;
    }
}
static struct msp_ladder L;
static void world(void) { memset(pq, 0, sizeof pq); ntg = 0; first_ip = to_srv = 0; memset(sends_to, 0, sizeof sends_to); rng = 7; now_us = 5000000u; call_cost_us = 0; }
/* the caller loop: 1 ms steps; returns the milliseconds it took */
static msp_u32 run(msp_u32 max_ms)
{
    msp_u32 ms = 0; msp_u8 buf[64];
    while (ms < max_ms) {
        int act = msp_poll(&L, now_us), i;
        if (act == MSP_ACT_DONE) break;
        if (act == MSP_ACT_SEND) {
            msp_u32 ip, port, n = msp_make_query(&L, buf, now_us, &ip, &port);
            CHECK(n == 27 && port == 53);
            net_send(ip, buf, n);
            if (call_cost_us) msp_note_call(&L, call_cost_us);
        }
        for (i = 0; i < 256; i++) if (pq[i].used && (msp_s32)(now_us - pq[i].due) >= 0) { pq[i].used = 0; (void)msp_on_reply(&L, pq[i].b, pq[i].len, pq[i].ip, 53, now_us); }
        now_us += 1000u; ms++;
    }
    return ms;
}
static struct msp_result R;

int main(void)
{
    msp_u8 p[64], q[64]; msp_u32 id, n, ms;
    CHECK(sizeof(msp_u32) == 4 && sizeof(msp_u16) == 2 && sizeof(msp_u8) == 1);

    /* DNS packet */
    n = msp_dns_build(p, 0xBEEF);
    CHECK(n == 27 && p[0] == 0xBE && p[1] == 0xEF && p[2] == 0x01 && p[4] == 0 && p[5] == 1 && p[12] == 9 && p[13] == 'l' && p[22] == 0 && p[24] == 1 && p[26] == 1);
    CHECK(!msp_dns_parse(p, n, &id));
    memcpy(q, p, n); q[2] |= 0x80; CHECK(msp_dns_parse(q, n, &id) && id == 0xBEEF);
    q[3] = 3; CHECK(msp_dns_parse(q, n, &id));              /* NXDOMAIN */
    q[3] = 2; CHECK(msp_dns_parse(q, n, &id));              /* SERVFAIL */
    q[3] = 5; CHECK(msp_dns_parse(q, n, &id));              /* REFUSED */
    memcpy(q, p, n); q[2] |= 0x88; CHECK(!msp_dns_parse(q, n, &id));
    memcpy(q, p, n); q[2] |= 0x80; q[16] ^= 1; CHECK(!msp_dns_parse(q, n, &id));
    memcpy(q, p, n); q[2] |= 0x80; CHECK(!msp_dns_parse(q, n - 1, &id));
    CHECK(msp_is_private(GW) && msp_is_private(0x0A000001u) && msp_is_private(0xAC100001u) && msp_is_private(0xAC1FFFFFu) && msp_is_private(0xA9FE0001u));
    CHECK(!msp_is_private(0xAC200001u) && !msp_is_private(0xAC0FFFFFu) && !msp_is_private(CF) && !msp_is_private(SRV));
    /* leg rules */
    msp_init(&L, 0, 1, 0, 0);
    CHECK(!msp_add_leg(&L, MSP_LEG_ROUTER, SRV) && !msp_add_leg(&L, MSP_LEG_RESOLVER, CF) && !msp_add_leg(&L, MSP_LEG_CF, SRV) && !msp_add_leg(&L, MSP_LEG_GOOGLE, CF) && !msp_add_leg(&L, 9, GW));
    CHECK(msp_default_legs(&L, OWN, GW, DNS2) == 4 && L.leg_ip[0] == GW && L.leg_ip[1] == DNS2 && L.leg_ip[2] == CF && L.leg_ip[3] == GOOG);
    msp_init(&L, 0, 1, 0, 0); CHECK(msp_default_legs(&L, OWN, 0, 0) == 2 && L.leg_id[0] == MSP_LEG_CF);
    msp_init(&L, 0, 1, 0, 0); CHECK(msp_default_legs(&L, OWN, OWN, 0x0A000001u) == 2);                         /* own address as gateway, resolver off the subnet: ignored */
    msp_init(&L, 0, 1, 0, 0); CHECK(msp_default_legs(&L, OWN, GW, GW) == 3 && L.leg_id[1] == MSP_LEG_CF);      /* resolver equal to the gateway: one leg */
    msp_init(&L, 0, 1, 0, 0); CHECK(msp_default_legs(&L, OWN, SRV, 0) == 2);                                    /* a public "gateway": ignored */
    msp_init(&L, 0, 1, 0, 0); CHECK(msp_poll(&L, 0) == MSP_ACT_DONE && L.res.verdict == MSP_V_FAILED && L.res.reason == MSP_R_NOLEG);

    /* real gateway answers at 4 ms: router leg decides */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 4000);
    ms = run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_GOOD && R.leg == MSP_LEG_ROUTER && R.leg_ip == GW && R.sent == 400 && R.lost == 0 && !R.router_silent && !R.noisy && msp_done(&L));
    CHECK(R.p50 >= 3500 && R.p50 <= 4500 && first_ip == GW && to_srv == 0 && ms < 22000);
    printf("   router 4 ms     verdict %u leg %u sent %u recv %u p50 %u p95 %u us in %u ms\n", R.verdict, R.leg, R.sent, R.recv, R.p50, R.p95, ms);
    /* no gateway known: 1.1.1.1 only, first send there */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, 0, 0); add(GW, 4000); add(CF, 9000);
    run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_GOOD && R.leg == MSP_LEG_CF && first_ip == CF && sends_to[1][0] == 0 && !R.router_silent);
    /* router never answers: the DHCP resolver */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, DNS2); silent(GW); add(DNS2, 12000);
    run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_GOOD && R.leg == MSP_LEG_RESOLVER && R.router_silent && sends_to[0][0] == GW && sends_to[0][1] <= 70u && sends_to[1][0] == DNS2);
    /* router and resolver silent: 1.1.1.1; the router_silent flag stays (the glue can say so) */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, DNS2); silent(GW); silent(DNS2); add(CF, 15000);
    ms = run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_GOOD && R.leg == MSP_LEG_CF && R.router_silent && ms > 26000 && ms < 32000);
    /* 1.1.1.1 silent too: 8.8.8.8 */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, 0, 0); add(GOOG, 20000);
    run(60000); msp_result(&L, &R); CHECK(R.verdict == MSP_V_GOOD && R.leg == MSP_LEG_GOOGLE);
    /* nobody answers: Failed, reason NOANSWER, within 4 legs x 3 s */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, DNS2);
    ms = run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_FAILED && R.reason == MSP_R_NOANSWER && R.router_silent && ms >= 12000 && ms <= 12200 && R.leg == 0 && msp_done(&L));
    printf("   nobody answers  verdict %u reason %u after %u ms\n", R.verdict, R.reason, ms);
    /* the same with only the Internet legs */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, 0, 0);
    ms = run(60000); msp_result(&L, &R); CHECK(R.verdict == MSP_V_FAILED && !R.router_silent && ms <= 6200);

    /* loss, gaps, spread on the router leg */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 5000); tg[0].loss_permille = 40; run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_POOR && R.lost > 0);
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 5000); tg[0].loss_permille = 18; run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_OKAY);
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 5000); tg[0].noqr = 1; run(60000); msp_result(&L, &R);
    CHECK(R.leg != MSP_LEG_ROUTER && R.router_silent);                                                        /* queries bounced back are not answers */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 5000); tg[0].hole_at = 8000000u; tg[0].hole_len = 150000u; run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_OKAY && R.gap100 == 1 && R.gap250 == 0);
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 5000); tg[0].hole_at = 8000000u; tg[0].hole_len = 300000u; run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_OKAY && R.gap250 == 1);                                                          /* one silence is forgiven */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 5000); tg[0].delay_hi = 120000; tg[0].hi_permille = 100; run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_POOR);
    /* a distant but steady Internet leg is Good (70 ms steady) */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, 0, 0); add(CF, 70000); run(60000); msp_result(&L, &R);
    CHECK(R.verdict == MSP_V_GOOD && R.gap100 == 0);
    /* noisy IOS calls: 100+ ms per call (here 120 ms for every call; the caller falls behind its cadence) -> loss only, capped at Okay */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 4000); call_cost_us = 120000u; run(60000); msp_result(&L, &R);
    CHECK(R.noisy && R.verdict == MSP_V_OKAY && R.slow_calls == R.sent && R.call_max_us == 120000u);
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 4000); tg[0].loss_permille = 60; call_cost_us = 120000u; run(60000); msp_result(&L, &R);
    CHECK(R.noisy && R.verdict == MSP_V_POOR);
    /* a few slow calls (under 5 %) do not make the run noisy */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); add(GW, 4000);
    { msp_u32 k; msp_poll(&L, now_us); for (k = 0; k < 100; k++) { msp_u8 b[32]; msp_u32 ip, pt; (void)msp_make_query(&L, b, now_us + k * 50000u, &ip, &pt); msp_note_call(&L, k < 4 ? 80000u : 1000u); } }
    msp_result(&L, &R); CHECK(R.slow_calls == 4 && R.sent == 100);
    /* foreign / stale / duplicate datagrams */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0);
    { msp_u8 b[32]; msp_u32 ip, pt, k; msp_poll(&L, now_us); k = msp_make_query(&L, b, now_us, &ip, &pt); b[2] |= 0x80;
      CHECK(!msp_on_reply(&L, b, k, SRV, 53, now_us + 1000u));                 /* wrong source address */
      CHECK(!msp_on_reply(&L, b, k, GW, 5353, now_us + 1000u));                /* wrong port */
      CHECK(msp_on_reply(&L, b, k, GW, 53, now_us + 1000u));
      CHECK(!msp_on_reply(&L, b, k, GW, 53, now_us + 2000u));                  /* duplicate */
      b[0] ^= 0x55; CHECK(!msp_on_reply(&L, b, k, GW, 53, now_us + 2000u));    /* an id we never sent */
      msp_result(&L, &R); CHECK(R.recv == 1 && R.dup == 0 && R.sent == 1); CHECK(L.s.dup == 1 && L.s.late == 3); }
    /* a leg switch leaves nothing of the old leg behind: late replies of the old address are ignored */
    world(); msp_init(&L, now_us, 3, 0, 0); msp_default_legs(&L, OWN, GW, 0); silent(GW); add(CF, 9000);
    run(60000); msp_result(&L, &R); printf("   after switch leg %u sent %u recv %u verdict %u\n", R.leg, R.sent, R.recv, R.verdict); CHECK(R.leg == MSP_LEG_CF && R.sent >= 395 && R.sent <= 401);

    /* verdict boundaries, hop rule (expected 400) */
    memset(&R, 0, sizeof R); R.sent = 400; R.recv = 400; R.p50 = 3000; R.p95 = 24999;
    CHECK(msp_verdict_hop(&R, 400) == MSP_V_GOOD);
    R.p95 = 25000; CHECK(msp_verdict_hop(&R, 400) == MSP_V_OKAY);
    R.p95 = 99999; CHECK(msp_verdict_hop(&R, 400) == MSP_V_OKAY);
    R.p95 = 100000; CHECK(msp_verdict_hop(&R, 400) == MSP_V_POOR);
    R.p95 = 5000; R.recv = 396; R.lost = 4; CHECK(msp_verdict_hop(&R, 400) == MSP_V_OKAY);                   /* 4/400 = 1.00 %: not below 1 % */
    R.sent = 1000; R.recv = 991; R.lost = 9; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_GOOD);                 /* 0.90 % */
    R.recv = 990; R.lost = 10; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_OKAY);                               /* 1.00 % */
    R.recv = 971; R.lost = 29; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_OKAY);                               /* 2.90 % */
    R.recv = 970; R.lost = 30; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_POOR);                               /* 3.00 % */
    R.recv = 1000; R.lost = 0; R.gap100 = 1; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_OKAY);
    R.gap250 = 1; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_OKAY);                                             /* one forgiven */
    R.gap250 = 2; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_POOR);
    R.gap100 = R.gap250 = 0; R.sent = R.recv = 849; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_POOR);          /* 84.9 % of the schedule */
    R.sent = R.recv = 850; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_GOOD);
    R.recv = 0; CHECK(msp_verdict_hop(&R, 1000) == MSP_V_FAILED);
    /* internet rule */
    memset(&R, 0, sizeof R); R.sent = 400; R.recv = 400; R.p50 = 60000; R.p95 = 79999;
    CHECK(msp_verdict_internet(&R, 400) == MSP_V_GOOD);
    R.p95 = 80000; CHECK(msp_verdict_internet(&R, 400) == MSP_V_OKAY);
    R.p50 = 30000; R.p95 = 59999; CHECK(msp_verdict_internet(&R, 400) == MSP_V_GOOD);                        /* spread 29.999 */
    R.p95 = 60000; CHECK(msp_verdict_internet(&R, 400) == MSP_V_OKAY);                                       /* spread 30 */
    R.p95 = 129999; CHECK(msp_verdict_internet(&R, 400) == MSP_V_OKAY);                                      /* spread 99.999 */
    R.p95 = 130000; CHECK(msp_verdict_internet(&R, 400) == MSP_V_POOR);
    R.p50 = 150000; R.p95 = 199999; CHECK(msp_verdict_internet(&R, 400) == MSP_V_OKAY);
    R.p95 = 200000; CHECK(msp_verdict_internet(&R, 400) == MSP_V_POOR);
    /* dispatch + noisy */
    R.p50 = 3000; R.p95 = 5000; R.noisy = 0; CHECK(msp_verdict(&R, MSP_LEG_ROUTER, 400) == MSP_V_GOOD);
    R.noisy = 1; CHECK(msp_verdict(&R, MSP_LEG_ROUTER, 400) == MSP_V_OKAY);
    R.noisy = 0; R.p95 = 110000; CHECK(msp_verdict(&R, MSP_LEG_ROUTER, 400) == MSP_V_POOR && msp_verdict(&R, MSP_LEG_CF, 400) == MSP_V_POOR);   /* spread 107 ms */
    R.p50 = 90000; R.p95 = 100000; CHECK(msp_verdict(&R, MSP_LEG_ROUTER, 400) == MSP_V_POOR && msp_verdict(&R, MSP_LEG_CF, 400) == MSP_V_OKAY);

    /* finish_now: the caller's hard limit */
    {
        static struct msp_ladder F;
        msp_u8 b[64]; msp_u32 ip, port, k;
        msp_init(&F, 1000, 7, 10000000u, 0); msp_default_legs(&F, 0xC0A80064u, 0xC0A80001u, 0);
        msp_finish_now(&F, 2000000u); { struct msp_result o; msp_result(&F, &o); CHECK(o.verdict == MSP_V_FAILED); }
        msp_init(&F, 1000, 7, 10000000u, 0); msp_default_legs(&F, 0xC0A80064u, 0xC0A80001u, 0);
        CHECK(msp_poll(&F, 1000) == MSP_ACT_SEND); k = msp_make_query(&F, b, 1000, &ip, &port);
        { msp_u8 r[64]; memcpy(r, b, k); r[2] |= 0x80; CHECK(msp_on_reply(&F, r, k, ip, port, 4000)); }
        msp_finish_now(&F, 5000000u); { struct msp_result o; msp_result(&F, &o); CHECK(o.verdict != MSP_V_RUNNING && o.verdict != MSP_V_NOTRUN && msp_done(&F)); }
    }

    /* should_run + cfg_kind */
    {
        int h = 9, lk = 9;
        static msp_u8 c[0x1B5C];
        CHECK(msp_should_run(MSP_NET_WIRED, 0, &h) == 0 && h == 0);
        CHECK(msp_should_run(MSP_NET_WIRED, 1, &h) == 0);
        CHECK(msp_should_run(MSP_NET_WIRED, MSP_NET_UNKNOWN, &h) == 0);
        CHECK(msp_should_run(MSP_NET_WIRELESS, 0, &h) == 1 && h == 0);
        CHECK(msp_should_run(MSP_NET_WIRELESS, 1, &h) == 1 && h == 1);              /* adapter plugged in, Wi-Fi profile: runs, with the hint */
        CHECK(msp_should_run(MSP_NET_WIRELESS, MSP_NET_UNKNOWN, &h) == 1 && h == 0);
        CHECK(msp_should_run(MSP_NET_UNKNOWN, 1, &h) == 1 && h == 0);               /* never skip on a guess; an adapter alone does not switch it off */
        CHECK(msp_should_run(MSP_NET_UNKNOWN, 0, &h) == 1 && h == 0);
        CHECK(msp_should_run(MSP_NET_UNKNOWN, MSP_NET_UNKNOWN, 0) == 1);
        memset(c, 0, sizeof c);
        CHECK(msp_cfg_kind(c, sizeof c, 0, &lk) == MSP_NET_UNKNOWN && lk == MSP_NET_UNKNOWN);     /* all zero: nothing consistent */
        CHECK(msp_cfg_kind(c, 0x1B5Bu, 0, &lk) == MSP_NET_UNKNOWN);
        CHECK(msp_cfg_kind(0, 100, 0, &lk) == MSP_NET_UNKNOWN);
        c[4] = 2; c[8] = 0x80; c[8 + 0x91C] = 0x81;                                               /* entry 2 wired, header says wired */
        CHECK(msp_cfg_kind(c, sizeof c, 0, &lk) == MSP_NET_WIRED && lk == MSP_NET_WIRED);
        CHECK(msp_cfg_kind(c, sizeof c, 1, &lk) == MSP_NET_WIRELESS && lk == MSP_NET_WIRED);      /* Nintendont row 1 is a Wi-Fi entry although the Wii's selected one is wired */
        CHECK(msp_cfg_kind(c, sizeof c, 2, 0) == MSP_NET_WIRED);
        CHECK(msp_cfg_kind(c, sizeof c, 3, 0) == MSP_NET_WIRELESS);
        c[4] = 1; CHECK(msp_cfg_kind(c, sizeof c, 0, &lk) == MSP_NET_WIRELESS && lk == MSP_NET_WIRELESS);
        c[4] = 7; CHECK(msp_cfg_kind(c, sizeof c, 0, &lk) == MSP_NET_WIRELESS && lk == MSP_NET_UNKNOWN);   /* header byte garbage: the selected entry (1, wireless) decides */
        c[8] = 0x81; CHECK(msp_cfg_kind(c, sizeof c, 0, 0) == MSP_NET_WIRED);
    }

    printf("probe_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
