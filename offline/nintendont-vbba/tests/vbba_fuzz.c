/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * Fuzz (run under ASan+UBSan): hostile guest frames into the router, hostile datagrams from the network side, random
 * EXI command streams into the chip, ARP storms.  Invariants checked on every step:
 *   - nothing leaves for a destination outside the allow-list;
 *   - every frame the router produces is well formed (IPv4 header checksum, UDP checksum, lengths, <= capacity);
 *   - the chip never reports a ring pointer outside its ring once the ring is valid;
 *   - afterwards the unchanged driver re-initialises and a UDP echo works again (recovery).
 * usage: vbba_fuzz [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vbba_rig.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; if (g_fail < 20) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define IP4(a, b, c, d) (((vbba_u32)(a) << 24) | ((vbba_u32)(b) << 16) | ((vbba_u32)(c) << 8) | (vbba_u32)(d))
#define ECHO IP4(192, 168, 7, 20)

static struct rig G;
static unsigned long long rs = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }
static void p16(vbba_u8 *p, unsigned v) { p[0] = (vbba_u8)(v >> 8); p[1] = (vbba_u8)v; }
static void p32(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }
static unsigned g16(const vbba_u8 *p) { return ((unsigned)p[0] << 8) | p[1]; }

static unsigned produced, prod_arp, prod_udp, prod_icmp, bad_frames, sent_out, denied_leak;

static void validate(const vbba_u8 *f, unsigned n, unsigned cap)
{
    unsigned type;
    produced++;
    if (n > cap || n < 42) { bad_frames++; if (bad_frames < 4) printf("   malformed: len %u cap %u\n", n, cap); return; }
    type = g16(f + 12);
    if (type == 0x0806) { prod_arp++; if (g16(f + 20) != 2) bad_frames++; return; }
    if (type != 0x0800) { bad_frames++; return; }
    {
        const vbba_u8 *ip = f + 14;
        unsigned ihl = (ip[0] & 15u) * 4u, tot = g16(ip + 2);
        if ((ip[0] >> 4) != 4 || ihl != 20 || tot + 14 > n || vbba_csum(ip, 20, 0) != 0) { bad_frames++; return; }
        if (ip[9] == 17) {
            const vbba_u8 *u = ip + 20;
            vbba_u8 ph[12];
            vbba_u32 s = 0;
            int i;
            if (g16(u + 4) != tot - 20) { bad_frames++; return; }
            memcpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 17; p16(ph + 10, tot - 20);
            for (i = 0; i < 12; i += 2) s += ((vbba_u32)ph[i] << 8) | ph[i + 1];
            if (g16(u + 6) && vbba_csum(u, tot - 20, s) != 0) bad_frames++;
            prod_udp++;
        } else if (ip[9] == 1) {
            if (vbba_csum(ip + 20, tot - 20, 0) != 0) bad_frames++;
            prod_icmp++;
        } else bad_frames++;
    }
}

static void drain_router(void)
{
    const struct vbba_frame_backend *be = vbba_router_backend(&G.router);
    vbba_u8 f[VBBA_FRAME_MAX + 16];
    int k;
    for (k = 0; k < 16; k++) {
        vbba_u16 n = 0;
        unsigned cap = (rnd() & 3) ? VBBA_FRAME_MAX : rnd() % (VBBA_FRAME_MAX + 1);
        memset(f + cap, 0xEE, 16);
        if (be->receive_rx(be->context, f, (vbba_u16)cap, &n) != VBBA_OK) break;
        validate(f, n, cap);
        CHECK(f[cap] == 0xEE);                                     /* never past the capacity */
    }
}

static void check_out(unsigned sent0)
{
    if (G.io.sent != sent0) {
        vbba_u32 d = G.io.last_tx_ip;
        sent_out++;
        if (!((d >> 16) == 0xC0A8u || (d >> 24) == 127u)) denied_leak++;
    }
}

/* ---- templates ---- */
static int t_udp(vbba_u8 *f, vbba_u32 sip, vbba_u32 dip, unsigned sp, unsigned dp, int n)
{
    vbba_u8 *ip = f + 14, *u = f + 34, ph[12];
    vbba_u32 s = 0;
    int i;
    memcpy(f, RIG_ROUTER_MAC, 6); memcpy(f + 6, RIG_MAC, 6); p16(f + 12, 0x0800);
    ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 28 + n); p16(ip + 4, 1); p16(ip + 6, 0); ip[8] = 64; ip[9] = 17; p16(ip + 10, 0);
    p32(ip + 12, sip); p32(ip + 16, dip); p16(ip + 10, vbba_csum(ip, 20, 0));
    p16(u, sp); p16(u + 2, dp); p16(u + 4, 8 + n); p16(u + 6, 0);
    for (i = 0; i < n; i++) u[8 + i] = (vbba_u8)rnd();
    memcpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 17; p16(ph + 10, 8 + n);
    for (i = 0; i < 12; i += 2) s += ((vbba_u32)ph[i] << 8) | ph[i + 1];
    s = vbba_csum(u, 8 + n, s); p16(u + 6, s ? s : 0xffff);
    return 42 + n < 60 ? 60 : 42 + n;
}
static int t_arp(vbba_u8 *f, vbba_u32 spa, vbba_u32 tpa)
{
    memset(f, 0, 60); memset(f, 0xff, 6); memcpy(f + 6, RIG_MAC, 6); p16(f + 12, 0x0806);
    p16(f + 14, 1); p16(f + 16, 0x0800); f[18] = 6; f[19] = 4; p16(f + 20, 1);
    memcpy(f + 22, RIG_MAC, 6); p32(f + 28, spa); p32(f + 38, tpa);
    return 60;
}
static int t_dhcp(vbba_u8 *f, int type)
{
    vbba_u8 m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6; p32(m + 4, rnd()); memcpy(m + 28, RIG_MAC, 6); p32(m + 236, 0x63825363u);
    m[240] = 53; m[241] = 1; m[242] = (vbba_u8)type; m[243] = 50; m[244] = 4; p32(m + 245, rnd() & 1 ? G.S.ip : rnd()); m[249] = 255;
    {
        int n = t_udp(f, 0, 0xffffffffu, 68, 67, 0);
        vbba_u8 *ip = f + 14, *u = f + 34, ph[12];
        vbba_u32 s = 0;
        int i;
        (void)n;
        memset(f, 0xff, 6);
        memcpy(u + 8, m, 300); p16(ip + 2, 328); p16(u + 4, 308); p16(ip + 10, 0); p16(ip + 10, vbba_csum(ip, 20, 0));
        p16(u + 6, 0);
        memcpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 17; p16(ph + 10, 308);
        for (i = 0; i < 12; i += 2) s += ((vbba_u32)ph[i] << 8) | ph[i + 1];
        s = vbba_csum(u, 308, s); p16(u + 6, s ? s : 0xffff);
    }
    return 342;
}

static void fuzz_router(long iters)
{
    const struct vbba_frame_backend *be;
    vbba_u8 f[1700];
    long i;
    unsigned sent0;
    struct vbba_router_cfg rc;
    printf("-- router: %ld hostile guest frames + %ld hostile datagrams\n", iters, iters / 4);
    rig_router_cfg(&rc, VBBA_RF_ICMP);
    rc.arp_per_s = 1000000; rc.tx_per_s = 1000000;                 /* let the parsers see everything */
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.interleave = 0;
    rig_setup(&G, 0, MOCK_MEM, IP4(192, 168, 1, 50), &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    rig_run(&G, 3000);
    CHECK(sdnet_dhcp_state(&G.S) == SDNET_DHCP_BOUND);
    be = vbba_router_backend(&G.router);
    for (i = 0; i < iters; i++) {
        int n, k, kind = (int)(rnd() % 6), muts;
        vbba_u32 dst;
        switch (kind) {
        case 0: n = t_arp(f, rnd() & 1 ? G.S.ip : rnd(), rnd() & 1 ? IP4(192, 168, 1, rnd() & 255) : rnd()); break;
        case 1: n = t_dhcp(f, (int)(rnd() % 9)); break;
        case 2: dst = (rnd() & 1) ? ECHO : rnd(); n = t_udp(f, (rnd() & 7) ? G.S.ip : rnd(), dst, rnd() & 0xffff, rnd() % 3 ? 7 : rnd() & 0xffff, (int)(rnd() % 1473)); break;
        case 3:                                                  /* ICMP echo request to the router */
            n = t_udp(f, G.S.ip, 0x0AD30001u, 1000, 7, (int)(rnd() % 400));
            f[23] = 1; p16(f + 24, 0); p16(f + 24, vbba_csum(f + 14, 20, 0));
            f[34] = 8; f[35] = 0; p16(f + 36, 0); p16(f + 36, vbba_csum(f + 34, g16(f + 16) - 20u, 0));
            break;
        case 4: n = (int)(rnd() % 1600); for (k = 0; k < n; k++) f[k] = (vbba_u8)rnd(); break;
        default: n = t_udp(f, G.S.ip, ECHO, 7777, 7, (int)(rnd() % 600)); break;
        }
        muts = (int)(rnd() % 4);
        for (k = 0; k < muts; k++) {
            switch (rnd() % 5) {
            case 0: f[rnd() % (unsigned)(n ? n : 1)] ^= (vbba_u8)(1u << (rnd() % 8)); break;
            case 1: n = (int)(rnd() % (unsigned)(n + 1)); break;                       /* truncate */
            case 2: { int e = (int)(rnd() % 200); for (; e > 0 && n < 1600; e--) f[n++] = (vbba_u8)rnd(); } break;
            case 3: p16(f + 16, rnd() & 0xffff); break;                                 /* IPv4 total length */
            default: p16(f + 38, rnd() & 0xffff); break;                                /* UDP length */
            }
        }
        sent0 = G.io.sent;
        (void)be->submit_tx(be->context, f, (vbba_u16)n);
        if ((i & 15) == 0) { G.us += 1000; G.io.now = (vbba_u32)(G.us / 1000); }
        (void)be->poll(be->context);
        (void)be->poll(be->context);
        check_out(sent0);
        drain_router();
        if ((i & 3) == 0) {                                          /* the network side */
            static vbba_u8 big[2100];
            unsigned len = rnd() % 2100;
            for (k = 0; k < (int)len; k += 7) big[k] = (vbba_u8)rnd();
            (void)mock_ios_inject(&G.io, (vbba_u16)((rnd() & 1) ? 7777 : (rnd() & 0xffff)), (rnd() & 1) ? ECHO : rnd(), (vbba_u16)((rnd() & 1) ? 7 : rnd()), big, len);
        }
    }
    printf("   produced %u frames (arp %u, udp %u, icmp %u), malformed %u, datagrams out %u, outside the allow-list %u\n",
           produced, prod_arp, prod_udp, prod_icmp, bad_frames, sent_out, denied_leak);
    printf("   router: tx %u udp %u denied %u bad %u csum %u frag %u tcp %u spoof %u bcast %u | dhcp disc %u req %u bad %u | "
           "rx %u unsolicited %u trunc %u | maps %u evicted %u\n",
           G.router.st.tx_frames, G.router.st.tx_udp, G.router.st.tx_denied, G.router.st.tx_bad, G.router.st.tx_bad_csum,
           G.router.st.tx_frag, G.router.st.tx_tcp, G.router.st.tx_spoof, G.router.st.tx_bcast, G.router.st.dhcp_disc,
           G.router.st.dhcp_req, G.router.st.dhcp_bad, G.router.st.rx_dgrams, G.router.st.rx_unsolicited, G.router.st.rx_trunc,
           G.router.st.maps_opened, G.router.st.maps_evicted);
    CHECK(bad_frames == 0 && denied_leak == 0 && produced > 0 && G.router.st.tx_denied > 0 && G.router.st.rx_unsolicited > 0);
    rig_close(&G);
}

static void fuzz_chip(long iters)
{
    struct vbba_router_cfg rc;
    vbba_u8 b[300], pay[64], rx[600];
    sdnet_u32 sip; sdnet_u16 sp;
    long i;
    int k, sock, ok = 0, bad_ptr = 0;
    printf("-- chip: %ld random EXI transactions from a hostile guest, then recovery\n", iters);
    rig_router_cfg(&rc, 0);
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.interleave = 1;
    rig_setup(&G, 0, MOCK_MEM, IP4(192, 168, 1, 50), &rc, VBBA_CHIP_AUTH, 0);
    G.io.echo_ip = ECHO;
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    rig_run(&G, 3000);
    sock = sdnet_bind(&G.S, 7777, 0, 0);
    for (i = 0; i < iters; i++) {
        int n = (int)(rnd() % 64) + 1, w = (int)(rnd() & 1);
        vbba_chip_select(&G.chip);
        for (k = 0; k < n; k++) b[k] = (vbba_u8)rnd();
        if (rnd() & 1) { b[0] = (rnd() & 1) ? 0xC0 : 0x80; b[1] = (vbba_u8)(rnd() % 0x20); }   /* aim at real registers */
        vbba_chip_xfer(&G.chip, b, 4, 1);
        vbba_chip_xfer(&G.chip, b + 4, (vbba_u32)n, w);
        if (rnd() % 3 == 0) vbba_chip_deselect(&G.chip);
        if ((i & 31) == 0) { (void)sdnet_sendto(&G.S, sock, ECHO, 7, b, 16); G.us += 1000; G.io.now = (vbba_u32)(G.us / 1000); }
        {
            unsigned rx0 = G.chip.st.rx_frames;
            (void)vbba_chip_poll(&G.chip, 2);
            if (G.chip.st.rx_frames != rx0) {                     /* a frame went in: RWP must be inside BP..RHBP */
                unsigned bp = (G.chip.regs[0x0a] | (G.chip.regs[0x0b] << 8)) & 0xfff, rh = (G.chip.regs[0x1a] | (G.chip.regs[0x1b] << 8)) & 0xfff;
                unsigned rw = vbba_chip_rwp(&G.chip);
                if (rw < bp || rw > rh || rh > 16) bad_ptr++;
            }
        }
    }
    printf("   chip: selects %u bytes in %u out %u, tx frames %u runt %u overflow %u, rx frames %u bad-ring %u filtered %u off %u, "
           "resets %u, unknown cmds %u\n", G.chip.st.selects, G.chip.st.bytes_in, G.chip.st.bytes_out, G.chip.st.tx_frames,
           G.chip.st.tx_runt, G.chip.st.tx_overflow, G.chip.st.rx_frames, G.chip.st.rx_bad_ring, G.chip.st.rx_filtered,
           G.chip.st.rx_off_drop, G.chip.st.resets, G.chip.st.bad_cmds);
    /* recovery: the driver re-initialises the chip and traffic flows again */
    vbba_chip_deselect(&G.chip);
    CHECK(rig_bringup(&G, 1) == BBA_OK);
    rig_run(&G, 20000);
    CHECK(sdnet_dhcp_state(&G.S) == SDNET_DHCP_BOUND);
    sock = sdnet_bind(&G.S, 7778, 0, 0);
    memset(pay, 0x3c, sizeof pay);
    for (k = 0; k < 20 && ok < 5; k++) {
        (void)sdnet_sendto(&G.S, sock, ECHO, 7, pay, 40);
        rig_run(&G, 5);
        while (sdnet_pending(&G.S, sock)) if (sdnet_recvfrom(&G.S, sock, rx, sizeof rx, &sip, &sp) == 40 && !memcmp(rx, pay, 40)) ok++;
    }
    printf("   recovery after re-init: %d echoes ok, bad pointers %d\n", ok, bad_ptr);
    CHECK(ok >= 5 && bad_ptr == 0);
    rig_close(&G);
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 200000;
    fuzz_router(n);
    fuzz_chip(n);
    printf("fuzz: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
