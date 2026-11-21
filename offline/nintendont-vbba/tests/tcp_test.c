/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* Host test of the router's TCP splice (core/vbba_tcp.c): the REAL guest stack (offline/bba driver + the project's other tooling + the project's other tooling,
   unchanged) -> rig EXI model -> vbba_chip -> vbba_router (VBBA_TCP=1) -> host stream sockets on 127.0.0.1 -> an in-process TCP server.
   Faults: frame loss / reordering toward the guest and loss from the guest (a backend wrapper), short socket sends/receives (s_chunk).
   Build: see run_tests.sh (-DVBBA_TCP=1 -DSDNET_ENABLE_TCP=1, sdtcp.c).  Usage: tcp_test [mm PORT] */
#include "vbba_rig.h"
#include "../../net/sdtcp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct rig G;
static struct sdtcp T;
static struct sdtcp_conn TC[2];
static unsigned char trx[2][4096], ttx[2][4096];

/* ---- lossy backend wrapper ---- */
static struct { unsigned loss_rx, loss_tx, reorder; unsigned seed; unsigned char held[1600]; vbba_u16 held_len; int have; unsigned dropped_rx, dropped_tx, reordered; } F;
static unsigned rnd(void) { F.seed = F.seed * 1103515245u + 12345u; return (F.seed >> 8) % 1000u; }
static int is_tcp(const vbba_u8 *f, vbba_u16 n) { return n >= 34 && f[12] == 8 && f[13] == 0 && f[23] == 6; }
static int w_caps(void *c, struct vbba_frame_caps *o) { const struct vbba_frame_backend *b = vbba_router_backend(&G.router); (void)c; return b->get_capabilities(b->context, o); }
static int w_tx(void *c, const vbba_u8 *f, vbba_u16 n)
{
    const struct vbba_frame_backend *b = vbba_router_backend(&G.router); (void)c;
    if (is_tcp(f, n) && rnd() < F.loss_tx) { F.dropped_tx++; return VBBA_OK; }
    return b->submit_tx(b->context, f, n);
}
static int w_rx(void *c, vbba_u8 *f, vbba_u16 cap, vbba_u16 *len)
{
    const struct vbba_frame_backend *b = vbba_router_backend(&G.router);
    int rc; (void)c;
    if (F.have == 2 && F.held_len <= cap) { memcpy(f, F.held, F.held_len); *len = F.held_len; F.have = 0; return VBBA_OK; }   /* the held frame, after the one that overtook it */
    for (;;) {
        rc = b->receive_rx(b->context, f, cap, len);
        if (rc != VBBA_OK) {
            if (F.have == 1 && F.held_len <= cap) { memcpy(f, F.held, F.held_len); *len = F.held_len; F.have = 0; return VBBA_OK; }
            return rc;
        }
        if (!is_tcp(f, *len)) return rc;
        if (rnd() < F.loss_rx) { F.dropped_rx++; continue; }
        if (!F.have && rnd() < F.reorder) { memcpy(F.held, f, *len); F.held_len = *len; F.have = 1; F.reordered++; continue; }
        if (F.have == 1) F.have = 2;
        return rc;
    }
}
static void w_poll_dummy(void) {}
static int w_poll(void *c) { const struct vbba_frame_backend *b = vbba_router_backend(&G.router); (void)c; return b->poll(b->context); }
static void w_reset(void *c) { const struct vbba_frame_backend *b = vbba_router_backend(&G.router); (void)c; b->reset(b->context); }
static struct vbba_frame_backend WB;

/* ---- in-process server: modes echo / source N then close / sink ---- */
enum { SRV_ECHO, SRV_SOURCE, SRV_BOTH };
struct srv { int lfd, fd, mode; unsigned port, want, sent, got, closed_by_peer, rx_ok; unsigned char out[8192]; unsigned olen; };
static unsigned char pat(unsigned i) { return (unsigned char)((i * 2654435761u) >> 24) ^ (unsigned char)i; }
static void srv_open(struct srv *s, int mode, unsigned want)
{
    struct sockaddr_in a; socklen_t l = sizeof a; int one = 1;
    memset(s, 0, sizeof *s); s->fd = -1; s->mode = mode; s->want = want; s->rx_ok = 1;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7F000001u);
    bind(s->lfd, (struct sockaddr *)&a, sizeof a); listen(s->lfd, 4);
    getsockname(s->lfd, (struct sockaddr *)&a, &l); s->port = ntohs(a.sin_port);
    fcntl(s->lfd, F_SETFL, O_NONBLOCK);
}
static void srv_poll(struct srv *s)
{
    unsigned char b[4096]; ssize_t n;
    if (s->fd < 0) { s->fd = accept(s->lfd, 0, 0); if (s->fd >= 0) fcntl(s->fd, F_SETFL, O_NONBLOCK); return; }
    if (s->closed_by_peer == 2) return;
    n = recv(s->fd, b, sizeof b, 0);
    if (n > 0) {
        unsigned k;
        if (s->mode != SRV_ECHO) for (k = 0; k < (unsigned)n; k++) if (b[k] != pat(s->got + k)) s->rx_ok = 0;
        s->got += (unsigned)n;
        if (s->mode == SRV_ECHO && s->olen + (unsigned)n <= sizeof s->out) { memcpy(s->out + s->olen, b, (size_t)n); s->olen += (unsigned)n; }
    } else if (n == 0) s->closed_by_peer = 1;
    if (s->mode == SRV_ECHO && s->olen) {
        n = send(s->fd, s->out, s->olen, 0);
        if (n > 0) { memmove(s->out, s->out + n, s->olen - (unsigned)n); s->olen -= (unsigned)n; s->sent += (unsigned)n; }
    }
    if (s->mode != SRV_ECHO && s->sent < s->want) {
        unsigned k, m = s->want - s->sent; if (m > sizeof b) m = sizeof b;
        for (k = 0; k < m; k++) b[k] = pat(s->sent + k);
        n = send(s->fd, b, m, 0);
        if (n > 0) s->sent += (unsigned)n;
    }
    if (s->mode == SRV_SOURCE && s->sent >= s->want && !s->closed_by_peer) { shutdown(s->fd, SHUT_WR); }
    if (s->closed_by_peer == 1 && !s->olen && (s->mode == SRV_ECHO || s->sent >= s->want)) { close(s->fd); s->fd = -1; s->closed_by_peer = 2; s->fd = -2; }
}
static void srv_close(struct srv *s) { if (s->fd >= 0) close(s->fd); close(s->lfd); }

static void step(struct srv *s)
{
    struct timespec ts = { 0, 200000L };
    rig_arm(&G); rig_arm(&G);
    sdnet_poll(&G.S);
    if (s) srv_poll(s);
    nanosleep(&ts, 0);
}

static void bring_up(int lossy, int ios)
{
    struct vbba_router_cfg rc;
    static const unsigned char seed[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    unsigned t0;
    rig_router_cfg(&rc, VBBA_RF_ICMP);
    WB.context = 0; WB.get_capabilities = w_caps; WB.submit_tx = w_tx; WB.receive_rx = w_rx; WB.poll = w_poll; WB.reset = w_reset;
    G.use_dma = 1; G.stub = RIG_STUB_PATCHED; G.split3 = 0; G.interleave = 1;
    if (ios) rig_setup(&G, 0, MOCK_UDP, 0x7F000001u, &rc, VBBA_CHIP_AUTH, lossy ? &WB : 0);
    else rig_setup(&G, 1, 0, 0, &rc, VBBA_CHIP_AUTH, lossy ? &WB : 0);
    CHECK(rig_bringup(&G, 1) == 0);
    t0 = vbba_host_ms();
    while (!sdnet_configured(&G.S) && vbba_host_ms() - t0 < 15000) step(0);
    CHECK(sdnet_configured(&G.S));
    sdtcp_conn_init(&TC[0], trx[0], 4096, ttx[0], 4096);
    sdtcp_conn_init(&TC[1], trx[1], 4096, ttx[1], 4096);
    sdtcp_init(&T, &G.S, TC, 2, seed);
    sdtcp_set_mss(&T, 1400);
    sdtcp_set_window_cap(&T, 3840);
}

/* one transfer: up = bytes the guest sends (echo or sink), down = bytes the server sends (SRV_SOURCE / SRV_BOTH) */
static void transfer(const char *name, int mode, unsigned up, unsigned down, unsigned limit_ms)
{
    struct srv s;
    int h, ok_rx = 1, eof = 0;
    unsigned sent = 0, got = 0, t0, want_rx, closed_at = 0, k;
    unsigned char b[1500];
    srv_open(&s, mode, down);
    t0 = vbba_host_ms();
    h = sdtcp_connect(&T, 0x7F000001u, (sdtcp_u16)s.port);
    CHECK(h >= 0);
    want_rx = mode == SRV_ECHO ? up : down;
    while (vbba_host_ms() - t0 < limit_ms && h >= 0) {
        int n;
        step(&s);
        if (sdtcp_error(&T, h)) break;
        if (!sdtcp_established(&T, h) && sdtcp_state(&T, h) != SDTCP_CLOSE_WAIT) { if (sdtcp_state(&T, h) == SDTCP_CLOSED) break; continue; }
        if (sent < up) {
            unsigned m = up - sent; if (m > sizeof b) m = sizeof b;
            for (k = 0; k < m; k++) b[k] = pat(sent + k);
            n = sdtcp_send(&T, h, b, m);
            if (n > 0) sent += (unsigned)n;
        }
        n = sdtcp_recv(&T, h, b, sizeof b);
        if (n > 0) { for (k = 0; k < (unsigned)n; k++) if (b[k] != pat(got + k)) ok_rx = 0; got += (unsigned)n; }
        if (n == SDTCP_E_EOF) eof = 1;
        if (sent >= up && got >= want_rx && !closed_at && (mode != SRV_SOURCE || eof)) { sdtcp_close(&T, h); closed_at = vbba_host_ms(); }
        if (closed_at && sdtcp_state(&T, h) == SDTCP_CLOSED) break;
        if (closed_at && sdtcp_state(&T, h) == SDTCP_TIME_WAIT && s.closed_by_peer == 2) break;
    }
    { unsigned t1 = vbba_host_ms() + 1500; while (vbba_host_ms() < t1 && (G.router.tcp.c[0].st || G.router.tcp.c[1].st)) step(&s); }   /* let the router finish its side */
    printf("%-34s up %6u/%6u down %6u/%6u in %5u ms  srv got %6u ok %u  guest ok %d eof %d err %d  router: opened %u closed %u retrans %u ooo %u rst_tx %u lg %u  drops rx %u tx %u reord %u  conn %u/%u\n",
           name, sent, up, got, want_rx, vbba_host_ms() - t0, s.got, s.rx_ok, ok_rx, eof, h >= 0 ? sdtcp_error(&T, h) : h,
           G.router.tcp.st.opened, G.router.tcp.st.closed, G.router.tcp.st.retrans, G.router.tcp.st.ooo, G.router.tcp.st.rst_tx, G.router.tcp.st.abort_linger,
           F.dropped_rx, F.dropped_tx, F.reordered, G.router.tcp.c[0].st, G.router.tcp.c[1].st);
    if (G.tp == vbba_tp_ios_transport(&G.tpi)) printf("   (ios: last connect rc %d, last poll revents %d, opens %u connected %u fail %u, slots %u/%u %u/%u)\n", G.tpi.tst.last_conn_rc, G.tpi.tst.last_poll_rev, G.tpi.tst.opens, G.tpi.tst.connected, G.tpi.tst.open_fail, G.tpi.tc[0].state, G.tpi.tc[0].closing, G.tpi.tc[1].state, G.tpi.tc[1].closing);
    CHECK(sent == up);
    CHECK(got == want_rx);
    CHECK(ok_rx);
    CHECK(s.got == up);
    CHECK(s.rx_ok);
    if (mode == SRV_SOURCE) CHECK(eof);
    CHECK(h >= 0 && sdtcp_error(&T, h) == 0);                                 /* a clean close: no RST */
    CHECK(G.router.tcp.st.abort_linger == 0);
    CHECK(G.router.tcp.c[0].st == VTC_FREE && G.router.tcp.c[1].st == VTC_FREE);   /* the router freed its slot */
    if (h >= 0 && sdtcp_state(&T, h) != SDTCP_CLOSED) sdtcp_abort(&T, h);
    srv_close(&s);
}

static void refused(void)
{
    struct srv s; int h; unsigned t0, port;
    srv_open(&s, SRV_ECHO, 0); port = s.port; srv_close(&s);      /* a port nobody listens on */
    t0 = vbba_host_ms();
    h = sdtcp_connect(&T, 0x7F000001u, (sdtcp_u16)port);
    while (vbba_host_ms() - t0 < 5000 && !sdtcp_error(&T, h)) step(0);
    printf("%-34s error %d after %u ms (open_fail %u)\n", "refused connect", sdtcp_error(&T, h), vbba_host_ms() - t0, G.router.tcp.st.open_fail);
    CHECK(sdtcp_error(&T, h) == SDTCP_E_REFUSED);
    CHECK(vbba_host_ms() - t0 < 1500);
    CHECK(G.router.tcp.c[0].st == VTC_FREE && G.router.tcp.c[1].st == VTC_FREE);
}

static void denied(void)
{
    int h; unsigned t0 = vbba_host_ms();
    h = sdtcp_connect(&T, 0x08080808u, 443);                       /* not in the test allow-list */
    while (vbba_host_ms() - t0 < 3000 && !sdtcp_error(&T, h)) step(0);
    printf("%-34s error %d after %u ms (denied %u)\n", "denied destination", sdtcp_error(&T, h), vbba_host_ms() - t0, G.router.tcp.st.denied);
    CHECK(sdtcp_error(&T, h) == SDTCP_E_REFUSED);
}

/* guest aborts mid-transfer: the router must close the socket and free the slot */
static void guest_abort(void)
{
    struct srv s; int h; unsigned t0; unsigned char b[1000]; memset(b, 7, sizeof b);
    srv_open(&s, SRV_ECHO, 0);
    h = sdtcp_connect(&T, 0x7F000001u, (sdtcp_u16)s.port);
    t0 = vbba_host_ms();
    while (vbba_host_ms() - t0 < 2000 && !sdtcp_established(&T, h)) step(&s);
    sdtcp_send(&T, h, b, sizeof b);
    t0 = vbba_host_ms(); while (vbba_host_ms() - t0 < 300) step(&s);
    sdtcp_abort(&T, h);
    t0 = vbba_host_ms(); while (vbba_host_ms() - t0 < 1000 && (G.router.tcp.c[0].st || G.router.tcp.c[1].st || s.closed_by_peer != 2)) step(&s);
    printf("%-34s router conn %u/%u rst_rx %u, server saw close %d\n", "guest RST", G.router.tcp.c[0].st, G.router.tcp.c[1].st, G.router.tcp.st.rst_rx, s.closed_by_peer);
    CHECK(G.router.tcp.c[0].st == VTC_FREE && G.router.tcp.c[1].st == VTC_FREE);
    CHECK(s.closed_by_peer == 2);
    srv_close(&s);
}

/* the server end resets / dies mid-transfer: the guest must see an error, not hang */
static void tcp_reset_check(void)     /* established leftovers (a silent guest) are dropped by a backend reset (guest restart) */
{
    const struct vbba_frame_backend *b = vbba_router_backend(&G.router);
    b->reset(b->context);
    CHECK(G.router.tcp.c[0].st == VTC_FREE && G.router.tcp.c[1].st == VTC_FREE);
}
static void server_dies(void)
{
    struct srv s; int h; unsigned t0; struct linger lg = { 1, 0 }; unsigned char b[1500];
    srv_open(&s, SRV_SOURCE, 1u << 30);
    h = sdtcp_connect(&T, 0x7F000001u, (sdtcp_u16)s.port);
    t0 = vbba_host_ms();
    while (vbba_host_ms() - t0 < 600) { step(&s); (void)sdtcp_recv(&T, h, b, sizeof b); }
    setsockopt(s.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); close(s.fd); s.fd = -2; s.closed_by_peer = 2;   /* RST from the server */
    t0 = vbba_host_ms();
    while (vbba_host_ms() - t0 < 3000 && !sdtcp_error(&T, h) && sdtcp_state(&T, h) != SDTCP_CLOSED) { step(&s); (void)sdtcp_recv(&T, h, b, sizeof b); }
    printf("%-34s guest error %d state %d after %u ms, sock_err %u\n", "server RST mid-stream", sdtcp_error(&T, h), sdtcp_state(&T, h), vbba_host_ms() - t0, G.router.tcp.st.sock_err);
    /* the host transport sees the RST; through the (mock) IOS path a reset may also read as an EOF: then the guest gets a FIN and closes */
    CHECK(sdtcp_error(&T, h) == SDTCP_E_RESET || sdtcp_state(&T, h) == SDTCP_CLOSE_WAIT);
    if (sdtcp_state(&T, h) == SDTCP_CLOSE_WAIT) { sdtcp_close(&T, h); t0 = vbba_host_ms(); while (vbba_host_ms() - t0 < 3000 && (G.router.tcp.c[0].st || G.router.tcp.c[1].st)) step(&s); }
    CHECK(G.router.tcp.c[0].st == VTC_FREE && G.router.tcp.c[1].st == VTC_FREE);
    srv_close(&s);
}


/* hostile guest: random TCP segments (valid checksums, random flags / seq / ack / options / payload) straight into the router, with the
   guest-side receive drained and the router polled; afterwards every slot must come free (linger / retry limits) and nothing may crash */
static void p16(vbba_u8 *p, unsigned v) { p[0] = (vbba_u8)(v >> 8); p[1] = (vbba_u8)v; }
static void p32(vbba_u8 *p, unsigned v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }
static void fuzz(unsigned n)
{
    struct srv s;
    const struct vbba_frame_backend *b = vbba_router_backend(&G.router);
    unsigned i, k, rx = 0, seed = 777;
    vbba_u8 f[1600], o[1600];
    vbba_u16 ol;
    srv_open(&s, SRV_ECHO, 0);
    for (i = 0; i < n; i++) {
        vbba_u8 *ip = f + 14, *th = f + 34, ph[12];
        unsigned plen, hl, tl, sum, fl;
        seed = seed * 1103515245u + 12345u; fl = (seed >> 9) & 0x3f;
        memcpy(f, RIG_ROUTER_MAC, 6); memcpy(f + 6, RIG_MAC, 6); f[12] = 8; f[13] = 0;
        hl = 20 + ((seed >> 20) & 1u) * 4u;
        seed = seed * 1103515245u + 12345u; plen = (seed >> 12) % 1400u; if ((seed >> 5) & 1) plen = 0;
        tl = hl + plen;
        ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 20 + tl); p16(ip + 4, i); p16(ip + 6, 0); ip[8] = 64; ip[9] = 6; p16(ip + 10, 0);
        p32(ip + 12, G.router.leased); p32(ip + 16, 0x7F000001u);
        p16(ip + 10, vbba_csum(ip, 20, 0));
        seed = seed * 1103515245u + 12345u;
        p16(th, 40000 + (seed >> 28)); p16(th + 2, ((seed >> 4) & 3) ? s.port : 1 + ((seed >> 8) & 1023));
        seed = seed * 1103515245u + 12345u; p32(th + 4, (seed & 1) ? G.router.tcp.c[0].rcv_nxt : seed);
        seed = seed * 1103515245u + 12345u; p32(th + 8, (seed & 1) ? G.router.tcp.c[0].snd_nxt : seed);
        th[12] = (vbba_u8)((hl / 4) << 4); th[13] = (vbba_u8)fl; p16(th + 14, (seed >> 16)); p16(th + 16, 0); p16(th + 18, 0);
        if (hl == 24) { th[20] = (seed & 2) ? 2 : (vbba_u8)(seed >> 3); th[21] = (seed & 4) ? 4 : (vbba_u8)(seed >> 7); p16(th + 22, seed >> 11); }
        for (k = 0; k < plen; k++) { seed = seed * 1103515245u + 12345u; th[hl + k] = (vbba_u8)(seed >> 16); }
        memcpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 6; p16(ph + 10, tl);
        { unsigned a = 0, j; for (j = 0; j < 12; j += 2) a += (ph[j] << 8) | ph[j + 1]; sum = a; }
        p16(th + 16, vbba_csum(th, tl, sum));
        if ((i % 50) == 7) th[hl ? 5 : 0] ^= 1;                    /* some with a broken checksum */
        (void)b->submit_tx(b->context, f, (vbba_u16)(34 + tl));
        while (b->receive_rx(b->context, o, sizeof o, &ol) == VBBA_OK && ol) rx++;
        (void)b->poll(b->context);
        if ((i & 63) == 0) srv_poll(&s);
    }
    { unsigned t0 = vbba_host_ms(); while (vbba_host_ms() - t0 < 15000 && (G.router.tcp.c[0].st == VTC_SYNACK || G.router.tcp.c[1].st == VTC_SYNACK || G.router.tcp.c[0].st == VTC_CONNECTING || G.router.tcp.c[1].st == VTC_CONNECTING)) { (void)b->poll(b->context); while (b->receive_rx(b->context, o, sizeof o, &ol) == VBBA_OK && ol) rx++; srv_poll(&s); usleep(1000); } }
    printf("%-34s %u segments, %u frames out; syn %u opened %u fail %u denied %u noslot %u orphan %u bad %u rst_tx %u closed %u linger %u tries %u; slots %u/%u\n", "fuzz (hostile TCP)", n, rx,
           G.router.tcp.st.syn, G.router.tcp.st.opened, G.router.tcp.st.open_fail, G.router.tcp.st.denied, G.router.tcp.st.noslot, G.router.tcp.st.orphan_rst, G.router.tcp.st.bad, G.router.tcp.st.rst_tx,
           G.router.tcp.st.closed, G.router.tcp.st.abort_linger, G.router.tcp.st.abort_tries, G.router.tcp.c[0].st, G.router.tcp.c[1].st);
    CHECK(G.router.tcp.c[0].st != VTC_SYNACK && G.router.tcp.c[1].st != VTC_SYNACK && G.router.tcp.c[0].st != VTC_CONNECTING && G.router.tcp.c[1].st != VTC_CONNECTING);   /* half-open ones are gone */
    tcp_reset_check();
    srv_close(&s);
}

static void suite(int ios)
{
    printf("---- transport: %s\n", ios ? "IOS transport (vbba_tp_ios.c) over mock IOS with real host TCP" : "host stream sockets");
    memset(&F, 0, sizeof F);
    bring_up(0, ios);
    transfer("echo 64 KB", SRV_ECHO, 65536, 0, 20000);
    transfer("download 64 KB then server close", SRV_SOURCE, 0, 65536, 20000);
    transfer("both ways 64 KB each", SRV_BOTH, 65536, 65536, 20000);
    refused();
    denied();
    guest_abort();
    server_dies();
    if (ios) G.io.tcp_chunk = 97; else G.tph.s_chunk = 97;
    transfer("short socket calls (97 B) echo 16 KB", SRV_ECHO, 16384, 0, 20000);
    G.io.tcp_chunk = 0; G.tph.s_chunk = 0;
    if (ios) printf("   IOS transport: opens %u connected %u fail %u conn_poll %u rx %u B in %u, tx %u B in %u, short %u, neg %u, polls %u, eof %u, closes %u, shut %u, last connect rc %d; mock stream requests %u\n",
                    G.tpi.tst.opens, G.tpi.tst.connected, G.tpi.tst.open_fail, G.tpi.tst.conn_poll, G.tpi.tst.rx_bytes, G.tpi.tst.rx_n, G.tpi.tst.tx_bytes, G.tpi.tst.tx_n, G.tpi.tst.tx_short, G.tpi.tst.tx_neg,
                    G.tpi.tst.rx_poll, G.tpi.tst.eof, G.tpi.tst.closes, G.tpi.tst.shut, G.tpi.tst.last_conn_rc, G.io.tcp_ops);
    fuzz(ios ? 30000u : 60000u);
    rig_close(&G);
    memset(&F, 0, sizeof F); F.seed = 12345; F.loss_rx = 100; F.loss_tx = 50; F.reorder = 100;   /* 10 % loss + 10 % reorder toward the guest, 5 % loss from it */
    bring_up(1, ios);
    transfer("lossy: echo 64 KB", SRV_ECHO, 65536, 0, 60000);
    transfer("lossy: both ways 64 KB each", SRV_BOTH, 65536, 65536, 60000);
    transfer("lossy: download 64 KB + close", SRV_SOURCE, 0, 65536, 60000);
    rig_close(&G);
}

int main(int argc, char **argv)
{
    (void)w_poll_dummy; (void)argc; (void)argv;
    setvbuf(stdout, 0, _IONBF, 0);
    suite(0);
    suite(1);
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
