/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_soak_core.[ch] (tests/test_soak.py builds and runs it).
 *   soak_test                asserts
 *   soak_test emit [N]       prints a complete diagnostics body (N sample lines + T/N/E/U lines + proof) to stdout; the Python test feeds it to the server's DiagStore and to the report parser
 *   soak_test lines          prints one line of each kind (the Python parser test reads them) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_soak_core.h"
#include "ms_diag_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void mkstats(struct sk_stats st[SK_LEGS], unsigned seed)
{
    unsigned l;
    for (l = 0; l < SK_LEGS; l++) {
        memset(&st[l], 0, sizeof st[l]);
        st[l].sent = 20; st[l].recv = 20 - (seed % 3); st[l].min = 1500 + l * 9000; st[l].p50 = 2500 + l * 10000 + seed; st[l].p95 = 9000 + l * 12000 + seed * 3; st[l].p99 = 30000 + seed * 7; st[l].max = 41000 + seed * 11;
        st[l].jitter = 1800 + seed; st[l].gap_ms = 100 + seed % 50;
    }
}

static void t_burst(void)
{
    struct sk_burst b;
    struct sk_stats s;
    unsigned i;
    sk_burst_init(&b, 0xC0A80A01u);
    for (i = 0; i < 20; i++) CHECK(sk_burst_send(&b, 1000000u + i * 50000u) == i);
    CHECK(sk_burst_send(&b, 3000000u) == 0xFFFFFFFFu);
    for (i = 0; i < 20; i++) if (i != 5 && i != 6) CHECK(sk_burst_reply(&b, i, 1000000u + i * 50000u + 2000u + i * 100u));
    CHECK(!sk_burst_reply(&b, 3, 1200000u));          /* duplicate */
    CHECK(!sk_burst_reply(&b, 25, 1200000u));         /* never sent */
    sk_burst_stats(&b, &s);
    CHECK(s.sent == 20 && s.recv == 18);
    CHECK(s.min == 2000 && s.max == 2000 + 19 * 100);
    CHECK(s.p50 >= 2000 && s.p50 <= 3900 && s.p95 <= s.p99 && s.p99 <= s.max && s.p50 <= s.p95);
    CHECK(s.gap_ms >= 150 && s.gap_ms < 156);         /* two lost probes: the replies around them are 150 ms apart */
    CHECK(s.jitter > 0 && s.jitter < 300);
    /* all lost */
    sk_burst_init(&b, 1);
    for (i = 0; i < 20; i++) sk_burst_send(&b, 5000000u + i * 50000u);
    sk_burst_stats(&b, &s);
    CHECK(s.recv == 0 && s.p50 == 0 && s.max == 0 && s.gap_ms == 950);
    /* nothing sent */
    sk_burst_init(&b, 1); sk_burst_stats(&b, &s); CHECK(s.sent == 0 && s.gap_ms == 0);
    /* clock wrap inside a burst */
    sk_burst_init(&b, 1);
    for (i = 0; i < 20; i++) sk_burst_send(&b, 0xFFFF0000u + i * 50000u);
    for (i = 0; i < 20; i++) sk_burst_reply(&b, i, 0xFFFF0000u + i * 50000u + 5000u);
    sk_burst_stats(&b, &s);
    CHECK(s.recv == 20 && s.min == 5000 && s.max == 5000 && s.jitter == 0 && s.gap_ms == 50);
    /* percentiles of a known set: 20 replies with rtt (i+1) ms */
    sk_burst_init(&b, 1);
    for (i = 0; i < 20; i++) sk_burst_send(&b, i * 50000u);
    for (i = 0; i < 20; i++) sk_burst_reply(&b, i, i * 50000u + (i + 1) * 1000u);
    sk_burst_stats(&b, &s);
    CHECK(s.p50 == 10000 && s.p95 == 19000 && s.p99 == 20000 && s.max == 20000 && s.min == 1000);
}

static void t_txid(void)
{
    sk_u32 leg, idx;
    CHECK(sk_txid_parse(sk_txid(7, 2, 19), 7, &leg, &idx) && leg == 2 && idx == 19);
    CHECK(!sk_txid_parse(sk_txid(7, 2, 19), 8, &leg, &idx));
    CHECK(sk_txid(0x1FF, 7, 31) == 0xFFFFu);
}

static void t_health(void)
{
    struct sk_health h;
    unsigned t = 100000, i;
    int a;
    sk_health_init(&h, t);
    CHECK(sk_health_burst(&h, t += 10000, 1, 60, 58) == SK_ACT_NONE && h.ev == SK_EV_NONE && h.drops == 0);
    /* silent bursts with the network "up": a drop at the first, restart at the third */
    a = sk_health_burst(&h, t += 10000, 1, 60, 0); CHECK(a == SK_ACT_NONE && h.ev == SK_EV_DROP && h.ev_a == 2 && h.drops == 1 && h.blackouts == 1);
    a = sk_health_burst(&h, t += 10000, 1, 60, 0); CHECK(a == SK_ACT_NONE && h.ev == SK_EV_NONE && h.drops == 1 && h.blackouts == 1);
    a = sk_health_burst(&h, t += 10000, 1, 60, 0); CHECK(a == SK_ACT_RESTART && h.restarts == 1);
    sk_health_restarted(&h, t += 12000);
    a = sk_health_burst(&h, t += 10000, 1, 60, 57); CHECK(a == SK_ACT_NONE && h.ev == SK_EV_UP && h.ev_b == 3 && h.down == 0 && h.ev_a == 42000 && h.down_total_ms == 42000);
    CHECK(h.down_max_ms == 42000);
    /* a bad network status restarts at once, and a second drop is a second drop */
    a = sk_health_burst(&h, t += 10000, 0, 0, 0); CHECK(a == SK_ACT_RESTART && h.drops == 2 && h.ev == SK_EV_DROP && h.ev_a == 1 && h.blackouts == 1);
    sk_health_restarted(&h, t);
    a = sk_health_burst(&h, t += 1000, 0, 0, 0); CHECK(a == SK_ACT_NONE);       /* less than SK_RESTART_GAP_MS since the last restart */
    a = sk_health_burst(&h, t += 6000, 0, 0, 0); CHECK(a == SK_ACT_RESTART);
    /* it must never wedge: a long outage keeps asking for restarts, spaced */
    for (i = 0; i < 20; i++) { a = sk_health_burst(&h, t += 10000, 0, 0, 0); CHECK(a == SK_ACT_RESTART); sk_health_restarted(&h, t); }
    /* watchdog */
    sk_health_init(&h, 0);
    CHECK(!sk_health_watchdog(&h, 59999));
    CHECK(sk_health_watchdog(&h, 60000) && h.wd == 1 && h.restarts == 1);
    CHECK(!sk_health_watchdog(&h, 61000));
    CHECK(sk_health_watchdog(&h, 120000) && h.wd == 2);
    sk_health_init(&h, 0xFFFFF000u);                                            /* wrap */
    CHECK(!sk_health_watchdog(&h, 10000) && sk_health_watchdog(&h, 0xFFFFF000u + 60000u));
}

static void t_lines(void)
{
    struct sk_ctx c;
    struct sk_stats st[SK_LEGS];
    struct sk_health h;
    char l[SK_LINE_MAX + 8];
    sk_u32 n;
    memset(&c, 0, sizeof c);
    sk_session_id(c.session, 1234, 99);
    CHECK(strlen(c.session) == 8);
    { unsigned i; for (i = 0; i < 8; i++) CHECK((c.session[i] >= '0' && c.session[i] <= '9') || (c.session[i] >= 'a' && c.session[i] <= 'z')); }
    c.up_s = 90; c.wall = 1763500000u; mkstats(st, 4);
    sk_health_init(&h, 0); h.drops = 2; h.restarts = 1;
    n = sk_fmt_sample(l, sizeof l, &c, 0, &h, 3, 1, 0, st);
    CHECK(n > 100 && n < SK_LINE_MAX && strlen(l) == n && !strncmp(l, "MSK1,S,", 7) && c.seq == 1);
    { unsigned commas = 0, i; for (i = 0; l[i]; i++) commas += l[i] == ','; CHECK(commas == 11 + 27); }
    n = sk_fmt_tls(l, sizeof l, &c, 0, 8000, 120, 0, 0, "connect timeout, (tls err 1)\nx");
    CHECK(n && !strchr(l, '\n') && c.seq == 2); { unsigned commas = 0, i; for (i = 0; l[i]; i++) commas += l[i] == ','; CHECK(commas == 11); }
    n = sk_fmt_net(l, sizeof l, &c, 0, 0, 0, &h, 5, 1, 2, 0, 0, 0); CHECK(n && !strncmp(l, "MSK1,N,", 7) && c.seq == 3); { unsigned commas = 0, i; for (i = 0; l[i]; i++) commas += l[i] == ','; CHECK(commas == 20); }
    n = sk_fmt_event(l, sizeof l, &c, "drop", 2, 0); CHECK(n && strstr(l, ",E,") && strstr(l, ",drop,2,0"));
    n = sk_fmt_upload(l, sizeof l, &c, 1, 900, 100, 400, 200, 5000, 40, 0, ""); CHECK(n && strstr(l, ",U,"));
    CHECK(sk_fmt_sample(l, 20, &c, 0, &h, 0, 0, 0, st) == 0);                   /* too small: refused, never cut */
    {
        char w[48];
        sk_clean(w, sizeof w, "a,b\n\x01z");
        CHECK(!strcmp(w, "a_b__z"));
        sk_clean(w, sizeof w, "0123456789012345678901234567890123456789ABCDEF");
        CHECK(strlen(w) == 40);
    }
}

static void t_queue(void)
{
    static struct sk_queue q;
    static char out[SK_BODY_MAX + 64];
    char l[64];
    sk_u32 n, sk, bytes, i;
    sk_q_init(&q);
    for (i = 0; i < 5; i++) { snprintf(l, sizeof l, "line%u", i); sk_q_push(&q, l); }
    bytes = sk_q_build(&q, out, sizeof out, &n, &sk);
    CHECK(n == 5 && sk == 0 && bytes == 5 * 6 && !strncmp(out, "line0\nline1\n", 12));
    sk_q_done(&q, 0, 0);
    CHECK(q.unsent == 5);
    for (i = 5; i < 8; i++) { snprintf(l, sizeof l, "line%u", i); sk_q_push(&q, l); }
    bytes = sk_q_build(&q, out, 20, &n, &sk);                                    /* room for 3 lines of 6 bytes: the NEWEST three */
    CHECK(n == 3 && sk == 5 && bytes == 18 && !strncmp(out, "line5\nline6\nline7\n", 18));
    sk_q_done(&q, 1, sk);
    CHECK(q.unsent == 0 && q.skipped == 5);
    bytes = sk_q_build(&q, out, sizeof out, &n, &sk); CHECK(n == 0 && bytes == 0 && sk == 0);
    /* overflow: the oldest unsent goes and is counted */
    sk_q_init(&q);
    for (i = 0; i < SK_QMAX + 10; i++) { snprintf(l, sizeof l, "x%u", i); sk_q_push(&q, l); }
    CHECK(q.count == SK_QMAX && q.dropped == 10 && q.unsent == SK_QMAX);
    bytes = sk_q_build(&q, out, sizeof out, &n, &sk);
    CHECK(n == SK_QMAX && !strncmp(out, "x10\n", 4));
    sk_q_done(&q, 1, 0);
    for (i = 0; i < 20; i++) sk_q_push(&q, "y");                                 /* sent lines fall off the ring without counting as dropped */
    CHECK(q.dropped == 10 && q.unsent == 20);
    CHECK(q.count == SK_QMAX);
}

static void t_files(void)
{
    char n[96];
    CHECK(sk_part_name(n, sizeof n, "abcd1234", 1) && !strcmp(n, "sd:/multishine/soak/soak-abcd1234.csv"));
    CHECK(sk_part_name(n, sizeof n, "abcd1234", 3) && !strcmp(n, "sd:/multishine/soak/soak-abcd1234.3.csv"));
    CHECK(sk_part_name(n, 10, "abcd1234", 1) == 0);
    CHECK(!sk_need_rotate(0, 300) && !sk_need_rotate(1000, 300) && !sk_need_rotate(SK_ROTATE_BYTES - 300, 300) && sk_need_rotate(SK_ROTATE_BYTES - 299, 300) && sk_need_rotate(SK_ROTATE_BYTES, 1));
    CHECK(!sk_need_rotate(0, 5000000u));                                         /* an empty file takes any line */
    CHECK(sk_parse_opts("", 0) == 0);
    CHECK(sk_parse_opts("Link\n", 5) == SK_O_LINK);
    CHECK(sk_parse_opts("noupload link", 13) == (SK_O_LINK | SK_O_NOUP));
}

/* a complete body the server would accept */
static void emit(unsigned nsamples)
{
    static char body[16384 + 256], lines[SK_BODY_MAX];
    static struct sk_queue q;
    struct sk_ctx c;
    struct sk_stats st[SK_LEGS];
    struct sk_health h;
    char l[SK_LINE_MAX + 8], pr[17];
    sk_u32 i, n, sk, at, bytes;
    const char *cid = "0123456789abcdef";
    memset(&c, 0, sizeof c);
    sk_session_id(c.session, 7, 8);
    sk_health_init(&h, 0);
    sk_q_init(&q);
    c.wall = 1763500000u;
    sk_fmt_event(l, sizeof l, &c, "start", 160, 0); sk_q_push(&q, l);
    for (i = 0; i < nsamples; i++) {
        c.up_s = (i + 1) * 10; c.wall += 10;
        mkstats(st, i);
        if (i % 6 == 5) { sk_fmt_tls(l, sizeof l, &c, 1, 640, 90, 380, 200, ""); sk_q_push(&q, l); sk_fmt_net(l, sizeof l, &c, 0, 0, 0, &h, i / 6 + 1, 0, 0, 0, 0, 0); sk_q_push(&q, l); }
        if (i == 7) { h.drops = 1; sk_fmt_event(l, sizeof l, &c, "drop", 2, 0); sk_q_push(&q, l); }
        if (i == 9) { sk_fmt_event(l, sizeof l, &c, "up", 30000, 3); sk_q_push(&q, l); }
        sk_fmt_sample(l, sizeof l, &c, 0, &h, 0, 0, 0, st); sk_q_push(&q, l);
    }
    sk_fmt_upload(l, sizeof l, &c, 0, 8000, 0, 0, 0, 0, 0, 0, "connect timeout"); sk_q_push(&q, l);
    at = sk_body_head(body, sizeof body, cid, c.session, 160, 0, 0, c.up_s);
    bytes = sk_q_build(&q, lines, SK_BODY_MAX - at, &n, &sk);
    at = sk_body_head(body, sizeof body, cid, c.session, 160, n, sk, c.up_s);
    memcpy(body + at, lines, bytes); at += bytes; body[at] = 0;
    msd_proof(pr, cid, body, at);
    at += (sk_u32)snprintf(body + at, sizeof body - at, "proof %s\n", pr);
    fwrite(body, 1, at, stdout);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "emit")) { emit(argc >= 3 ? (unsigned)atoi(argv[2]) : 30u); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "lines")) {
        struct sk_ctx c; struct sk_stats st[SK_LEGS]; struct sk_health h; char l[SK_LINE_MAX + 8];
        memset(&c, 0, sizeof c); sk_session_id(c.session, 1, 2); c.up_s = 10; c.wall = 1763500000u; mkstats(st, 1); sk_health_init(&h, 0);
        sk_fmt_sample(l, sizeof l, &c, 0, &h, 0, 0, 0, st); puts(l);
        sk_fmt_tls(l, sizeof l, &c, 1, 600, 90, 380, 200, ""); puts(l);
        sk_fmt_net(l, sizeof l, &c, 0, 0, 0, &h, 1, 0, 0, 0, 0, 0); puts(l);
        sk_fmt_event(l, sizeof l, &c, "drop", 2, 0); puts(l);
        sk_fmt_upload(l, sizeof l, &c, 1, 900, 100, 400, 200, 5000, 40, 0, ""); puts(l);
        return 0;
    }
    t_burst(); t_txid(); t_health(); t_lines(); t_queue(); t_files();
    printf("soak_test: %d failures\n", fails);
    return fails ? 1 : 0;
}
