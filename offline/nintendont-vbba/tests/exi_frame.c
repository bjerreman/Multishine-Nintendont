/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * EXI commands per match frame on a Wii: OUR driver (offline/bba) + sdnet against the virtual chip through the model of
 * Nintendont's stubs (tests/vbba_rig.c: select / imm / dma = one kernel main-loop pass each, deselect / sync free), with the traffic of one netplay
 * frame as the 1252 frame-cost readout saw it: the peer's pad packet and its pad ack arrive, our B0 hook polls and sends our pad, the network slice
 * polls and sends the ack (2 datagrams each way, 2 polls).  Run with the 1252 driver settings (rx_burst, the kernel's RWP word absent as on )
 * and with wii_fast, and compares: commands per frame (kernel trips, and trips + deselects = what the FC readout counts), the UDP payloads that went
 * out (must be identical), every received datagram's payload and the delay from its arrival in IOS to the poll that delivered it (must not grow).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vbba_rig.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define IP4(a, b, c, d) (((vbba_u32)(a) << 24) | ((vbba_u32)(b) << 16) | ((vbba_u32)(c) << 8) | (vbba_u32)(d))
#define WII_IP IP4(192, 168, 1, 50)
#define PEER IP4(192, 168, 1, 20)
#define PEER_PORT 2626
#define FRAMES 600
#define PAD_LEN 66                   /* ENet header + unsequenced command + S7 pad message with a few unacknowledged frames */
#define ACK_LEN 18                   /* ENet header + unsequenced command + the 6-byte pad ack */

static struct rig G;
static unsigned g_desel;
static void (*g_desel_orig)(void *);
static void desel_count(void *c) { g_desel++; g_desel_orig(c); }

/* what went out (IOS sendto) */
static unsigned char g_tx[4 * FRAMES + 64][PAD_LEN];
static unsigned g_txlen[4 * FRAMES + 64], g_ntx;
static void tx_hook(vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u32 len)
{
    if (ip != PEER || port != PEER_PORT || g_ntx >= sizeof g_txlen / sizeof g_txlen[0]) return;
    memcpy(g_tx[g_ntx], d, len < PAD_LEN ? len : PAD_LEN); g_txlen[g_ntx++] = len;
}

struct result {
    unsigned trips, desel, rx, rx_bad, delay_us_sum, delay_us_max, txf;
    unsigned b0_trips, slice_trips, idle_trips;
    unsigned ntx; unsigned char tx[4 * FRAMES + 64][PAD_LEN]; unsigned txlen[4 * FRAMES + 64];
};
static struct result R[3];

static void step_us(unsigned us)    /* the kernel's main loop between our hooks: time passes, the chip model fills the ring */
{
    unsigned k;
    for (k = 0; k < us; k += 100) { G.us += 100; G.io.now = (vbba_u32)(G.us / 1000ull); rig_arm(&G); }
}

static unsigned long long g_inj_us[2 * FRAMES + 16];
static unsigned g_ninj;
static void inject(unsigned seq, int len)
{
    vbba_u8 p[PAD_LEN];
    memset(p, 0x33, sizeof p);
    p[0] = (vbba_u8)seq; p[1] = (vbba_u8)(seq >> 8); p[2] = (vbba_u8)len;
    CHECK(mock_ios_inject(&G.io, 5000, PEER, PEER_PORT, p, (vbba_u32)len) == 1);
    g_inj_us[seq] = G.us;
    g_ninj++;
}

static void poll_recv(struct result *r, int sock)
{
    vbba_u8 rx[1600]; sdnet_u32 ip; sdnet_u16 port; int n;
    sdnet_poll(&G.S);
    while (sdnet_pending(&G.S, sock)) {
        n = sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &port);
        if (n >= 3 && ip == PEER && port == PEER_PORT) {
            unsigned seq = (unsigned)rx[0] | ((unsigned)rx[1] << 8), d;
            int ok = n == rx[2];
            for (d = 3; ok && d < (unsigned)n; d++) ok = rx[d] == 0x33;
            if (!ok || seq >= g_ninj) r->rx_bad++;
            else { d = (unsigned)(G.us - g_inj_us[seq]); r->delay_us_sum += d; if (d > r->delay_us_max) r->delay_us_max = d; }
            r->rx++;
        }
    }
}

static void send_pay(int sock, unsigned seq, int len)
{
    vbba_u8 p[PAD_LEN];
    memset(p, 0x77, sizeof p);
    p[0] = (vbba_u8)seq; p[1] = (vbba_u8)(seq >> 8);
    CHECK(sdnet_sendto(&G.S, sock, PEER, PEER_PORT, p, (sdnet_u16)len) == len);
}

static void run(int fast, struct result *r)
{
    struct vbba_router_cfg rc;
    static const vbba_u32 dead = 0;      /* / no RXPUB, the word never says 'RX' */
    int sock, i, f;
    unsigned t0, d0, t1;
    memset(r, 0, sizeof *r);
    memset(g_tx, 0, sizeof g_tx); memset(g_txlen, 0, sizeof g_txlen); g_ntx = 0;
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.latency_ms = 1;
    G.io.tx_hook = tx_hook;
    g_desel_orig = G.ops.deselect; G.ops.deselect = desel_count;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    G.B.rx_burst = 1; G.B.rx_hint = &dead;                           /* the 1252 Wii settings (s12_gc.c) */
    G.B.wii_fast = (sdnet_u8)(fast != 0);
    for (i = 0; i < 20000 && sdnet_dhcp_state(&G.S) != SDNET_DHCP_BOUND; i += 10) rig_run(&G, 10);
    CHECK(sdnet_dhcp_state(&G.S) == SDNET_DHCP_BOUND);
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    for (i = 0; i < 20; i++) { vbba_u8 p[8] = { 0 }; if (sdnet_sendto(&G.S, sock, PEER, PEER_PORT, p, 8) == 8) break; rig_run(&G, 10); }
    rig_run(&G, 600);                                                /* the IOS socket of port 5000 is open */
    { vbba_u8 rx[1600]; sdnet_u32 ip; sdnet_u16 p; while (sdnet_pending(&G.S, sock)) (void)sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &p); }
    G.B.pv = (sdnet_u8)(fast == 2);                                  /* the PV fast path (a kernel with NIN_MS_PVBBA) */
    G.trip_us = 150;                                                 /* about what one network EXI command cost the PowerPC in the 1252 readout (127 us average incl. the free deselects) */
    g_ntx = 0; g_ninj = 0;
    t0 = G.trips; d0 = g_desel;
    for (f = 0; f < FRAMES; f++) {                                   /* one 16.68 ms frame */
        inject(2u * (unsigned)f, PAD_LEN);                           /* the peer's pad packet */
        step_us(1500);
        t1 = G.trips; poll_recv(r, sock); send_pay(sock, 2u * (unsigned)f, PAD_LEN); r->b0_trips += G.trips - t1;     /* B0: poll, our pad */
        step_us(3000);
        inject(2u * (unsigned)f + 1u, ACK_LEN);                      /* the peer's ack of our pad */
        step_us(4500);
        t1 = G.trips; poll_recv(r, sock); send_pay(sock, 2u * (unsigned)f + 1u, ACK_LEN); r->slice_trips += G.trips - t1;   /* slice: poll, the ack */
        step_us(7680);
    }
    r->trips = G.trips - t0; r->desel = g_desel - d0;
    t1 = G.trips; poll_recv(r, sock); r->idle_trips = G.trips - t1;  /* one poll with nothing waiting */
    step_us(20000); poll_recv(r, sock);
    r->txf = G.B.st.tx_frames;
    r->ntx = g_ntx; memcpy(r->tx, g_tx, sizeof g_tx); memcpy(r->txlen, g_txlen, sizeof g_txlen);
    CHECK(G.chip.st.tx_runt == 0 && G.chip.st.tx_overflow == 0 && G.chip.st.rx_bad_ring == 0 && G.router.st.tx_bad == 0 && G.router.st.tx_bad_csum == 0);
    CHECK(G.B.st.rx_bad_desc == 0 && G.B.st.rx_resync == 0 && G.B.st.tx_timeouts == 0);
    rig_close(&G);
}

/* PV fast path with long frames and ring wrap: bursts of 4 datagrams of 2..512 bytes (1-3 pages each: the ring wraps; 700+ byte bursts lose datagrams on BOTH paths in this rig, not looked into), every payload must come back intact and in order;
   and sends of every length class go out byte for byte */
static void test_pv_sizes(void)
{
    struct vbba_router_cfg rc;
    static const int sz[] = { 2, 18, 60, 150, 181, 182, 183, 300, 400, 512, 5, 450, 250 };
    int sock, i, r, n, got = 0, bad = 0, sent_ok = 0;
    vbba_u8 p[1500], rx[1600]; sdnet_u32 ip; sdnet_u16 port;
    printf("-- PV fast path: long frames, bursts, ring wrap\n");
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.latency_ms = 1; G.io.tx_hook = 0;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    G.B.rx_burst = 1; G.B.wii_fast = 1; G.B.pv = (sdnet_u8)(getenv("NOPV") ? 0 : 1);
    for (i = 0; i < 20000 && sdnet_dhcp_state(&G.S) != SDNET_DHCP_BOUND; i += 10) rig_run(&G, 10);
    CHECK(sdnet_dhcp_state(&G.S) == SDNET_DHCP_BOUND);                /* DHCP itself ran over the PV path */
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    for (i = 0; i < 20; i++) { if (sdnet_sendto(&G.S, sock, PEER, PEER_PORT, p, 8) == 8) break; rig_run(&G, 10); }
    rig_run(&G, 600);
    while (sdnet_pending(&G.S, sock)) (void)sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &port);
    for (r = 0; r < 20; r++) {
        if (r == 7) G.pv_zero = 1;                                   /* one receive answered without the header (the 1256 field answer): retried, nothing lost */
        for (i = 0; i < 4; i++) {                                    /* 4 at once: up to 24 pages queued, the ring (14 usable) wraps and refills */
            int k = (r * 4 + i) % (int)(sizeof sz / sizeof sz[0]), j;
            for (j = 0; j < sz[k]; j++) p[j] = (vbba_u8)(j * 3 + r * 4 + i);
            p[0] = (vbba_u8)k; p[1] = (vbba_u8)(r * 4 + i);
            CHECK(mock_ios_inject(&G.io, 5000, PEER, PEER_PORT, p, (vbba_u32)sz[k]) == 1);
        }
        for (i = 0; i < 60; i++) {
            step_us(1000); sdnet_poll(&G.S);
            while (sdnet_pending(&G.S, sock)) {
                int k, j, ok;
                n = sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &port);
                k = n > 0 ? rx[0] : 0; ok = n == sz[k];
                for (j = 2; ok && j < n; j++) ok = rx[j] == (vbba_u8)(j * 3 + rx[1]);
                if (ok && rx[1] != (vbba_u8)got) printf("   order/lost: got #%u at %d (size %d)\n", rx[1], got, n);
                got++; if (!ok) { bad++; printf("   bad: n %d k %d\n", n, k); }
            }
        }
        if (sdnet_sendto(&G.S, sock, PEER, PEER_PORT, p, (sdnet_u16)sz[r % (int)(sizeof sz / sizeof sz[0])]) >= 0) sent_ok++;
    }
    printf("   received %d of 80 (bad %d), PV rx %u tx %u errors %u, chip pv short reads %u bad %u, sends accepted %d of 20, bad answer: PV left, why %u\n", got, bad, G.B.st_pv_rx, G.B.st_pv_tx, G.B.st_pv_err,
           G.chip.st.pv_rx_short, G.chip.st.pv_bad, sent_ok, G.B.pv_why);
    CHECK(got >= 78 && got <= 79 && bad == 0 && G.B.st_pv_err == 1 && G.B.pv == 0 && G.chip.st.pv_bad == 0 && G.chip.st.pv_rx_short == 0 && sent_ok == 20);   /* 1261: the bad answer of round 7 leaves PV at once; the frame it consumed is lost, the register path carries the rest */
#ifdef MM_FC
    CHECK(G.B.pv_why == 3 && G.B.pv_bad0 == 0 && G.B.pv_bad1 == 0);    /* the why-record (FC words 121 / 123 / 124) exists only in MM_FC builds of the driver */
#else
    CHECK(G.B.pv_why == 0);                                             /* release driver: no why-record, PV is still left at once (st_pv_err, pv == 0 above) */
#endif
    CHECK(G.pv_cmp_n > 20u && G.pv_cmp_bad == 0u);                      /* every delivered frame (odd lengths included) equals the ring byte for byte (word stores) */
    CHECK(G.chip.st.tx_runt == 0 && G.chip.st.tx_overflow == 0 && G.router.st.tx_bad == 0 && G.router.st.tx_bad_csum == 0);
    rig_close(&G);
}

/* The VI wait ( 5): after render the scene loop waits for the next VI and s12_idle polls the network in it (the early rollback start).  1253 on the
   Wii polled every 333 us counted from the poll START (500 us converted with the GameCube's clock), i.e. back to back; 1256 waits gap_us from the END of the previous poll.
   Datagrams arrive at times spread over the whole frame.  Checks: nothing lost, a datagram that arrives in the wait is delivered (= the early rollback can start) within
   gap + one poll, the B0 / slice deliveries are as before, and the idle polls per frame. */
struct wres { unsigned idle_polls, idle_trips, trips, rx, rx_bad, wait_n, wait_max_us, wait_sum_us, all_max_us, ntx; unsigned long long all_sum_us; };
static void run_wait(unsigned gap_us, int from_end, int pv, struct wres *w)
{
    struct vbba_router_cfg rc;
    static const vbba_u32 dead = 0;
    static struct result rr;
    struct result *r = &rr;
    int sock, i;
    unsigned f, t0, k;
    memset(w, 0, sizeof *w);
    memset(r, 0, sizeof *r);
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.latency_ms = 1; G.io.tx_hook = tx_hook;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    G.B.rx_burst = 1; G.B.rx_hint = &dead; G.B.wii_fast = 1;
    for (i = 0; i < 20000 && sdnet_dhcp_state(&G.S) != SDNET_DHCP_BOUND; i += 10) rig_run(&G, 10);
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    for (i = 0; i < 20; i++) { vbba_u8 p[8] = { 0 }; if (sdnet_sendto(&G.S, sock, PEER, PEER_PORT, p, 8) == 8) break; rig_run(&G, 10); }
    rig_run(&G, 600);
    { vbba_u8 rx[1600]; sdnet_u32 ip; sdnet_u16 p; while (sdnet_pending(&G.S, sock)) (void)sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &p); }
    G.B.pv = (sdnet_u8)pv;
    G.trip_us = 112;                                                 /* the 1253 readout: 112 us per command */
    g_ntx = 0; g_ninj = 0;
    t0 = G.trips;
    for (f = 0; f < FRAMES; f++) {
        unsigned long long fs = G.us, last = 0;
        unsigned inj[2], done[2] = { 0, 0 }, have = 0, seq0 = g_ninj;
        int b0 = 0, sl = 0;
        inj[0] = (f * 7919u) % 16000u; inj[1] = (f * 4271u + 8000u) % 16000u;    /* the peer's pad and ack: anywhere in the frame */
        while (G.us - fs < 16680u) {
            unsigned now = (unsigned)(G.us - fs), rx0, d0;
            for (k = 0; k < 2u; k++) if (!done[k] && now >= inj[k]) { inject(seq0 + have, k ? ACK_LEN : PAD_LEN); done[k] = 1; inj[k] = now; have++; }
            if (!b0 && now >= 1500u) { b0 = 1; poll_recv(r, sock); send_pay(sock, 2u * f, PAD_LEN); continue; }
            if (!sl && now >= 9000u) { sl = 1; poll_recv(r, sock); send_pay(sock, 2u * f + 1u, ACK_LEN); last = 0; continue; }
            if (sl && now >= 11000u && (!last || G.us - last >= gap_us)) {    /* s12_idle in the VI wait */
                unsigned long long ps = G.us; unsigned tr = G.trips;
                rx0 = r->rx; d0 = r->delay_us_max; (void)d0;
                { unsigned long long before = r->delay_us_sum; poll_recv(r, sock);
                  if (r->rx != rx0) { unsigned d = (unsigned)((r->delay_us_sum - before) / (r->rx - rx0)); w->wait_n += r->rx - rx0; w->wait_sum_us += d * (r->rx - rx0); if (d > w->wait_max_us) w->wait_max_us = d; } }
                w->idle_polls++; w->idle_trips += G.trips - tr;
                last = from_end ? G.us : ps;
                continue;
            }
            step_us(100);
        }
    }
    w->trips = G.trips - t0;
    step_us(20000); poll_recv(r, sock);
    w->rx = r->rx; w->rx_bad = r->rx_bad; w->all_max_us = r->delay_us_max; w->all_sum_us = r->delay_us_sum; w->ntx = g_ntx;
    rig_close(&G);
}
static void test_wait(void)
{
    static struct wres a, b, c;
    printf("-- the VI wait: s12_idle's polls (1253: every 333 us from the poll start; 1256: 1500 us after the poll end), datagrams arriving anywhere in the frame, 112 us per command\n");
    run_wait(333, 0, 0, &a);
    run_wait(1500, 1, 0, &b);
    run_wait(1500, 1, 1, &c);
    {   struct wres *v[3] = { &a, &b, &c }; static const char *nm[3] = { "1253 (333 us from start)", "1256 (1500 us from end)", "1256 + PV" }; int k;
        for (k = 0; k < 3; k++)
            printf("   %-26s %.1f commands per frame, of them %.1f in %.1f idle polls; received %u of %u (bad %u); arriving in the wait: delivered after mean %.2f ms max %.2f ms; all: mean %.2f max %.2f ms\n",
                   nm[k], (double)v[k]->trips / FRAMES, (double)v[k]->idle_trips / FRAMES, (double)v[k]->idle_polls / FRAMES, v[k]->rx, 2u * FRAMES, v[k]->rx_bad,
                   v[k]->wait_n ? v[k]->wait_sum_us / 1000.0 / v[k]->wait_n : 0.0, v[k]->wait_max_us / 1000.0, v[k]->rx ? v[k]->all_sum_us / 1000.0 / v[k]->rx : 0.0, v[k]->all_max_us / 1000.0);
        for (k = 0; k < 3; k++) CHECK(v[k]->rx == 2u * FRAMES && v[k]->rx_bad == 0 && v[k]->ntx == 2u * FRAMES);   /* nothing lost */
        CHECK(b.wait_max_us <= 1500u + 3u * 112u + 1000u + 200u && c.wait_max_us <= 1500u + 2u * 112u + 1000u + 200u);   /* in the wait: within gap + one poll + the mock's 1 ms IOS completion + 2 rig steps */
        CHECK(b.all_max_us <= a.all_max_us + 100u && c.all_max_us <= a.all_max_us);                      /* the worst delivery of the frame (B0 / slice) is not later */
        CHECK(b.idle_polls * 3u < a.idle_polls && b.idle_trips * 3u < a.idle_trips);
    }
}

int main(void)
{
    int k;
    printf("-- EXI commands per netplay frame (2 datagrams in, 2 out, 2 polls per frame; %d frames)\n", FRAMES);
    run(0, &R[0]);
    run(1, &R[1]);
    run(2, &R[2]);
    test_pv_sizes();
    test_wait();
    for (k = 0; k < 3; k++) {
        struct result *r = &R[k];
        printf("   %s: %.1f kernel trips per frame (B0 %.1f, slice %.1f), %.1f with the free deselects (what the FC readout counts); idle poll %u trips;"
               " received %u (bad %u), delivery delay mean %.2f ms max %.2f ms; sent %u\n",
               k == 2 ? "PV kernel " : k ? "wii_fast  " : "1252 (old)", (double)r->trips / FRAMES, (double)r->b0_trips / FRAMES, (double)r->slice_trips / FRAMES,
               (double)(r->trips + r->desel) / FRAMES, r->idle_trips, r->rx, r->rx_bad, r->rx ? r->delay_us_sum / 1000.0 / r->rx : 0.0, r->delay_us_max / 1000.0, r->ntx);
        CHECK(r->rx == 2u * FRAMES && r->rx_bad == 0 && r->ntx == 2u * FRAMES);
    }
    CHECK(R[1].ntx == R[0].ntx && memcmp(R[1].txlen, R[0].txlen, R[0].ntx * sizeof R[0].txlen[0]) == 0 && memcmp(R[1].tx, R[0].tx, R[0].ntx * sizeof R[0].tx[0]) == 0);   /* same wire */
    CHECK(R[1].delay_us_max <= R[0].delay_us_max && R[1].delay_us_sum <= R[0].delay_us_sum);                                                  /* nothing arrives later */
    CHECK(R[1].trips * 2u < R[0].trips);
    CHECK(R[2].ntx == R[0].ntx && memcmp(R[2].txlen, R[0].txlen, R[0].ntx * sizeof R[0].txlen[0]) == 0 && memcmp(R[2].tx, R[0].tx, R[0].ntx * sizeof R[0].tx[0]) == 0);
    CHECK(R[2].delay_us_max <= R[1].delay_us_max && R[2].delay_us_sum <= R[1].delay_us_sum && R[2].trips * 2u < R[1].trips);
    printf("   PV fast path: same UDP payloads out: %s; trips per frame %.1f, %.1f with the deselects\n", memcmp(R[2].tx, R[0].tx, R[0].ntx * sizeof R[0].tx[0]) ? "NO" : "yes",
           (double)R[2].trips / FRAMES, (double)(R[2].trips + R[2].desel) / FRAMES);
    printf("   same UDP payloads out: %s; trips per frame %.1f -> %.1f (%.0f %% fewer)\n", memcmp(R[1].tx, R[0].tx, R[0].ntx * sizeof R[0].tx[0]) ? "NO" : "yes",
           (double)R[0].trips / FRAMES, (double)R[1].trips / FRAMES, 100.0 - 100.0 * R[1].trips / R[0].trips);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
