/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * Host tests: OUR unchanged BBA driver (offline/bba) and sdnet (offline/net) against the virtual BBA through a model of
 * Nintendont's EXI stubs, the vbba router and the IOS transport over an in-memory IOS (tests/mock_ios.c).
 * Deterministic (simulated clock).  Every section prints what it proved; exit 0 = all checks passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vbba_rig.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define IP4(a, b, c, d) (((vbba_u32)(a) << 24) | ((vbba_u32)(b) << 16) | ((vbba_u32)(c) << 8) | (vbba_u32)(d))
#define WII_IP IP4(192, 168, 1, 50)
#define ECHO_LAN IP4(192, 168, 1, 20)
#define ECHO_FAR IP4(192, 168, 7, 20)

static struct rig G;

static void p16(vbba_u8 *p, unsigned v) { p[0] = (vbba_u8)(v >> 8); p[1] = (vbba_u8)v; }
static void p32(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }
static unsigned g16(const vbba_u8 *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* a complete IPv4 frame from the guest to the router (checksums right unless spoiled later) */
static int mk_ip(vbba_u8 *f, vbba_u32 sip, vbba_u32 dip, int proto, const vbba_u8 *l4, int l4len)
{
    vbba_u8 *ip = f + 14;
    memcpy(f, RIG_ROUTER_MAC, 6); memcpy(f + 6, RIG_MAC, 6); p16(f + 12, 0x0800);
    ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 20 + l4len); p16(ip + 4, 7); p16(ip + 6, 0); ip[8] = 64; ip[9] = (vbba_u8)proto;
    p16(ip + 10, 0); p32(ip + 12, sip); p32(ip + 16, dip); p16(ip + 10, vbba_csum(ip, 20, 0));
    memcpy(f + 34, l4, (size_t)l4len);
    return 34 + l4len < 60 ? 60 : 34 + l4len;
}
static int mk_udp(vbba_u8 *f, vbba_u32 sip, vbba_u32 dip, unsigned sp, unsigned dp, const vbba_u8 *pay, int n)
{
    vbba_u8 u[1600], ph[12];
    unsigned s;
    p16(u, sp); p16(u + 2, dp); p16(u + 4, 8 + n); p16(u + 6, 0); memcpy(u + 8, pay, (size_t)n);
    p32(ph, sip); p32(ph + 4, dip); ph[8] = 0; ph[9] = 17; p16(ph + 10, 8 + n);
    {
        vbba_u32 sum = 0; int i;
        for (i = 0; i < 12; i += 2) sum += ((vbba_u32)ph[i] << 8) | ph[i + 1];
        s = vbba_csum(u, 8 + n, sum);
    }
    p16(u + 6, s ? s : 0xffff);
    return mk_ip(f, sip, dip, 17, u, 8 + n);
}

/* ---- bring-up + DHCP + echo ---------------------------------------------------------------------------------- */
static int dhcp_until(int max_ms)
{
    int t;
    for (t = 0; t < max_ms; t += 10) { rig_run(&G, 10); if (sdnet_dhcp_state(&G.S) == SDNET_DHCP_BOUND) return t + 10; }
    return -1;
}

static int echo_sizes(vbba_u32 dst, int *bad_sizes)
{
    static const int sizes[] = { 1, 18, 31, 32, 33, 64, 100, 255, 256, 257, 400, 500 };
    vbba_u8 pay[600], rx[700];
    sdnet_u32 sip; sdnet_u16 sp;
    int i, ok = 0, sock, n, k;
    for (i = 0; i < 600; i++) pay[i] = (vbba_u8)(i * 7 + 1);
    sock = sdnet_bind(&G.S, 7777, 0, 0);
    CHECK(sock >= 0);
    n = sdnet_sendto(&G.S, sock, dst, 7, pay, 20);
    CHECK(n == SDNET_E_ARP);
    rig_run(&G, 5);
    CHECK(sdnet_arp_lookup(&G.S, ((dst ^ G.S.ip) & G.S.mask) ? G.S.gw : dst, rx) == 1 && memcmp(rx, RIG_ROUTER_MAC, 6) == 0);   /* gateway / proxy ARP */
    *bad_sizes = 0;
    for (i = 0; i < 12; i++) {
        int got = 0;
        n = sdnet_sendto(&G.S, sock, dst, 7, pay, (sdnet_u16)sizes[i]);
        CHECK(n == sizes[i]);
        for (k = 0; k < 10 && !got; k++) {
            rig_run(&G, 1);
            while (sdnet_pending(&G.S, sock)) {
                n = sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &sip, &sp);
                if (n == sizes[i] && !memcmp(rx, pay, (size_t)n) && sip == dst && sp == 7) got = 1;
            }
        }
        if (got) ok++; else *bad_sizes |= 1 << i;
    }
    sdnet_close(&G.S, sock);
    return ok;
}

static void scenario(const char *name, int dma, int mirror, int stub, int split3, int inter, int expect_ok)
{
    struct vbba_router_cfg rc;
    int t, ok, bad;
    vbba_u32 dst = mirror ? ECHO_LAN : ECHO_FAR;
    printf("-- %s\n", name);
    rig_router_cfg(&rc, (mirror ? VBBA_RF_MIRROR : 0u) | VBBA_RF_ICMP);
    G.use_dma = dma; G.stub = stub; G.split3 = split3; G.interleave = inter;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.latency_ms = 1;
    G.io.echo_ip = dst;
    CHECK(rig_bringup(&G, 1) == BBA_OK);                                      /* strict: the chip must authenticate */
    CHECK(G.B.auth == BBA_AUTH_OK && G.chip.st.challenges == 1 && G.chip.st.responses == 1);
    CHECK(memcmp(G.B.mac, RIG_MAC, 6) == 0);
    CHECK(bba_link_wait(&G.B, 100) == BBA_OK);
    t = dhcp_until(20000);
    CHECK(t > 0);
    CHECK(G.S.ip == (mirror ? WII_IP : 0x0AD30002u));
    CHECK(G.S.gw == (mirror ? IP4(192, 168, 1, 1) : 0x0AD30001u));
    ok = echo_sizes(dst, &bad);
    printf("   dhcp bound after %d ms (ip %u.%u.%u.%u), echoes %d/12 (failed size mask %03x), trips %u (select %u imm %u dma %u), "
           "3-byte reads %u dropped by the stub %u, sdnet rx_bad %u\n",
           t, G.S.ip >> 24, (G.S.ip >> 16) & 255, (G.S.ip >> 8) & 255, G.S.ip & 255, ok, bad, G.trips, G.trip_select, G.trip_imm,
           G.trip_dma, G.nx.imm_r3, G.garbage, G.S.stats.rx_bad);
    CHECK(ok == expect_ok);
    if (expect_ok == 12) CHECK(G.garbage == 0 && G.B.st.rx_bad_desc == 0 && G.B.st.rx_resync == 0);
    else CHECK(G.garbage > 0 && G.S.stats.rx_bad > 0);
    CHECK(G.chip.st.tx_runt == 0 && G.chip.st.tx_overflow == 0 && G.chip.st.rx_bad_ring == 0);
    CHECK(G.router.st.tx_bad == 0 && G.router.st.tx_bad_csum == 0);
    rig_close(&G);
}

/* ---- EXI trip cost per operation (the number the Wii's ARM loop latency multiplies) ---------------------------- */
static void trip_costs(int dma)
{
    struct vbba_router_cfg rc;
    static const int sizes[] = { 60, 100, 300, 600, 1000, 1514 };
    vbba_u8 f[1600], out[1600];
    unsigned i, t0;
    rig_router_cfg(&rc, 0);
    G.use_dma = dma; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 0;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    t0 = G.trips; (void)bba_recv(&G.B, out, sizeof out);
    printf("   %s: idle bba_recv %u trips;", dma ? "dma" : "imm", G.trips - t0);
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        unsigned ts, tr;
        memset(f, 0x5a, sizeof f); memcpy(f, RIG_MAC, 6); memcpy(f + 6, RIG_ROUTER_MAC, 6); p16(f + 12, 0x88b5);
        t0 = G.trips; CHECK(bba_send(&G.B, f, (sdnet_u16)sizes[i]) == 0); ts = G.trips - t0;
        G.chip.regs[0x01] |= 0x01;                                     /* promiscuous: hand our own frame back */
        G.chip.held_len = (vbba_u16)sizes[i]; memcpy(G.chip.held, f, (size_t)sizes[i]);
        (void)vbba_chip_poll(&G.chip, 1);
        t0 = G.trips; CHECK(bba_recv(&G.B, out, sizeof out) == sizes[i]); tr = G.trips - t0;
        printf(" %d B: send %u recv %u;", sizes[i], ts, tr);
    }
    printf("\n");
    rig_close(&G);
}

/* ---- ring wrap through the router: bursts of 542-byte replies (3 pages each, 4 fit in the ring) ------------------ */
static void test_router_wrap(void)
{
    struct vbba_router_cfg rc;
    vbba_u8 pay[500], rx[600];
    sdnet_u32 sip; sdnet_u16 sp;
    int sock, i, k, got = 0, bad = 0, n;
    printf("-- ring wrap through the router (bursts, 3 copies of every echo)\n");
    rig_router_cfg(&rc, 0);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_FAR; G.io.echo_copies = 2; G.io.latency_ms = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    CHECK(dhcp_until(10000) > 0);
    sock = sdnet_bind(&G.S, 7000, 0, 0);
    for (i = 0, n = 0; i < 10 && !n; i++) {                       /* warm up: ARP, socket open on the Wii side */
        (void)sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 4);
        rig_run(&G, 5);
        while (sdnet_pending(&G.S, sock)) { (void)sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &sip, &sp); n++; }
    }
    for (k = 0; k < 40; k++) {
        for (i = 0; i < 8; i++) { memset(pay, k * 8 + i, sizeof pay); CHECK(sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 500) == 500); }
        for (i = 0; i < 40; i++) {
            rig_run(&G, 1);
            while (sdnet_pending(&G.S, sock)) {
                n = sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &sip, &sp);
                if (n != 500 || rx[0] != rx[499]) bad++;
                got++;
            }
        }
    }
    printf("   echoes received %d of %d, bad %d, chip rx %u frames, ring-full waits %u, sdnet sock_drop %u, bba rx_bad_desc %u\n",
           got, 40 * 8 * 3, bad, G.chip.st.rx_frames, G.chip.st.rx_full_waits, G.S.stats.sock_drop, G.B.st.rx_bad_desc);
    printf("   router tx_udp %u busy %u err %u opening %u | ios tx %u busy %u rx %u | mock sent %u echoed %u refused %u | router rx %u unsol %u\n",
           G.router.st.tx_udp, G.router.st.tx_tp_busy, G.router.st.tx_tp_err, G.router.st.tx_opening, G.tpi.st.tx, G.tpi.st.tx_busy, G.tpi.st.rx,
           G.io.sent, G.io.echoed, G.io.refused, G.router.st.rx_dgrams, G.router.st.rx_unsolicited);
    CHECK(got == 40 * 8 * 3 && bad == 0 && G.chip.st.rx_frames >= 960 && G.B.st.rx_bad_desc == 0);   /* 3 pages each: ~190 laps of the ring */
    rig_close(&G);
}

/* ---- tiny datagrams: 1, 2 and 17 bytes (frames of 43, 44 and 59 bytes, padded to 60), 4 echoed copies, a lost first try ---- */
static void test_tiny_retry(void)
{
    static const int sizes[] = { 1, 2, 17 };
    struct vbba_router_cfg rc;
    vbba_u8 pay[32], rx[64];
    sdnet_u32 sip; sdnet_u16 sp;
    int sock, i, j, k, n, copies, bad = 0, short_rx = 0;
    printf("-- tiny datagrams (1, 2, 17 bytes), 4 echoed copies, first send lost and retried\n");
    rig_router_cfg(&rc, 0);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_FAR; G.io.echo_copies = 3; G.io.latency_ms = 2;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    CHECK(dhcp_until(10000) > 0);
    sock = sdnet_bind(&G.S, 7000, 0, 0);
    for (i = 0; i < 20; i++) { (void)sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 4); rig_run(&G, 5); while (sdnet_pending(&G.S, sock)) (void)sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &sip, &sp); }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 2; j++) {                                  /* j = 1: the first send is lost, the retry carries another byte 1 */
            for (k = 0; k < 32; k++) pay[k] = (vbba_u8)(0x40 + 31 * k + j * 17 + i);
            if (j) G.io.drop_first = 1;
            copies = 0;
            for (k = 0; k < (j ? 2 : 1); k++) {
                CHECK(sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, (sdnet_u16)sizes[i]) == sizes[i]);
                rig_run(&G, 30);
                while (sdnet_pending(&G.S, sock)) {
                    memset(rx, 0xEE, sizeof rx);
                    n = sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &sip, &sp);
                    if (n != sizes[i] || memcmp(rx, pay, (size_t)n) || sip != ECHO_FAR || sp != 7 || rx[n] != 0xEE) bad++;
                    if (n != sizes[i]) short_rx++;
                    copies++;
                }
            }
            CHECK(copies == 4);
        }
    }
    printf("   sizes 1/2/17: bad %d (wrong length %d), sdnet rx_bad %u, router tx_bad %u\n", bad, short_rx, G.S.stats.rx_bad, G.router.st.tx_bad);
    CHECK(bad == 0 && G.S.stats.rx_bad == 0);
    rig_close(&G);
}

/* ---- chip-level ring wrap with a scripted frame backend --------------------------------------------------------- */
struct fb { int n, next, link; vbba_u32 seen_tx; };
static struct fb FB;
static int fb_caps(void *c, struct vbba_frame_caps *o) { (void)c; memset(o, 0, sizeof *o); o->link_up = (vbba_u8)FB.link; return VBBA_OK; }
static int fb_tx(void *c, const vbba_u8 *f, vbba_u16 n) { (void)c; (void)f; (void)n; FB.seen_tx++; return VBBA_OK; }
static int fb_rx(void *c, vbba_u8 *f, vbba_u16 cap, vbba_u16 *len)
{
    int i, sz;
    (void)c;
    if (FB.next >= FB.n) return VBBA_EMPTY;
    sz = 800 + FB.next % 200 + (FB.next % 7);
    if (sz > cap) return VBBA_ERROR;
    memcpy(f, RIG_MAC, 6); memcpy(f + 6, RIG_ROUTER_MAC, 6); p16(f + 12, 0x88b5);
    f[14] = (vbba_u8)FB.next; f[15] = (vbba_u8)(FB.next >> 8);
    for (i = 16; i < sz; i++) f[i] = (vbba_u8)(i ^ FB.next);
    *len = (vbba_u16)sz;
    FB.next++;
    return VBBA_OK;
}
static int fb_poll(void *c) { (void)c; return VBBA_OK; }
static void fb_reset(void *c) { (void)c; }
static const struct vbba_frame_backend FBE = { 0, fb_caps, fb_tx, fb_rx, fb_poll, fb_reset };

static void test_chip_wrap(int dma)
{
    struct vbba_router_cfg rc;
    vbba_u8 out[1600];
    int got = 0, bad = 0, n, i, guard = 0;
    printf("-- chip ring wrap, 300 frames of 800-1005 B, %s\n", dma ? "dma" : "imm");
    rig_router_cfg(&rc, 0);
    G.use_dma = dma; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    memset(&FB, 0, sizeof FB); FB.link = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, &FBE);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    FB.n = 300;                                               /* frames before the receiver is on would be dropped */
    while (got < 300 && guard++ < 5000) {
        rig_arm(&G);
        while ((n = bba_recv(&G.B, out, 1536)) > 0) {
            int seq = out[14] | (out[15] << 8), sz = 800 + seq % 200 + (seq % 7);
            if (seq != got || n != sz) bad++;
            for (i = 16; i < n; i++) if (out[i] != (vbba_u8)(i ^ seq)) { bad++; break; }
            got++;
        }
    }
    printf("   frames %d bad %d, ring-full waits %u, driver rx %u\n", got, bad, G.chip.st.rx_full_waits, G.B.st.rx_frames);
    CHECK(got == 300 && bad == 0 && G.chip.st.rx_full_waits > 0 && G.garbage == 0);
    rig_close(&G);
}

/* ---- error paths --------------------------------------------------------------------------------------------- */
static void raw_fifo(struct rig *g, int n)
{
    vbba_u8 b[2100];
    memset(b, 0x11, sizeof b);
    vbba_chip_select(&g->chip);
    { vbba_u8 h[4] = { 0xC0, 0x00, 0x48, 0x00 }; vbba_chip_xfer(&g->chip, h, 4, 1); }
    vbba_chip_xfer(&g->chip, b, (vbba_u32)n, 1);
    vbba_chip_deselect(&g->chip);
    bba_reg_out8(&g->B, BBA_NCRA, BBA_NCRA_ST1 | BBA_NCRA_SR);
}

static void test_errors(void)
{
    struct vbba_router_cfg rc;
    vbba_u8 out[1600];
    printf("-- error paths\n");
    rig_router_cfg(&rc, 0);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 0;
    /* link down from the backend */
    memset(&FB, 0, sizeof FB); FB.link = 0;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, &FBE);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    CHECK(bba_link_wait(&G.B, 20) == BBA_E_LINK);
    /* chip without challenge (like Dolphin / Swiss): strict driver refuses, tolerant one runs */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, 0, 0);
    CHECK(rig_bringup(&G, 1) == BBA_E_AUTH);
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, 0, 0);
    CHECK(rig_bringup(&G, 0) == BBA_OK && G.B.auth == BBA_AUTH_NONE);
    /* re-init (NCRA reset) authenticates again */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    CHECK(rig_bringup(&G, 1) == BBA_OK && G.chip.st.challenges == 2);
    /* no adapter answering: the select reaches another device (stock Nintendont has no SP1 Ethernet model) */
    G.absent = 1;
    { struct bba b; memset(&b, 0, sizeof b); CHECK(bba_init(&b, &G.ops, &G, 0) == BBA_E_NODEV); }
    G.absent = 0;
    CHECK(G.nx.other_dev >= 1);
    /* TX FIFO overflow and runt frames from a hostile guest: dropped, counted, nothing forwarded */
    {
        unsigned tx0 = G.router.st.tx_frames;
        raw_fifo(&G, 2000);
        CHECK(G.chip.st.tx_overflow == 1);
        raw_fifo(&G, 20);
        CHECK(G.chip.st.tx_runt == 1);
        CHECK(G.router.st.tx_frames == tx0 && (G.chip.regs[0] & BBA_NCRA_ST1) == 0);
    }
    /* receiver stopped: frames are dropped, not queued */
    memset(&FB, 0, sizeof FB); FB.n = 3; FB.link = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, &FBE);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    bba_reg_out8(&G.B, BBA_NCRA, 0);
    rig_arm(&G);
    CHECK(G.chip.st.rx_off_drop == 3 && bba_recv(&G.B, out, 1536) == 0);
    /* guest corrupts RRP (outside BP..RHBP): the chip stops delivering, nothing breaks; re-init recovers */
    memset(&FB, 0, sizeof FB); FB.n = 4; FB.link = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, &FBE);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    bba_reg_out8(&G.B, BBA_RRP, 0x40);
    rig_arm(&G);
    CHECK(G.chip.st.rx_bad_ring > 0 && G.chip.st.rx_frames == 0);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    rig_arm(&G);
    CHECK(bba_recv(&G.B, out, 1536) > 0);
    printf("   link down, auth off strict/tolerant, re-auth, other device, FIFO overflow, runt, receiver off, bad RRP: done\n");
    rig_close(&G);
}

/* ---- router policy -------------------------------------------------------------------------------------------- */
static int bound_private(vbba_u32 flags)
{
    struct vbba_router_cfg rc;
    rig_router_cfg(&rc, flags | VBBA_RF_ICMP);
    rc.ct_ms = 5000;
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_FAR; G.io.latency_ms = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    return dhcp_until(10000) > 0;
}

static int recv_all(int sock, vbba_u32 *from)
{
    vbba_u8 rx[1600]; sdnet_u32 ip; sdnet_u16 p; int n = 0;
    while (sdnet_pending(&G.S, sock)) { (void)sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &p); if (from) *from = ip; n++; }
    return n;
}

static void test_policy(void)
{
    vbba_u8 pay[64], f[1600], out[1600];
    int sock, n, i;
    unsigned sent0;
    printf("-- router policy\n");
    memset(pay, 0x42, sizeof pay);
    CHECK(bound_private(0));
    sock = sdnet_bind(&G.S, 7777, 0, 0);
    /* allowed echo opens the map and a conntrack entry */
    (void)sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 10); rig_run(&G, 3);    /* E_ARP: sdnet holds this datagram and sends it once ARP resolves */
    CHECK(sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 10) == 10); rig_run(&G, 5);
    CHECK(recv_all(sock, 0) == 2);                                           /* the held first datagram and the second one */
    /* destination outside the allow-list: dropped in the router, IOS never sees it */
    sent0 = G.io.sent;
    CHECK(sdnet_sendto(&G.S, sock, IP4(8, 8, 8, 8), 53, pay, 10) == 10); rig_run(&G, 5);
    CHECK(G.router.st.tx_denied >= 1 && G.io.sent == sent0);
    /* unsolicited datagram from an unknown LAN host: not delivered */
    CHECK(mock_ios_inject(&G.io, 7777, IP4(192, 168, 1, 99), 5000, pay, 20));
    rig_run(&G, 5);
    CHECK(recv_all(sock, 0) == 0 && G.router.st.rx_unsolicited == 1);
    /* from the conntrack peer: delivered */
    CHECK(mock_ios_inject(&G.io, 7777, ECHO_FAR, 7, pay, 20)); rig_run(&G, 5);
    CHECK(recv_all(sock, 0) == 1);
    /* conntrack expiry (ct_ms 5000) */
    rig_run(&G, 6000);
    CHECK(mock_ios_inject(&G.io, 7777, ECHO_FAR, 7, pay, 20)); rig_run(&G, 5);
    CHECK(recv_all(sock, 0) == 0 && G.router.st.rx_unsolicited == 2);
    /* oversize inbound datagram (2000 B): IOS reports truncation, never delivered */
    { static vbba_u8 big[2000]; CHECK(sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 4) == 4); rig_run(&G, 5); (void)recv_all(sock, 0);
      CHECK(mock_ios_inject(&G.io, 7777, ECHO_FAR, 7, big, 2000)); rig_run(&G, 5);
      n = recv_all(sock, 0);
      CHECK(n == 0 && G.tpi.st.rx_trunc == 1 && G.router.st.rx_trunc == 1);
      if (n || G.router.st.rx_trunc != 1) printf("   trunc: got %d tp %u router %u\n", n, G.tpi.st.rx_trunc, G.router.st.rx_trunc); }
    /* broadcast UDP, spoofed source, TCP, bad checksums, fragments: all dropped and counted */
    CHECK(sdnet_sendto(&G.S, sock, 0xffffffffu, 9999, pay, 8) == 8); rig_run(&G, 2);
    CHECK(G.router.st.tx_bcast == 1);
    n = mk_udp(f, IP4(10, 211, 0, 99), ECHO_FAR, 7777, 7, pay, 8); CHECK(bba_send(&G.B, f, (sdnet_u16)n) == 0);
    CHECK(G.router.st.tx_spoof == 1);
    n = mk_ip(f, G.S.ip, ECHO_FAR, 6, pay, 20); CHECK(bba_send(&G.B, f, (sdnet_u16)n) == 0);
    CHECK(G.router.st.tx_tcp == 1);
    n = mk_udp(f, G.S.ip, ECHO_FAR, 7777, 7, pay, 8); f[42] ^= 0xff; CHECK(bba_send(&G.B, f, (sdnet_u16)n) == 0);
    n = mk_udp(f, G.S.ip, ECHO_FAR, 7777, 7, pay, 8); f[24] ^= 0x10; CHECK(bba_send(&G.B, f, (sdnet_u16)n) == 0);
    CHECK(G.router.st.tx_bad_csum == 2);
    n = mk_udp(f, G.S.ip, ECHO_FAR, 7777, 7, pay, 8); f[20] = 0x20; f[24] = 0; f[25] = 0; p16(f + 24, vbba_csum(f + 14, 20, 0));
    CHECK(bba_send(&G.B, f, (sdnet_u16)n) == 0 && G.router.st.tx_frag == 1);
    /* ping to the router */
    {
        vbba_u8 ic[40];
        memset(ic, 0, sizeof ic); ic[0] = 8; p16(ic + 4, 0x1234); p16(ic + 6, 1); memset(ic + 8, 0x61, 32);
        p16(ic + 2, vbba_csum(ic, 40, 0));
        n = mk_ip(f, G.S.ip, 0x0AD30001u, 1, ic, 40);
        for (i = 0; i < 20; i++) (void)bba_recv(&G.B, out, 1536);
        CHECK(bba_send(&G.B, f, (sdnet_u16)n) == 0);
        rig_arm(&G);
        n = bba_recv(&G.B, out, 1536);
        CHECK(n >= 74 && out[23] == 1 && out[34] == 0 && vbba_csum(out + 34, 40, 0) == 0 && g16(out + 38) == 0x1234);
        CHECK(G.router.st.icmp_replies == 1);
    }
    /* ARP storm: 1000 requests for random LAN addresses within ~100 ms: answers capped at arp_per_s (50) per second */
    {
        unsigned rep0 = G.router.st.arp_replies;
        for (i = 0; i < 1000; i++) {
            memset(f, 0, 60); memset(f, 0xff, 6); memcpy(f + 6, RIG_MAC, 6); p16(f + 12, 0x0806);
            p16(f + 14, 1); p16(f + 16, 0x0800); f[18] = 6; f[19] = 4; p16(f + 20, 1);
            memcpy(f + 22, RIG_MAC, 6); p32(f + 28, G.S.ip); p32(f + 38, IP4(192, 168, 3, i & 255));
            CHECK(bba_send(&G.B, f, 60) == 0);
            if ((i & 7) == 0) { rig_arm(&G); while (bba_recv(&G.B, out, 1536) > 0) { } }
            if ((i % 10) == 0) { G.us += 1000; G.io.now = (vbba_u32)(G.us / 1000); }
        }
        printf("   ARP storm: 1000 requests -> %u replies, %u rate-limited, %u local-queue drops\n",
               G.router.st.arp_replies - rep0, G.router.st.local_rate, G.router.st.localq_drop);
        CHECK(G.router.st.arp_replies - rep0 <= 2 * 50 + 4 && G.router.st.local_rate > 0);
    }
    rig_close(&G);

    /* OPEN_INBOUND: an allow-listed peer may speak first */
    CHECK(bound_private(VBBA_RF_OPEN_INBOUND));
    sock = sdnet_bind(&G.S, 7777, 0, 0);
    (void)sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 10); rig_run(&G, 3);
    (void)sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 10); rig_run(&G, 5); (void)recv_all(sock, 0);
    CHECK(mock_ios_inject(&G.io, 7777, IP4(192, 168, 1, 99), 5000, pay, 20)); rig_run(&G, 5);
    CHECK(recv_all(sock, 0) == 1);
    CHECK(mock_ios_inject(&G.io, 7777, IP4(10, 9, 9, 9), 5000, pay, 20)); rig_run(&G, 5);       /* not allow-listed */
    CHECK(recv_all(sock, 0) == 0);
    rig_close(&G);
    printf("   allow-list, conntrack (+expiry), OPEN_INBOUND, truncation, broadcast, spoof, TCP, checksums, fragments, ping: done\n");
}

/* ---- DHCP option 6 (resolver) and a DNS query to it ----------------------------------------------------- */
static void test_dns(void)
{
    struct vbba_router_cfg rc;
    vbba_u8 pay[32];
    int sock, k;
    unsigned sent0, denied0;
    const vbba_u32 DNS1 = IP4(1, 1, 1, 1);
    printf("-- DHCP option 6 (resolver)\n");
    memset(pay, 0x42, sizeof pay);
    for (k = 0; k < 2; k++) {                                   /* k 0: cfg.dns = 0 (no option 6, the guest has no resolver); k 1: cfg.dns = 1.1.1.1 */
        rig_router_cfg(&rc, VBBA_RF_ICMP);
        rc.allow[0].ip = 0; rc.allow[0].mask = 0; rc.allow[0].port_lo = 1; rc.allow[0].port_hi = 65535;   /* the Wii glue's rule: every destination, every port */
        rc.nallow = 1;
        rc.dns = k ? DNS1 : 0;
        G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
        rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
        G.io.echo_ip = ECHO_FAR; G.io.latency_ms = 1;
        CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(10000) > 0);
        CHECK(G.router.st.dhcp_ack >= 1);
        printf("   cfg.dns %08X -> guest resolver %u.%u.%u.%u\n", rc.dns, G.S.dns >> 24, (G.S.dns >> 16) & 255u, (G.S.dns >> 8) & 255u, G.S.dns & 255u);
        CHECK(G.S.dns == rc.dns);                               /* option 6 reached sdnet's lease, with the expected address (0 when none is configured) */
        sock = sdnet_bind(&G.S, 5353, 0, 0);
        (void)sdnet_sendto(&G.S, sock, ECHO_FAR, 7, pay, 4); rig_run(&G, 5);              /* the usual first-datagram warm-up */
        sent0 = G.io.sent; denied0 = G.router.st.tx_denied;
        CHECK(sdnet_sendto(&G.S, sock, DNS1, 53, pay, 29) == 29); rig_run(&G, 5);
        CHECK(G.router.st.tx_dns == 1);                         /* counted */
        CHECK(G.io.sent == sent0 + 1 && G.router.st.tx_denied == denied0);   /* forwarded to IOS, not denied */
        {   /* diagnostics: frame types and the first distinct headers */
            const struct vbba_router_stats *q = &G.router.st; unsigned i, hit = 0;
            CHECK(q->tx_f_udp >= 1 && q->tx_f_arp + q->tx_f_udp + q->tx_f_ip_other + q->tx_f_nonip == q->tx_frames);
            CHECK(G.router.txhdr_n >= 1 && G.router.txhdr_n <= VBBA_TXHDR);
            for (i = 0; i < G.router.txhdr_n; i++) if (G.router.txhdr[i].eth == 0x0800 && G.router.txhdr[i].proto == 17 && G.router.txhdr[i].dport == 53 && G.router.txhdr[i].dip == DNS1) hit++;
            CHECK(hit == 1);                                    /* the DNS query is recorded exactly once */
            (void)sdnet_sendto(&G.S, sock, DNS1, 53, pay, 29); rig_run(&G, 5);
            hit = 0; for (i = 0; i < G.router.txhdr_n; i++) if (G.router.txhdr[i].dport == 53) hit++;
            CHECK(hit == 1);                                    /* a repeat does not add another entry */
        }
        rig_close(&G);
    }
    printf("   option 6 absent without cfg.dns, present with it; DNS query to it forwarded and not counted as denied: done\n");
}

/* ---- IOS behaviour: DHCP waits for the Wii's address, startup failure, socket failure, port taken ------------------ */
static void test_ios(void)
{
    struct vbba_router_cfg rc;
    vbba_u8 pay[32];
    int sock, i, got;
    printf("-- IOS transport behaviour\n");
    memset(pay, 7, sizeof pay);
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 0;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.dhcp_ms = 3000; G.io.latency_ms = 2; G.io.echo_ip = ECHO_LAN;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    rig_run(&G, 2500);
    CHECK(sdnet_dhcp_state(&G.S) != SDNET_DHCP_BOUND && G.router.st.dhcp_wait_ip >= 1);
    { int ms = dhcp_until(20000); printf("   address from IOS at 3.0 s: lease %d ms after 2.5 s (deferred offers %u; the guest's own retries come at 2, 6, 14, 30 s)\n", ms, G.router.st.dhcp_deferred);
      CHECK(ms > 0 && ms <= 1000 && G.S.ip == WII_IP && G.router.st.dhcp_deferred == 1); }       /* not at the 6 s retry: the held DISCOVER is offered when the address appears */
    rig_close(&G);
    /* IOS startup fails once: the transport retries after 5 s */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.fail_startup = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    rig_run(&G, 1000);
    CHECK(G.tpi.net == VBBA_NET_FAILED);
    G.io.fail_startup = 0;
    CHECK(dhcp_until(30000) > 0 && G.tpi.st.startups >= 2);
    rig_close(&G);
    /* the guest's port is taken on the Wii: IOS picks another one, traffic still flows */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.port_taken = 7777; G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
    sock = sdnet_bind(&G.S, 7777, 0, 0);
    for (i = 0, got = 0; i < 5 && !got; i++) { (void)sdnet_sendto(&G.S, sock, ECHO_LAN, 7, pay, 10); rig_run(&G, 10); got = recv_all(sock, 0); }
    CHECK(got >= 1 && G.tpi.st.bind_fallback == 1);
    rig_close(&G);
    /* socket fails: datagrams dropped and counted; after recovery the router re-opens */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.fail_socket = 1; G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
    sock = sdnet_bind(&G.S, 7777, 0, 0);
    for (i = 0; i < 5; i++) { (void)sdnet_sendto(&G.S, sock, ECHO_LAN, 7, pay, 10); rig_run(&G, 10); }
    CHECK(recv_all(sock, 0) == 0 && G.router.st.maps_failed >= 1);
    G.io.fail_socket = 0;
    for (i = 0, got = 0; i < 6 && !got; i++) { (void)sdnet_sendto(&G.S, sock, ECHO_LAN, 7, pay, 10); rig_run(&G, 10); got = recv_all(sock, 0); }
    CHECK(got >= 1);
    rig_close(&G);
    /* the start-up request itself takes longer than the address wait (kd/request 6: ~20 s on our Wii, the wait is 12 s): the wait counts from the
       return of the request, so the first poll after it finds the address and no second start-up is needed */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.startup_latency_ms = 20000; G.io.latency_ms = 2; G.io.dhcp_ms = 0;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    { int ms = dhcp_until(40000); printf("   slow start-up request (20 s): lease after %d ms, start-ups %u\n", ms, G.tpi.st.startups); CHECK(ms >= 20000 && ms < 22500 && G.tpi.st.startups == 1 && G.S.ip == WII_IP); }
    rig_close(&G);
    printf("   DHCP held until IOS has an address, startup retry, port-taken fallback, socket failure + recovery, slow start-up request: done\n");
}

/* ---- send path: IOS sendto that completes late, send queue, receive order ---------------------------------------------------------------------- */
/* a guest datagram whose first byte carries a number: sends it and returns sdnet's result */
static int send_n(int sock, vbba_u32 dst, unsigned port, unsigned n, int len)
{
    vbba_u8 pay[64];
    memset(pay, 0x5A, sizeof pay);
    pay[0] = (vbba_u8)n; pay[1] = (vbba_u8)(n >> 8);
    (void)port;
    return sdnet_sendto(&G.S, sock, dst, 7, pay, (sdnet_u16)len);
}
/* answers waiting on a socket, their numbers in arrival order; returns the count */
static int recv_nums(int sock, unsigned *nums, int max)
{
    vbba_u8 rx[200]; sdnet_u32 ip; sdnet_u16 p; int c = 0;
    while (sdnet_pending(&G.S, sock)) { int n = sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &ip, &p); if (n >= 2 && c < max) nums[c++] = (unsigned)rx[0] | ((unsigned)rx[1] << 8); }
    return c;
}
static unsigned g_nums[2000]; static int g_nn;                 /* answers collected so far (sdnet keeps only a few per socket: drain it often) */
static void collect(int sock) { int n; unsigned t[64]; n = recv_nums(sock, t, 64); { int i; for (i = 0; i < n && g_nn < 2000; i++) g_nums[g_nn++] = t[i]; } }
static void run_collect(int sock, int ms) { int i; for (i = 0; i < ms; i++) { rig_run(&G, 1); collect(sock); } }
static int warm_up(int sock, vbba_u32 dst)
{
    int i, n = SDNET_E_ARP;
    for (i = 0; i < 20 && n == SDNET_E_ARP; i++) { n = send_n(sock, dst, 7, 60000, 64); rig_run(&G, 10); }
    (void)recv_all(sock, 0);
    rig_run(&G, 600);                                              /* the socket is open (and any slow warm-up reply is in) before the test counts */
    (void)recv_all(sock, 0);
    return n == 64;
}
static void test_send_path(void)
{
    struct vbba_router_cfg rc;
    int sock, i, got;
    unsigned ok0, tx0, k, hist0[VBBA_TP_HIST], nums[700];
    printf("-- send path: IOS sendto completing late, the send queue, receive order\n");
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    CHECK(warm_up(sock, ECHO_LAN));
    G.tpi.st.txq_peak = 0;                                           /* (the warm-up queued its first datagrams while the socket opened) */
    /* A: a prompt IOS: 100 datagrams, 20 per second, nothing queued, all in the 1..10 ms bucket, every echo comes back */
    ok0 = G.router.st.tx_udp; tx0 = G.tpi.st.tx_sent;
    for (k = 0; k < VBBA_TP_HIST; k++) hist0[k] = G.tpi.st.tx_hist[k];
    for (i = 0, got = 0; i < 100; i++) { CHECK(send_n(sock, ECHO_LAN, 7, (unsigned)i, 64) == 64); rig_run(&G, 50); got += recv_all(sock, 0); }
    rig_run(&G, 100); got += recv_all(sock, 0);
    printf("   prompt IOS: sent 100, accepted %u, queued at most %u, peak in flight %u, sendto latency max %u us, echoes back %d\n",
           G.router.st.tx_udp - ok0, G.tpi.st.txq_peak, G.tpi.st.tx_peak, G.tpi.st.tx_lat_max_us, got);
    CHECK(G.router.st.tx_udp - ok0 == 100 && G.router.st.tx_tp_busy == 0 && G.tpi.st.tx_busy == 0 && G.tpi.st.txq_drop_old == 0);
    CHECK(G.tpi.st.tx_peak >= 1 && G.tpi.st.tx_peak <= 2 && G.tpi.st.tx_lat_max_us >= 500 && G.tpi.st.tx_lat_max_us < 10000);
    CHECK(G.tpi.st.tx_hist[1] - hist0[1] == 100 && got == 100 && G.tpi.st.tx_sent - tx0 == 100);
    CHECK(G.tpi.st.rx_lat_n >= 100 && G.tpi.st.rx_wait_n == G.tpi.st.rx && G.tpi.st.rx_ready_lost == 0);
    /* B: the Wii of the sixth run: sendto needs 600 ms, 20 per second: 12 requests in flight, nothing dropped (8 slots used to drop 36 of 100) */
    G.io.send_latency_ms = 600;
    ok0 = G.router.st.tx_udp; tx0 = G.tpi.st.tx_sent;
    g_nn = 0;
    for (i = 0; i < 100; i++) { CHECK(send_n(sock, ECHO_LAN, 7, 1000u + (unsigned)i, 64) == 64); run_collect(sock, 50); }
    run_collect(sock, 2000);
    got = g_nn;
    printf("   sendto 600 ms at 20/s: accepted %u of 100, no-slot drops %u, queued at most %u, peak in flight %u, sendto latency max %u us, echoes back %d\n",
           G.router.st.tx_udp - ok0, G.router.st.tx_tp_busy, G.tpi.st.txq_peak, G.tpi.st.tx_peak, G.tpi.st.tx_lat_max_us, got);
    CHECK(G.router.st.tx_udp - ok0 == 100 && G.router.st.tx_tp_busy == 0 && G.tpi.st.txq_drop_old == 0 && G.tpi.st.tx_sent - tx0 == 100 && got == 100);
    CHECK(G.tpi.st.tx_peak >= 11 && G.tpi.st.tx_peak <= 13 && VBBA_TP_IOS_TX_INFLIGHT >= 13 && G.tpi.st.txq_peak == 0 && G.tpi.st.tx_lat_max_us >= 600000);
    /* C: 60 datagrams per second (the match traffic of the real online image) with a 300 ms IOS: 18 in flight, no queue, no drop, in order */
    G.io.send_latency_ms = 300;
    vbba_tp_ios_set_tx_cap(&G.tpi, VBBA_TP_IOS_TX);                 /* all 24 buffers may be in IOS: 18 are needed here */
    ok0 = G.router.st.tx_udp; tx0 = G.tpi.st.tx_sent;
    G.tpi.st.tx_peak = 0; G.tpi.st.txq_peak = 0; G.tpi.st.txq_wait_max_ms = 0;
    g_nn = 0;
    for (i = 0; i < 600; i++) { CHECK(send_n(sock, ECHO_LAN, 7, 2000u + (unsigned)i, 64) == 64); run_collect(sock, 17); }
    run_collect(sock, 1500);
    got = g_nn; for (i = 0; i < got && i < 700; i++) nums[i] = g_nums[i];
    printf("   60/s with a 300 ms sendto: sent 600, accepted %u, queued at most %u (wait max %u ms), peak in flight %u, echoes back %d\n", G.router.st.tx_udp - ok0, G.tpi.st.txq_peak, G.tpi.st.txq_wait_max_ms, G.tpi.st.tx_peak, got);
    CHECK(G.router.st.tx_udp - ok0 == 600 && G.router.st.tx_tp_busy == 0 && G.tpi.st.txq_drop_old == 0 && G.tpi.st.txq_peak == 0 && G.tpi.st.txq_wait_max_ms == 0 && got == 600);
    CHECK(G.tpi.st.tx_peak >= 16 && G.tpi.st.tx_peak <= 20);
    for (i = 1; i < got; i++) if (nums[i] != nums[i - 1] + 1) break;
    CHECK(i == got && nums[0] == 2000);                                    /* every datagram came out, in the order it went in */
    /* D: overload: a 1.5 s IOS carries 32 requests in flight = 21 per second; 60 per second pile up: the queue is bounded by the 64 buffers, the oldest queued
       datagram is dropped (counted) when they run out, the rest stay in order and the wait stays bounded */
    G.io.send_latency_ms = 1500;
    vbba_tp_ios_set_tx_cap(&G.tpi, VBBA_TP_IOS_TX_INFLIGHT);
    ok0 = G.router.st.tx_udp;
    G.tpi.st.tx_peak = 0; G.tpi.st.txq_peak = 0; G.tpi.st.txq_wait_max_ms = 0; G.tpi.st.txq_drop_old = 0;
    g_nn = 0;
    for (i = 0; i < 600; i++) { (void)send_n(sock, ECHO_LAN, 7, 3000u + (unsigned)i, 64); run_collect(sock, 17); }
    run_collect(sock, 6000);
    got = g_nn; for (i = 0; i < got && i < 700; i++) nums[i] = g_nums[i];
    printf("   overload (1.5 s sendto, 60/s for 10 s): accepted %u, oldest dropped %u, queued at most %u, wait max %u ms, in flight at most %u, echoes back %d\n",
           G.router.st.tx_udp - ok0, G.tpi.st.txq_drop_old, G.tpi.st.txq_peak, G.tpi.st.txq_wait_max_ms, G.tpi.st.tx_peak, got);
    CHECK(G.tpi.st.txq_drop_old > 100 && G.tpi.st.tx_peak == VBBA_TP_IOS_TX_INFLIGHT && G.tpi.st.txq_peak <= VBBA_TP_IOS_TX && G.tpi.st.txq_wait_max_ms <= 3600);
    CHECK(got > 0 && got + G.tpi.st.txq_drop_old + G.tpi.st.txq_drop_dead == 600);       /* every datagram is either delivered or counted as dropped */
    for (i = 1; i < got; i++) if (nums[i] <= nums[i - 1]) break;
    CHECK(i == got);                                                       /* dropping the oldest keeps the others in order */
    G.io.send_latency_ms = 0;
    /* E: IOS refuses sendto requests (IOS_IoctlvAsync < 0): the datagrams stay queued and go out when it accepts again */
    G.tpi.st.tx_refused = 0;
    G.io.refuse_send = 20;
    g_nn = 0;
    for (i = 0; i < 10; i++) { CHECK(send_n(sock, ECHO_LAN, 7, 4000u + (unsigned)i, 64) == 64); run_collect(sock, 3); }
    run_collect(sock, 500);
    got = g_nn; for (i = 0; i < got && i < 700; i++) nums[i] = g_nums[i];
    printf("   IOS refused %u submissions: datagrams kept %u times, echoes back %d of 10\n", 20u - (unsigned)G.io.refuse_send, G.tpi.st.tx_refused, got);
    CHECK(G.io.refuse_send == 0 && G.tpi.st.tx_refused >= 20 && got == 10);
    for (i = 0; i < got; i++) if (nums[i] != 4000u + (unsigned)i) break;
    CHECK(i == got);
    rig_close(&G);
    /* TOS: with the transport's tos set, every socket gets one IP_TOS setsockopt after its bind; the traffic is unchanged */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1; G.tpi.tos = 0xB8u;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    CHECK(warm_up(sock, ECHO_LAN));
    for (i = 0, got = 0; i < 10; i++) { (void)send_n(sock, ECHO_LAN, 7, (unsigned)i, 64); rig_run(&G, 20); got += recv_all(sock, 0); }
    printf("   TOS 0xB8: setsockopt answers %u (last rc %d), mock socket tos %02X, echoes %d of 10\n", G.tpi.tos_n, (int)G.tpi.tos_rc, (unsigned)G.io.s[0].tos, got);
    CHECK(G.tpi.tos_n >= 1u && G.tpi.tos_rc == 0 && G.io.s[0].tos == 0xB8u && got == 10);
    rig_close(&G);
    /* F: datagrams to sockets that are still opening are queued in order, not replaced (the old one-datagram hold dropped all but the newest) */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 50;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
    {
        int s1 = sdnet_bind(&G.S, 5000, 0, 0), s2 = sdnet_bind(&G.S, 5001, 0, 0), n = SDNET_E_ARP;
        unsigned a[16], b[16];
        n = send_n(s1, ECHO_LAN, 7, 100, 64);                 /* E_ARP: sdnet holds the datagram and sends it when the ARP reply arrives (no resend: that would duplicate it) */
        for (i = 0; i < 20 && G.S.pend.used; i++) rig_run(&G, 1);  /* the held datagram goes out: the socket starts opening */
        CHECK((n == 64 || n == SDNET_E_ARP) && !G.S.pend.used);
        (void)send_n(s1, ECHO_LAN, 7, 101, 64); (void)send_n(s1, ECHO_LAN, 7, 102, 64);
        (void)send_n(s2, ECHO_LAN, 7, 200, 64); (void)send_n(s2, ECHO_LAN, 7, 201, 64);
        rig_run(&G, 1200);
        n = recv_nums(s1, a, 16); i = recv_nums(s2, b, 16);
        printf("   opening sockets: router held %u, queued by the transport %u (peak %u), dropped %u/%u; echoes s1 %d (%u %u %u) s2 %d (%u %u)\n", G.router.st.tx_held, G.router.st.tx_opening, G.tpi.st.txq_peak,
               G.tpi.st.txq_drop_old, G.tpi.st.txq_drop_dead, n, a[0], a[1], a[2], i, b[0], b[1]);
        CHECK(G.router.st.tx_held == 0 && G.router.st.tx_opening >= 5 && G.tpi.st.txq_drop_old == 0 && G.tpi.st.txq_drop_dead == 0);
        CHECK(n == 3 && a[0] == 100 && a[1] == 101 && a[2] == 102 && i == 2 && b[0] == 200 && b[1] == 201);
    }
    rig_close(&G);
    /* G: receive order.  Two answers that finish in the same IOS poll come back in reverse order (replies reordered on the IPC path, datagrams assigned
       first come first served): handed on in request order they reach the guest in the order they arrived; without the ordering they do not */
    for (k = 0; k < 2; k++) {
        unsigned inv = 0, order[200];
        rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
        G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1;
        CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
        sock = sdnet_bind(&G.S, 5000, 0, 0);
        CHECK(warm_up(sock, ECHO_LAN));
        G.io.rx_rev = 1; G.tpi.rx_order = k == 0;
        g_nn = 0;
        for (i = 0; i < 100; i += 2) { (void)send_n(sock, ECHO_LAN, 7, 500u + (unsigned)i, 64); (void)send_n(sock, ECHO_LAN, 7, 501u + (unsigned)i, 64); run_collect(sock, 40); }
        run_collect(sock, 100);
        got = g_nn; for (i = 0; i < got && i < 200; i++) order[i] = g_nums[i];
        for (i = 1; i < got; i++) if (order[i] < order[i - 1]) inv++;
        printf("   receive order %s: %d answers, %u delivered behind a younger one, completions behind a younger request %u, held for an older request %u (handed on after the wait %u)\n",
               k == 0 ? "ON " : "OFF", got, inv, G.tpi.st.rx_inv, G.tpi.st.rx_gated, G.tpi.st.rx_gate_timeout);
        CHECK(got == 100 && G.tpi.st.rx_inv > 10);
        if (k == 0) CHECK(inv == 0); else CHECK(inv > 10);
        rig_close(&G);
    }
}

#if VBBA_TP_RXPOLL
/* ---- b18a receive pattern (VBBA_TP_RXPOLL): one request at a time, poll then recvfrom, no standing receive --------------------------------------- */
static void test_rxpoll(void)
{
    struct vbba_router_cfg rc;
    int sock, i, got, sent, ms;
    unsigned ok0, tx0, rx0, polls0, rs0, ready0, eag0;
    printf("-- receive by poll (libogc pattern): one request in IOS at a time, a recvfrom only after a poll that reported data\n");
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(20000) > 0);
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    CHECK(warm_up(sock, ECHO_LAN));
    /* A: idle for 2 s: polls go on (about one per 3 ms with 1 ms IOS latency), no recvfrom is ever submitted, none fails */
    polls0 = G.tpi.st.poll_n; rs0 = G.io.recv_submitted; eag0 = G.tpi.st.rx_eagain;
    rig_run(&G, 2000);
    printf("   idle 2 s: %u polls, %u recvfrom submitted, %u recvfrom without data\n", G.tpi.st.poll_n - polls0, G.io.recv_submitted - rs0, G.tpi.st.rx_eagain - eag0);
    CHECK(G.tpi.st.poll_n - polls0 >= 500 && G.io.recv_submitted == rs0 && G.tpi.st.rx_eagain == eag0 && G.tpi.st.poll_err == 0);
    /* B: 120 datagrams per second for 5 s, 64 B, 1 ms IOS: everything comes back, in order, nothing dropped, never two requests in IOS */
    ok0 = G.router.st.tx_udp; tx0 = G.tpi.st.tx_sent; rx0 = G.tpi.st.rx; ready0 = G.tpi.st.poll_ready; polls0 = G.tpi.st.poll_n; eag0 = G.tpi.st.rx_eagain;
    G.io.net_pend_max = 0; g_nn = 0;
    for (ms = 0, sent = 0; ms < 5000; ms++) {
        if (ms % 25 == 0 || ms % 25 == 8 || ms % 25 == 17) { CHECK(send_n(sock, ECHO_LAN, 7, 3000u + (unsigned)sent, 64) == 64); sent++; }
        run_collect(sock, 1);
    }
    run_collect(sock, 200);
    got = g_nn;
    printf("   120/s for 5 s: sent %d, accepted %u, handed to IOS %u, echoes back %d, oldest dropped %u, polls %u (ready %u), recvfrom %u (without data %u), most requests in IOS at once %u\n",
           sent, G.router.st.tx_udp - ok0, G.tpi.st.tx_sent - tx0, got, G.tpi.st.txq_drop_old, G.tpi.st.poll_n - polls0, G.tpi.st.poll_ready - ready0, G.tpi.st.rx - rx0, G.tpi.st.rx_eagain - eag0, G.io.net_pend_max);
    CHECK(sent == 600 && G.router.st.tx_udp - ok0 == 600 && G.tpi.st.tx_sent - tx0 == 600 && got == 600 && G.tpi.st.txq_drop_old == 0 && G.tpi.st.tx_busy == 0);
    CHECK(G.io.net_pend_max == 1 && G.tpi.st.rx_eagain == eag0 && G.tpi.st.poll_ready - ready0 == G.tpi.st.rx - rx0 && G.tpi.st.poll_err == 0);
    for (i = 1; i < got; i++) if (g_nums[i] <= g_nums[i - 1]) { CHECK(g_nums[i] > g_nums[i - 1]); break; }
    /* C: a send queued while a poll is in IOS waits for it (strictly sequential), and the poll waits for a sendto: both sides make progress under 600/s of 64 B for 3 s */
    ok0 = G.router.st.tx_udp; tx0 = G.tpi.st.tx_sent; rx0 = G.tpi.st.rx; G.io.net_pend_max = 0; g_nn = 0;
    for (ms = 0, sent = 0; ms < 3000; ms++) { int k; for (k = 0; k < 1; k++) { (void)send_n(sock, ECHO_LAN, 7, 6000u + (unsigned)sent, 64); sent++; } run_collect(sock, 1); }
    run_collect(sock, 400);
    printf("   overload 1000/s for 3 s: handed to IOS %u of %d, oldest dropped %u, echoes back %d, received %u, most requests in IOS at once %u\n",
           G.tpi.st.tx_sent - tx0, sent, G.tpi.st.txq_drop_old, g_nn, G.tpi.st.rx - rx0, G.io.net_pend_max);
    CHECK(G.io.net_pend_max == 1 && G.tpi.st.tx_sent - tx0 > 300 && G.tpi.st.rx - rx0 > 300 && G.tpi.st.rx - rx0 >= (unsigned)g_nn);
    /* D: IOS refuses every poll: the receive side falls back to the non-blocking recvfrom (answers still come back) and the failures are counted */
    G.io.fail_poll = 1; ok0 = G.router.st.tx_udp; eag0 = G.tpi.st.rx_eagain; polls0 = G.tpi.st.poll_err; g_nn = 0;
    for (i = 0; i < 100; i++) { CHECK(send_n(sock, ECHO_LAN, 7, 9000u + (unsigned)i, 64) == 64); run_collect(sock, 50); }
    run_collect(sock, 200);
    printf("   every poll refused: echoes back %d of 100, poll errors %u, recvfrom without data %u\n", g_nn, G.tpi.st.poll_err - polls0, G.tpi.st.rx_eagain - eag0);
    CHECK(g_nn == 100 && G.tpi.st.poll_err - polls0 > 100 && G.tpi.st.rx_eagain - eag0 > 0);
    G.io.fail_poll = 0;
    rig_close(&G);
}
#endif

/* the chip MAC follows the active interface: swap before the guest's first reset = the guest reads it; swap after = UDP keeps working with the MAC the guest
   already uses (the old swap changed the filter under a running guest and inbound unicast was dropped), and the next adapter init (Reconnect) picks the new one up */
static void test_mac_swap(void)
{
    struct vbba_router_cfg rc;
    static const vbba_u8 M1[6] = { 0xE8, 0x4E, 0xCE, 0x63, 0x0E, 0x1C }, M2[6] = { 0x00, 0x17, 0xAB, 0x12, 0x34, 0x56 };
    int sock, i, got;
    printf("-- chip MAC follows the active interface (set_mac before / after the guest's first reset, then Reconnect)\n");
    rig_router_cfg(&rc, VBBA_RF_MIRROR);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO_LAN; G.io.latency_ms = 1;
    vbba_chip_set_mac(&G.chip, M1);                                   /* early (the ncd Wi-Fi MAC) */
    CHECK(rig_bringup(&G, 1) == BBA_OK && memcmp(G.B.mac, M1, 6) == 0 && dhcp_until(20000) > 0);
    sock = sdnet_bind(&G.S, 5000, 0, 0);
    CHECK(warm_up(sock, ECHO_LAN));
    vbba_chip_set_mac(&G.chip, M2);                                   /* later: ip/top says the LAN adapter is the active one */
    CHECK(memcmp(G.chip.regs + 0x20, M1, 6) == 0 && memcmp(G.chip.mac, M2, 6) == 0);      /* the guest keeps what it read */
    g_nn = 0;
    for (i = 0; i < 40; i++) { CHECK(send_n(sock, ECHO_LAN, 7, 3000u + (unsigned)i, 64) == 64); run_collect(sock, 25); }
    run_collect(sock, 200);
    got = g_nn;
    CHECK(got == 40);                                                 /* all 40 echoes came back after the swap */
    CHECK(rig_bringup(&G, 1) == BBA_OK && memcmp(G.B.mac, M2, 6) == 0 && dhcp_until(20000) > 0);       /* Reconnect: new adapter init reads the new MAC, new lease */
    sock = sdnet_bind(&G.S, 5001, 0, 0);
    CHECK(warm_up(sock, ECHO_LAN));
    g_nn = 0;
    for (i = 0; i < 40; i++) { CHECK(send_n(sock, ECHO_LAN, 7, 4000u + (unsigned)i, 64) == 64); run_collect(sock, 25); }
    run_collect(sock, 200);
    CHECK(g_nn == 40);
    printf("   echoes back after the swap %d of 40, after Reconnect %d of 40, PAR read by the driver %02X:%02X:%02X:%02X:%02X:%02X\n", got, g_nn, G.B.mac[0], G.B.mac[1], G.B.mac[2], G.B.mac[3], G.B.mac[4], G.B.mac[5]);
    rig_close(&G);
}

/* ---- many guest ports: the game socket, the server session, a port that only receives and short-lived test ports ------------------------- */
/* the router's backend with a count of the datagrams handed to the guest, by guest port (the guest's sdnet has 4 sockets: the frames go to the backend directly) */
static unsigned g_port_rx[65536];
static const struct vbba_frame_backend *rbe(void) { return vbba_router_backend(&G.router); }
static int w_caps(void *c, struct vbba_frame_caps *o) { (void)c; return rbe()->get_capabilities(rbe()->context, o); }
static int w_tx(void *c, const vbba_u8 *f, vbba_u16 n) { (void)c; return rbe()->submit_tx(rbe()->context, f, n); }
static int w_rx(void *c, vbba_u8 *f, vbba_u16 cap, vbba_u16 *n)
{
    int rc = rbe()->receive_rx(rbe()->context, f, cap, n);
    (void)c;
    if (rc == VBBA_OK && *n >= 42 && g16(f + 12) == 0x0800 && f[23] == 17) g_port_rx[g16(f + 36)]++;
    return rc;
}
static int w_poll(void *c) { (void)c; return rbe()->poll(rbe()->context); }
static void w_reset(void *c) { (void)c; rbe()->reset(rbe()->context); }
static const struct vbba_frame_backend W_BE = { 0, w_caps, w_tx, w_rx, w_poll, w_reset };

static void port_send(unsigned port, unsigned *sent)
{
    vbba_u8 pay[16], f[128];
    int n;
    memset(pay, (int)(port & 0xffu), sizeof pay);
    n = mk_udp(f, G.S.ip, ECHO_FAR, port, 7, pay, (int)sizeof pay);
    CHECK(G.be->submit_tx(G.be->context, f, (vbba_u16)n) == VBBA_OK);
    if (sent) (*sent)++;
}

static void test_many_ports(void)
{
    struct vbba_router_cfg rc;
    enum { NACT = 3, NTEST = 12 };
    static const unsigned act[NACT] = { 43441, 43150, 44000 };   /* game socket, server session (both send every 100 ms), the third only receives (pushed every 500 ms) */
    unsigned sent[NACT] = { 0 }, push = 0, tsent[NTEST] = { 0 }, ms, k;
    vbba_u8 pay[16];
    printf("-- many guest ports: %u maps, %u IOS sockets; 3 long-lived ports (one receive-only) and %u short test ports, one new every 3 s\n",
           (unsigned)VBBA_MAPS, (unsigned)VBBA_TP_IOS_SOCKS, (unsigned)NTEST);
    memset(g_port_rx, 0, sizeof g_port_rx); memset(pay, 0x33, sizeof pay);
    rig_router_cfg(&rc, VBBA_RF_ICMP);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, &W_BE);
    G.io.echo_ip = ECHO_FAR; G.io.latency_ms = 5;                           /* a close takes 5 ms in IOS: an evicted socket is still busy when its replacement opens */
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(10000) > 0);
    port_send(act[2], 0);                                                    /* the receive-only port: one datagram out (conntrack), then only pushes in */
    rig_run(&G, 20);
    memset(g_port_rx, 0, sizeof g_port_rx);
    for (ms = 0; ms < 3000u * NTEST + 12000u; ms += 10) {
        if (ms % 100 == 0) { port_send(act[0], &sent[0]); port_send(act[1], &sent[1]); }
        if (ms % 500 == 0) { (void)mock_ios_inject(&G.io, (vbba_u16)act[2], ECHO_FAR, 7, pay, sizeof pay); push++; }   /* no socket on the port any more (evicted): the push is lost */
        k = ms / 3000u;
        if (k < NTEST && ms % 3000u < 300u && ms % 100u == 0) port_send(50000u + k, &tsent[k]);   /* a test port: three datagrams, then quiet */
        rig_run(&G, 10);
    }
    rig_run(&G, 50);
    printf("   game %u/%u, server %u/%u, receive-only %u/%u replies; maps opened %u evicted %u failed %u full %u peak %u, newest eviction port %u quiet %u ms\n",
           g_port_rx[act[0]], sent[0], g_port_rx[act[1]], sent[1], g_port_rx[act[2]], push, G.router.st.maps_opened, G.router.st.maps_evicted,
           G.router.st.maps_failed, G.router.st.maps_full, G.router.st.maps_peak, G.router.st.evict_port, G.router.st.evict_idle_ms);
    CHECK(g_port_rx[act[0]] == sent[0] && g_port_rx[act[1]] == sent[1] && g_port_rx[act[2]] == push);
    for (k = 0; k < NTEST; k++) { CHECK(g_port_rx[50000u + k] == tsent[k]); if (g_port_rx[50000u + k] != tsent[k]) printf("   test port %u: %u/%u\n", 50000u + k, g_port_rx[50000u + k], tsent[k]); }
    CHECK(G.router.st.maps_evicted >= 1 && G.router.st.maps_failed == 0 && G.router.st.maps_full == 0 && G.router.st.evict_port >= 50000u);
    CHECK(G.router.st.evict_idle_ms >= VBBA_MAP_KEEP_MS && G.router.st.maps_peak <= VBBA_MAPS);
    rig_close(&G);

    /* more active ports than maps: the established ones keep every reply, the extra one is refused (counted), nothing active is evicted */
    rig_setup(&G, 0, MOCK_MEM, WII_IP, &rc, VBBA_CHIP_AUTH, &W_BE);
    G.io.echo_ip = ECHO_FAR; G.io.latency_ms = 5;
    CHECK(rig_bringup(&G, 1) == BBA_OK && dhcp_until(10000) > 0);
    memset(g_port_rx, 0, sizeof g_port_rx);
    {
        unsigned s2[VBBA_MAPS + 1], i, ok = 1;
        memset(s2, 0, sizeof s2);
        for (ms = 0; ms < 20000u; ms += 10) {
            if (ms % 200 == 0) for (i = 0; i < VBBA_MAPS; i++) port_send(40000u + i, &s2[i]);
            if (ms >= 1000u && ms % 200 == 100) port_send(40000u + VBBA_MAPS, &s2[VBBA_MAPS]);
            rig_run(&G, 10);
        }
        rig_run(&G, 50);
        for (i = 0; i < VBBA_MAPS; i++) if (g_port_rx[40000u + i] != s2[i]) { ok = 0; printf("   active port %u: %u/%u\n", 40000u + i, g_port_rx[40000u + i], s2[i]); }
        printf("   %u ports active at once + 1 more: established replies %s, extra port %u/%u delivered, refused %u times, evicted %u\n",
               (unsigned)VBBA_MAPS, ok ? "all delivered" : "LOST", g_port_rx[40000u + VBBA_MAPS], s2[VBBA_MAPS], G.router.st.maps_full, G.router.st.maps_evicted);
        CHECK(ok && G.router.st.maps_evicted == 0 && G.router.st.maps_full >= 1 && g_port_rx[40000u + VBBA_MAPS] == 0);
    }
    rig_close(&G);
    printf("   many ports: no active port lost a reply, quiet ports evicted, an extra port never takes an active map: done\n");
}

int main(void)
{
#if VBBA_TP_RXPOLL
    test_rxpoll();                                       /* the other sections time their expectations on standing receives: only this one applies to this receive pattern */
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
#endif
    scenario("imm only, private subnet, patched stub", 0, 0, RIG_STUB_PATCHED, 0, 0, 12);
    scenario("dma + imm tail, private subnet, patched stub", 1, 0, RIG_STUB_PATCHED, 0, 0, 12);
    scenario("dma, MIRROR (lease = Wii address), on-link peer via proxy ARP, ARM interleaved", 1, 1, RIG_STUB_PATCHED, 0, 1, 12);
    scenario("imm, STOCK Nintendont stub (3-byte reads dropped): expected corruption", 0, 0, RIG_STUB_STOCK, 0, 0, 10);
    scenario("dma, STOCK stub, game glue splits 3-byte reads (image-side fix)", 1, 0, RIG_STUB_STOCK, 1, 1, 12);
    printf("-- EXI trips per driver operation (each trip = one PowerPC spin on the ARM)\n");
    trip_costs(0);
    trip_costs(1);
    test_router_wrap();
    test_tiny_retry();
    test_chip_wrap(0);
    test_chip_wrap(1);
    test_errors();
    test_policy();
    test_dns();
    test_ios();
    test_send_path();
    test_mac_swap();
    test_many_ports();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
