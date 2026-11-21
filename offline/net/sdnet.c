/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdnet.h"

/* Frame layout used for everything we transmit (IPv4 header has no options). */
#define ETH_HDR 14
#define IP_OFF ETH_HDR
#define IP_HDR 20
#define UDP_OFF (ETH_HDR + IP_HDR)
#define UDP_HDR 8
#define PAY_OFF (UDP_OFF + UDP_HDR)
#define ETH_MIN 60

#define ETYPE_IP 0x0800
#define ETYPE_ARP 0x0806
#define PROTO_ICMP 1
#define PROTO_UDP 17
#define PROTO_TCP 6

#define DHCP_CLIENT 68
#define DHCP_SERVER 67
#define DHCP_FIXED 240            /* BOOTP header + magic cookie */
#define DHCP_MIN_MSG 300

#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPACK 5
#define DHCPNAK 6

#define LEASE_CLAMP_S 2000000u    /* keep ms arithmetic inside 32 bits */

static const sdnet_u8 bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ---- byte helpers ---- */
static sdnet_u16 rd16(const sdnet_u8 *p)
{
    return (sdnet_u16)(((sdnet_u16)p[0] << 8) | p[1]);
}

static sdnet_u32 rd32(const sdnet_u8 *p)
{
    return ((sdnet_u32)p[0] << 24) | ((sdnet_u32)p[1] << 16) |
           ((sdnet_u32)p[2] << 8) | (sdnet_u32)p[3];
}

static void wr16(sdnet_u8 *p, sdnet_u16 v)
{
    p[0] = (sdnet_u8)(v >> 8);
    p[1] = (sdnet_u8)v;
}

static void wr32(sdnet_u8 *p, sdnet_u32 v)
{
    p[0] = (sdnet_u8)(v >> 24);
    p[1] = (sdnet_u8)(v >> 16);
    p[2] = (sdnet_u8)(v >> 8);
    p[3] = (sdnet_u8)v;
}

static void mcopy(sdnet_u8 *d, const sdnet_u8 *s, sdnet_u32 n)
{
    while (n--) {
        *d++ = *s++;
    }
}

static void mzero(sdnet_u8 *d, sdnet_u32 n)
{
    while (n--) {
        *d++ = 0;
    }
}

static int meq(const sdnet_u8 *a, const sdnet_u8 *b, sdnet_u32 n)
{
    while (n--) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return 1;
}

/* ---- checksum ---- */
sdnet_u32 sdnet_csum_add(sdnet_u32 sum, const sdnet_u8 *p, sdnet_u32 len)
{
    while (len > 1) {
        sum += ((sdnet_u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (sdnet_u32)p[0] << 8;
    }
    return sum;
}

sdnet_u16 sdnet_csum_fold(sdnet_u32 sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (sdnet_u16)(~sum & 0xffffu);
}

sdnet_u16 sdnet_checksum(const sdnet_u8 *p, sdnet_u32 len)
{
    return sdnet_csum_fold(sdnet_csum_add(0, p, len));
}

/* ---- time ---- */
static sdnet_u32 tick(struct sdnet *s)
{
    s->now = s->clock ? s->clock(s->clock_ctx) : 0;
    return s->now;
}

static int due(sdnet_u32 now, sdnet_u32 deadline)
{
    return (sdnet_s32)(now - deadline) >= 0;
}

static sdnet_u32 next_rand(struct sdnet *s)
{
    sdnet_u32 x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x ? x : 0x2545f491u;
    return s->rng;
}

/* ---- transmit helpers ---- */
static int send_frame(struct sdnet *s, sdnet_u32 len)
{
    if (len < ETH_MIN) {
        mzero(s->txbuf + len, ETH_MIN - len);
        len = ETH_MIN;
    }
    s->stats.tx_frames++;
    if (!s->nic.send || s->nic.send(s->nic.ctx, s->txbuf, (sdnet_u16)len) < 0) {
        return SDNET_E_NIC;
    }
    return SDNET_OK;
}

static void eth_hdr(struct sdnet *s, const sdnet_u8 *dst, sdnet_u16 type)
{
    mcopy(s->txbuf, dst, 6);
    mcopy(s->txbuf + 6, s->nic.mac, 6);
    wr16(s->txbuf + 12, type);
}

static void ip_hdr(struct sdnet *s, sdnet_u16 total, sdnet_u8 proto,
                   sdnet_u32 src, sdnet_u32 dst)
{
    sdnet_u8 *p = s->txbuf + IP_OFF;

    p[0] = 0x45;
    p[1] = 0;
    wr16(p + 2, total);
    wr16(p + 4, s->ip_id++);
    wr16(p + 6, 0);
    p[8] = 64;
    p[9] = proto;
    wr16(p + 10, 0);
    wr32(p + 12, src);
    wr32(p + 16, dst);
    wr16(p + 10, sdnet_checksum(p, IP_HDR));
}

static sdnet_u16 udp_checksum(sdnet_u32 src, sdnet_u32 dst, const sdnet_u8 *udp,
                              sdnet_u16 udplen)
{
    sdnet_u8 ph[12];
    sdnet_u32 sum;

    wr32(ph, src);
    wr32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = PROTO_UDP;
    wr16(ph + 10, udplen);
    sum = sdnet_csum_add(0, ph, 12);
    sum = sdnet_csum_add(sum, udp, udplen);
    return sdnet_csum_fold(sum);
}

/* Payload (plen bytes) is already at txbuf + PAY_OFF. */
static int finish_udp(struct sdnet *s, const sdnet_u8 *dmac, sdnet_u32 src,
                      sdnet_u32 dst, sdnet_u16 sport, sdnet_u16 dport,
                      sdnet_u16 plen)
{
    sdnet_u8 *u = s->txbuf + UDP_OFF;
    sdnet_u16 ck;

    eth_hdr(s, dmac, ETYPE_IP);
    ip_hdr(s, (sdnet_u16)(IP_HDR + UDP_HDR + plen), PROTO_UDP, src, dst);
    wr16(u, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (sdnet_u16)(UDP_HDR + plen));
    wr16(u + 6, 0);
    ck = udp_checksum(src, dst, u, (sdnet_u16)(UDP_HDR + plen));
    wr16(u + 6, ck ? ck : 0xffffu);
    return send_frame(s, (sdnet_u32)(PAY_OFF + plen));
}

/* ---- ARP ---- */
static struct sdnet_arp_entry *arp_find(struct sdnet *s, sdnet_u32 ip)
{
    int i;

    for (i = 0; i < SDNET_ARP_ENTRIES; ++i) {
        if (s->arp[i].state && s->arp[i].ip == ip) {
            return &s->arp[i];
        }
    }
    return 0;
}

static struct sdnet_arp_entry *arp_alloc(struct sdnet *s)
{
    int i;
    struct sdnet_arp_entry *best = &s->arp[0];
    sdnet_u32 best_age = 0;

    for (i = 0; i < SDNET_ARP_ENTRIES; ++i) {
        sdnet_u32 age;

        if (!s->arp[i].state) {
            return &s->arp[i];
        }
        age = s->now - s->arp[i].stamp;
        if (i == 0 || age > best_age) {
            best = &s->arp[i];
            best_age = age;
        }
    }
    return best;
}

static void arp_learn(struct sdnet *s, sdnet_u32 ip, const sdnet_u8 *mac)
{
    struct sdnet_arp_entry *e;

    if (ip == 0 || ip == 0xffffffffu || (mac[0] & 1)) {
        return;
    }
    e = arp_find(s, ip);
    if (!e) {
        e = arp_alloc(s);
    }
    if (e->state != 2) {
        e->last_use = s->now;
    }
    e->ip = ip;
    e->state = 2;
    e->stamp = s->now;
    e->tries = 0;
    mcopy(e->mac, mac, 6);
}

static int send_arp(struct sdnet *s, sdnet_u16 op, const sdnet_u8 *dmac,
                    const sdnet_u8 *tha, sdnet_u32 tpa)
{
    sdnet_u8 *p = s->txbuf + ETH_HDR;

    eth_hdr(s, dmac, ETYPE_ARP);
    wr16(p, 1);
    wr16(p + 2, ETYPE_IP);
    p[4] = 6;
    p[5] = 4;
    wr16(p + 6, op);
    mcopy(p + 8, s->nic.mac, 6);
    wr32(p + 14, s->ip);
    mcopy(p + 18, tha, 6);
    wr32(p + 24, tpa);
    return send_frame(s, ETH_HDR + 28);
}

int sdnet_arp_lookup(struct sdnet *s, sdnet_u32 ip, sdnet_u8 mac[6])
{
    struct sdnet_arp_entry *e;

    tick(s);
    e = arp_find(s, ip);
    if (!e || e->state != 2 || (s->now - e->stamp) >= SDNET_ARP_STALE_MS) {
        return 0;
    }
    e->last_use = s->now;
    mcopy(mac, e->mac, 6);
    return 1;
}

int sdnet_arp_resolve(struct sdnet *s, sdnet_u32 ip, sdnet_u8 mac[6])
{
    struct sdnet_arp_entry *e;
    static const sdnet_u8 zero[6] = { 0, 0, 0, 0, 0, 0 };

    if (sdnet_arp_lookup(s, ip, mac)) {
        return 1;
    }
    e = arp_find(s, ip);
    if (e && e->state == 1 && (s->now - e->stamp) < SDNET_ARP_PENDING_MS) {
        return 0;
    }
    if (!e) {
        e = arp_alloc(s);
    }
    e->ip = ip;
    e->state = 1;
    e->stamp = s->now;
    e->last_use = s->now;
    e->tries = 0;
    send_arp(s, 1, bcast_mac, zero, ip);
    return 0;
}

/* one-slot hold for the datagram that found its neighbour unresolved */
static void pend_flush(struct sdnet *s);

static void arp_age(struct sdnet *s)
{
    int i;
    static const sdnet_u8 zero[6] = { 0, 0, 0, 0, 0, 0 };

    for (i = 0; i < SDNET_ARP_ENTRIES; ++i) {
        struct sdnet_arp_entry *e = &s->arp[i];
        sdnet_u32 age = s->now - e->stamp;

        if (e->state == 2) {
            if (age >= SDNET_ARP_STALE_MS) {
                e->state = 0;
            } else if (age >= SDNET_ARP_REFRESH_MS && s->ip &&
                       (s->now - e->last_use) < SDNET_ARP_TTL_MS &&
                       (e->tries == 0 || (s->now - e->req_stamp) >= SDNET_ARP_PENDING_MS)) {
                /* refresh ahead: unicast to the cached MAC first (RFC 1122 2.3.2.1), broadcast after two silent tries */
                e->req_stamp = s->now;
                if (e->tries < 255) {
                    e->tries++;
                }
                s->stats.arp_refresh++;
                if (e->tries <= 2) {
                    send_arp(s, 1, e->mac, zero, e->ip);
                } else {
                    send_arp(s, 1, bcast_mac, zero, e->ip);
                }
            }
        } else if (e->state == 1 && age >= 3u * SDNET_ARP_PENDING_MS) {
            e->state = 0;
        }
    }
}

/* every frame we receive from a MAC proves that neighbour is alive: keep its entry fresh (the gateway forwards all internet traffic) */
static void arp_touch(struct sdnet *s, const sdnet_u8 *mac)
{
    int i;

    for (i = 0; i < SDNET_ARP_ENTRIES; ++i) {
        struct sdnet_arp_entry *e = &s->arp[i];

        if (e->state == 2 && meq(e->mac, mac, 6)) {
            e->stamp = s->now;
            e->tries = 0;
            s->stats.arp_touch++;
        }
    }
}

static void arp_rx(struct sdnet *s, const sdnet_u8 *p, sdnet_u32 len,
                   const sdnet_u8 *smac)
{
    sdnet_u16 op;
    sdnet_u32 spa, tpa;

    if (len < 28 || rd16(p) != 1 || rd16(p + 2) != ETYPE_IP || p[4] != 6 ||
        p[5] != 4) {
        s->stats.rx_bad++;
        return;
    }
    op = rd16(p + 6);
    spa = rd32(p + 14);
    tpa = rd32(p + 24);
    if (op != 1 && op != 2) {
        s->stats.rx_nomatch++;
        return;
    }
    if (s->ip == 0 || tpa != s->ip) {
        /* only refresh entries we already track (replies, and gratuitous requests with sender == target) */
        if ((op == 2 || spa == tpa) && arp_find(s, spa)) {
            arp_learn(s, spa, p + 8);
            pend_flush(s);
        }
        return;
    }
    arp_learn(s, spa, p + 8);
    if (op == 1) {
        send_arp(s, 2, smac, p + 8, spa);
    }
    pend_flush(s);
}

/* ---- sockets ---- */
int sdnet_bind(struct sdnet *s, sdnet_u16 port, sdnet_udp_cb cb, void *user)
{
    int i, free_i = -1;

    if (port == 0 || port == DHCP_CLIENT) {
        return SDNET_E_PARAM;
    }
    for (i = 0; i < SDNET_MAX_SOCKETS; ++i) {
        if (s->sock[i].used) {
            if (s->sock[i].port == port) {
                return SDNET_E_INUSE;
            }
        } else if (free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        return SDNET_E_NOSOCK;
    }
    s->sock[free_i].used = 1;
    s->sock[free_i].port = port;
    s->sock[free_i].cb = cb;
    s->sock[free_i].user = user;
    s->sock[free_i].head = 0;
    s->sock[free_i].count = 0;
    s->sock[free_i].dropped = 0;
    return free_i;
}

void sdnet_close(struct sdnet *s, int sock)
{
    if (sock >= 0 && sock < SDNET_MAX_SOCKETS) {
        s->sock[sock].used = 0;
        s->sock[sock].count = 0;
    }
}

int sdnet_pending(const struct sdnet *s, int sock)
{
    if (sock < 0 || sock >= SDNET_MAX_SOCKETS || !s->sock[sock].used) {
        return SDNET_E_NOSOCK;
    }
    return s->sock[sock].count;
}

int sdnet_recvfrom(struct sdnet *s, int sock, void *buf, sdnet_u16 max,
                   sdnet_u32 *src_ip, sdnet_u16 *src_port)
{
    struct sdnet_socket *k;
    struct sdnet_dgram *d;
    sdnet_u16 n;

    if (sock < 0 || sock >= SDNET_MAX_SOCKETS || !s->sock[sock].used) {
        return SDNET_E_NOSOCK;
    }
    k = &s->sock[sock];
    if (!k->count) {
        return SDNET_E_EMPTY;
    }
    d = &k->q[k->head];
    n = d->len < max ? d->len : max;
    if (buf) {
        mcopy((sdnet_u8 *)buf, d->data, n);
    }
    if (src_ip) {
        *src_ip = d->src_ip;
    }
    if (src_port) {
        *src_port = d->src_port;
    }
    k->head = (sdnet_u8)((k->head + 1) % SDNET_SOCK_SLOTS);
    k->count--;
    return n;
}

static int is_bcast(const struct sdnet *s, sdnet_u32 ip)
{
    return ip == 0xffffffffu ||
           (s->ip && s->mask != 0xffffffffu && ip == (s->ip | ~s->mask));
}

int sdnet_sendto(struct sdnet *s, int sock, sdnet_u32 dst_ip, sdnet_u16 dst_port,
                 const void *data, sdnet_u16 len)
{
    sdnet_u8 mac[6];
    sdnet_u32 hop;

    if (sock < 0 || sock >= SDNET_MAX_SOCKETS || !s->sock[sock].used ||
        (!data && len)) {
        return SDNET_E_NOSOCK;
    }
    if (len > SDNET_UDP_MAX) {
        return SDNET_E_TOOBIG;
    }
    if (!s->ip) {
        return SDNET_E_NOIP;
    }
    tick(s);
    if (is_bcast(s, dst_ip)) {
        mcopy(mac, bcast_mac, 6);
    } else if ((dst_ip >> 28) == 0xe) {
        mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
        mac[3] = (sdnet_u8)((dst_ip >> 16) & 0x7f);
        mac[4] = (sdnet_u8)(dst_ip >> 8);
        mac[5] = (sdnet_u8)dst_ip;
    } else {
        hop = ((dst_ip ^ s->ip) & s->mask) == 0 ? dst_ip : s->gw;
        if (!hop) {
            return SDNET_E_NOROUTE;
        }
        if (!sdnet_arp_resolve(s, hop, mac)) {
            /* hold this datagram (newest wins) and send it when the reply arrives; the caller still sees E_ARP */
            if (len > SDNET_PEND_MAX) {
                s->stats.pend_drop++;
            } else {
                if (s->pend.used) {
                    s->stats.pend_replaced++;
                }
                s->pend.used = 1;
                s->pend.src_port = s->sock[sock].port;
                s->pend.dst_port = dst_port;
                s->pend.dst_ip = dst_ip;
                s->pend.hop = hop;
                s->pend.len = len;
                s->pend.t = s->now;
                if (len) {
                    mcopy(s->pend.data, (const sdnet_u8 *)data, len);
                }
            }
            return SDNET_E_ARP;
        }
    }
    if (len) {
        mcopy(s->txbuf + PAY_OFF, (const sdnet_u8 *)data, len);
    }
    if (finish_udp(s, mac, s->ip, dst_ip, s->sock[sock].port, dst_port, len) <
        0) {
        return SDNET_E_NIC;
    }
    return len;
}

static void pend_flush(struct sdnet *s)
{
    sdnet_u8 mac[6];

    if (!s->pend.used) {
        return;
    }
    if (!s->ip) {
        s->pend.used = 0;
        s->stats.pend_drop++;
        return;
    }
    if (sdnet_arp_lookup(s, s->pend.hop, mac)) {
        s->pend.used = 0;
        mcopy(s->txbuf + PAY_OFF, s->pend.data, s->pend.len);
        if (finish_udp(s, mac, s->ip, s->pend.dst_ip, s->pend.src_port,
                       s->pend.dst_port, s->pend.len) < 0) {
            s->stats.pend_drop++;
        } else {
            s->stats.pend_sent++;
        }
    } else if ((s->now - s->pend.t) >= 3u * SDNET_ARP_PENDING_MS) {
        s->pend.used = 0;
        s->stats.pend_drop++;
    }
}

static void sock_deliver(struct sdnet *s, sdnet_u16 dport, sdnet_u32 src_ip,
                         sdnet_u16 sport, const sdnet_u8 *pay, sdnet_u16 plen)
{
    int i;

    for (i = 0; i < SDNET_MAX_SOCKETS; ++i) {
        struct sdnet_socket *k = &s->sock[i];
        struct sdnet_dgram *d;

        if (!k->used || k->port != dport) {
            continue;
        }
        if (plen > SDNET_SOCK_PAYLOAD || k->count >= SDNET_SOCK_SLOTS) {
            k->dropped++;
            s->stats.sock_drop++;
            return;
        }
        d = &k->q[(k->head + k->count) % SDNET_SOCK_SLOTS];
        d->src_ip = src_ip;
        d->src_port = sport;
        d->len = plen;
        mcopy(d->data, pay, plen);
        k->count++;
        if (k->cb) {
            k->cb(i, k->user);
        }
        return;
    }
    s->stats.rx_nomatch++;
}

/* ---- DHCP ---- */
static void dhcp_put_hdr(struct sdnet *s, sdnet_u8 *m, sdnet_u32 ciaddr,
                         sdnet_u16 flags)
{
    mzero(m, DHCP_MIN_MSG);
    m[0] = 1;
    m[1] = 1;
    m[2] = 6;
    wr32(m + 4, s->dhcp.xid);
    wr16(m + 10, flags);
    wr32(m + 12, ciaddr);
    mcopy(m + 28, s->nic.mac, 6);
    wr32(m + 236, 0x63825363u);
}

static sdnet_u32 dhcp_opt(sdnet_u8 *m, sdnet_u32 o, sdnet_u8 code, sdnet_u8 len,
                          const sdnet_u8 *val)
{
    m[o++] = code;
    m[o++] = len;
    mcopy(m + o, val, len);
    return o + len;
}

/* kind: DISCOVER, or REQUEST in one of the three request flavours. */
static void dhcp_send(struct sdnet *s, sdnet_u8 type)
{
    sdnet_u8 *m = s->txbuf + PAY_OFF;
    sdnet_u8 v[7];
    sdnet_u32 o = DHCP_FIXED, dst = 0xffffffffu, src = 0;
    const sdnet_u8 *dmac = bcast_mac;
    sdnet_u8 st = s->dhcp.state;
    int renewing = (st == SDNET_DHCP_RENEWING || st == SDNET_DHCP_REBINDING);

    dhcp_put_hdr(s, m, renewing ? s->ip : 0, renewing ? 0 : 0x8000u);
    v[0] = type;
    o = dhcp_opt(m, o, 53, 1, v);
    v[0] = 1;
    mcopy(v + 1, s->nic.mac, 6);
    o = dhcp_opt(m, o, 61, 7, v);
    if (type == DHCPREQUEST && st == SDNET_DHCP_REQUESTING) {
        wr32(v, s->dhcp.offered_ip);
        o = dhcp_opt(m, o, 50, 4, v);
        wr32(v, s->dhcp.server_ip);
        o = dhcp_opt(m, o, 54, 4, v);
    }
    v[0] = 1; v[1] = 3; v[2] = 6; v[3] = 51; v[4] = 58; v[5] = 59;
    o = dhcp_opt(m, o, 55, 6, v);
    m[o++] = 255;
    if (renewing) {
        src = s->ip;
        if (st == SDNET_DHCP_RENEWING && s->dhcp.have_server_mac) {
            dmac = s->dhcp.server_mac;
            dst = s->dhcp.server_ip;
        }
    }
    finish_udp(s, dmac, src, dst, DHCP_CLIENT, DHCP_SERVER, DHCP_MIN_MSG);
}

static sdnet_u32 dhcp_timeout(const struct sdnet *s)
{
    sdnet_u32 t = SDNET_DHCP_INIT_TIMEOUT_MS;
    int i;

    for (i = 1; i < s->dhcp.tries && t < SDNET_DHCP_MAX_TIMEOUT_MS; ++i) {
        t <<= 1;
    }
    return t > SDNET_DHCP_MAX_TIMEOUT_MS ? SDNET_DHCP_MAX_TIMEOUT_MS : t;
}

static void dhcp_transmit(struct sdnet *s, sdnet_u8 type)
{
    dhcp_send(s, type);
    if (s->dhcp.tries < 255) {
        s->dhcp.tries++;
    }
    s->dhcp.deadline = s->now + dhcp_timeout(s);
}

static void deconfigure(struct sdnet *s)
{
    int i;

    s->ip = s->mask = s->gw = s->dns = 0;
    for (i = 0; i < SDNET_ARP_ENTRIES; ++i) {
        s->arp[i].state = 0;
    }
}

/* new transaction id mixed with the clock: a sniffed xid of an earlier transaction must not validate a later reply */
static void dhcp_new_xid(struct sdnet *s)
{
    s->rng += s->now * 2654435761u + 0x7f4a7c15u;   /* added, not xored: dhcp_start already xors the same clock */
    if (!s->rng) {
        s->rng = 1;
    }
    s->dhcp.xid = next_rand(s);
}

static void dhcp_discover(struct sdnet *s)
{
    deconfigure(s);
    dhcp_new_xid(s);
    s->dhcp.tries = 0;
    s->dhcp.have_server_mac = 0;
    s->dhcp.state = SDNET_DHCP_SELECTING;
    dhcp_transmit(s, DHCPDISCOVER);
}

void sdnet_dhcp_start(struct sdnet *s)
{
    tick(s);
    s->rng ^= s->now;
    dhcp_discover(s);
}

int sdnet_dhcp_state(const struct sdnet *s)
{
    return s->dhcp.state;
}

int sdnet_configured(const struct sdnet *s)
{
    return s->ip != 0;
}

static void dhcp_tick(struct sdnet *s)
{
    sdnet_u32 el;

    switch (s->dhcp.state) {
    case SDNET_DHCP_SELECTING:
    case SDNET_DHCP_REQUESTING:
        if (due(s->now, s->dhcp.deadline)) {
            if (s->dhcp.tries >= SDNET_DHCP_MAX_TRIES) {
                s->dhcp.state = SDNET_DHCP_FAILED;
            } else {
                dhcp_transmit(s, s->dhcp.state == SDNET_DHCP_SELECTING
                                     ? DHCPDISCOVER : DHCPREQUEST);
            }
        }
        break;
    case SDNET_DHCP_BOUND:
    case SDNET_DHCP_RENEWING:
    case SDNET_DHCP_REBINDING:
        if (s->dhcp.infinite) {
            break;
        }
        el = (s->now - s->dhcp.t_bound) / 1000u;
        if (el >= s->dhcp.lease_s) {
            dhcp_discover(s);
        } else if (el >= s->dhcp.t2_s && s->dhcp.state != SDNET_DHCP_REBINDING) {
            s->dhcp.state = SDNET_DHCP_REBINDING;
            s->dhcp.tries = 0;
            dhcp_new_xid(s);
            dhcp_transmit(s, DHCPREQUEST);
            s->dhcp.deadline = s->now + SDNET_DHCP_RENEW_RETRY_MS;
        } else if (el >= s->dhcp.t1_s && s->dhcp.state == SDNET_DHCP_BOUND) {
            s->dhcp.state = SDNET_DHCP_RENEWING;
            s->dhcp.tries = 0;
            dhcp_new_xid(s);
            dhcp_transmit(s, DHCPREQUEST);
            s->dhcp.deadline = s->now + SDNET_DHCP_RENEW_RETRY_MS;
        } else if (s->dhcp.state != SDNET_DHCP_BOUND &&
                   due(s->now, s->dhcp.deadline)) {
            dhcp_transmit(s, DHCPREQUEST);
            s->dhcp.deadline = s->now + SDNET_DHCP_RENEW_RETRY_MS;
        }
        break;
    default:
        break;
    }
}

static void dhcp_rx(struct sdnet *s, const sdnet_u8 *p, sdnet_u32 len,
                    const sdnet_u8 *smac, sdnet_u32 sip)
{
    sdnet_u32 i, server = 0, lease = 0, t1 = 0, t2 = 0, mask = 0, router = 0;
    sdnet_u32 dns = 0, yi;
    sdnet_u8 type = 0, have_lease = 0, have_server = 0;
    sdnet_u8 st = s->dhcp.state;

    if (st == SDNET_DHCP_IDLE || st == SDNET_DHCP_FAILED ||
        st == SDNET_DHCP_BOUND) {
        s->stats.rx_nomatch++;
        return;
    }
    if (len < DHCP_FIXED || p[0] != 2 || p[1] != 1 || p[2] != 6 ||
        rd32(p + 4) != s->dhcp.xid || !meq(p + 28, s->nic.mac, 6) ||
        rd32(p + 236) != 0x63825363u) {
        s->stats.rx_bad++;
        return;
    }
    i = DHCP_FIXED;
    while (i < len) {
        sdnet_u8 c = p[i++], l;

        if (c == 255) {
            break;
        }
        if (c == 0) {
            continue;
        }
        if (i >= len) {
            break;
        }
        l = p[i++];
        if (l > len - i) {
            break;
        }
        switch (c) {
        case 53: if (l == 1) type = p[i]; break;
        case 54: if (l == 4) { server = rd32(p + i); have_server = 1; } break;
        case 51: if (l == 4) { lease = rd32(p + i); have_lease = 1; } break;
        case 58: if (l == 4) t1 = rd32(p + i); break;
        case 59: if (l == 4) t2 = rd32(p + i); break;
        case 1: if (l == 4) mask = rd32(p + i); break;
        case 3: if (l >= 4) router = rd32(p + i); break;
        case 6: if (l >= 4) dns = rd32(p + i); break;
        default: break;
        }
        i += l;
    }
    yi = rd32(p + 16);
    (void)sip;

    /* ACK/NAK must come from the server we chose (REQUESTING) or hold the lease with (RENEWING); a rebinding client
       accepts any server.  With the per-transaction xid this means an unsolicited NAK cannot wipe a working lease. */
    if ((type == DHCPACK || type == DHCPNAK) &&
        (st == SDNET_DHCP_REQUESTING || st == SDNET_DHCP_RENEWING) &&
        have_server && server != s->dhcp.server_ip) {
        s->stats.rx_nomatch++;
        return;
    }

    if (type == DHCPOFFER && st == SDNET_DHCP_SELECTING) {
        if (!yi || !have_server) {
            s->stats.rx_bad++;
            return;
        }
        s->dhcp.offered_ip = yi;
        s->dhcp.server_ip = server;
        s->dhcp.state = SDNET_DHCP_REQUESTING;
        s->dhcp.tries = 0;
        dhcp_transmit(s, DHCPREQUEST);
    } else if (type == DHCPACK && st != SDNET_DHCP_SELECTING) {
        if (!yi || !have_lease || lease == 0) {
            s->stats.rx_bad++;
            return;
        }
        if (s->ip != yi || s->gw != router) {      /* new address or gateway: old neighbours are void; announce the new one */
            for (i = 0; i < SDNET_ARP_ENTRIES; ++i) {
                s->arp[i].state = 0;
            }
            s->announce = 1;
        }
        s->ip = yi;
        s->mask = mask ? mask : 0xffffff00u;
        s->gw = router;
        s->dns = dns;
        if (have_server) {
            s->dhcp.server_ip = server;
        }
        mcopy(s->dhcp.server_mac, smac, 6);
        s->dhcp.have_server_mac = 1;
        s->dhcp.infinite = (lease == 0xffffffffu);
        if (lease > LEASE_CLAMP_S) {
            lease = LEASE_CLAMP_S;
        }
        if (t1 == 0 || t1 >= lease || t2 == 0 || t2 >= lease || t2 <= t1) {
            t1 = lease / 2;                 /* absent or inconsistent: RFC 2131 defaults */
            t2 = lease - lease / 8;
        }
        s->dhcp.lease_s = lease;
        s->dhcp.t1_s = t1;
        s->dhcp.t2_s = t2;
        s->dhcp.t_bound = s->now;
        s->dhcp.state = SDNET_DHCP_BOUND;
        s->dhcp.tries = 0;
    } else if (type == DHCPNAK && (st == SDNET_DHCP_REQUESTING ||
               st == SDNET_DHCP_RENEWING || st == SDNET_DHCP_REBINDING)) {
        dhcp_discover(s);
    } else {
        s->stats.rx_nomatch++;
    }
}

/* ---- IPv4 / ICMP receive ---- */
#if SDNET_ENABLE_ICMP
static void icmp_rx(struct sdnet *s, sdnet_u32 src, sdnet_u32 dst,
                    const sdnet_u8 *p, sdnet_u32 len, const sdnet_u8 *smac)
{
    sdnet_u8 *r;

    if (len < 8 || sdnet_checksum(p, len) != 0) {
        s->stats.rx_bad++;
        return;
    }
    if (p[0] != 8 || p[1] != 0 || dst != s->ip || len > 1500 - IP_HDR) {
        s->stats.rx_nomatch++;
        return;
    }
    r = s->txbuf + UDP_OFF;
    mcopy(r, p, len);
    r[0] = 0;
    wr16(r + 2, 0);
    wr16(r + 2, sdnet_checksum(r, len));
    eth_hdr(s, smac, ETYPE_IP);
    ip_hdr(s, (sdnet_u16)(IP_HDR + len), PROTO_ICMP, s->ip, src);
    send_frame(s, UDP_OFF + len);
}
#endif

static void ip_rx(struct sdnet *s, const sdnet_u8 *p, sdnet_u32 len,
                  const sdnet_u8 *smac)
{
    sdnet_u32 ihl, total, src, dst, plen;
    sdnet_u16 ff;
    const sdnet_u8 *l4;

    if (len < IP_HDR || (p[0] >> 4) != 4) {
        s->stats.rx_bad++;
        return;
    }
    ihl = (sdnet_u32)(p[0] & 15) * 4;
    total = rd16(p + 2);
    if (ihl < IP_HDR || ihl > len || total < ihl || total > len) {
        s->stats.rx_bad++;
        return;
    }
    if (sdnet_checksum(p, ihl) != 0) {
        s->stats.rx_bad++;
        return;
    }
    ff = rd16(p + 6);
    if ((ff & 0x3fff) != 0) {       /* MF set or fragment offset != 0 */
        s->stats.rx_frag++;
        return;
    }
    src = rd32(p + 12);
    dst = rd32(p + 16);
    l4 = p + ihl;
    plen = total - ihl;

    if (p[9] == PROTO_UDP) {
        sdnet_u32 ulen;
        sdnet_u16 sport, dport;

        if (plen < UDP_HDR) {
            s->stats.rx_bad++;
            return;
        }
        ulen = rd16(l4 + 4);
        if (ulen < UDP_HDR || ulen > plen) {
            s->stats.rx_bad++;
            return;
        }
        if (rd16(l4 + 6) != 0 &&
            udp_checksum(src, dst, l4, (sdnet_u16)ulen) != 0) {
            s->stats.rx_bad++;
            return;
        }
        sport = rd16(l4);
        dport = rd16(l4 + 2);
        if (dport == DHCP_CLIENT && sport == DHCP_SERVER &&
            (s->ip == 0 || dst == s->ip || is_bcast(s, dst))) {
            dhcp_rx(s, l4 + UDP_HDR, ulen - UDP_HDR, smac, src);
            return;
        }
        if (s->ip == 0 || !(dst == s->ip || is_bcast(s, dst))) {
            s->stats.rx_nomatch++;
            return;
        }
        sock_deliver(s, dport, src, sport, l4 + UDP_HDR,
                     (sdnet_u16)(ulen - UDP_HDR));
        return;
    }
#if SDNET_ENABLE_TCP
    if (p[9] == PROTO_TCP && s->ip && dst == s->ip && s->tcp_rx) {
        s->tcp_rx(s->tcp, src, dst, l4, plen);
        return;
    }
#endif
#if SDNET_ENABLE_ICMP
    if (p[9] == PROTO_ICMP && s->ip) {
        icmp_rx(s, src, dst, l4, plen, smac);
        return;
    }
#endif
    s->stats.rx_nomatch++;
}

static void rx_frame(struct sdnet *s, const sdnet_u8 *f, sdnet_u32 len)
{
    sdnet_u16 et;

    s->stats.rx_frames++;
    if (len < ETH_HDR) {
        s->stats.rx_bad++;
        return;
    }
    if (!meq(f, s->nic.mac, 6) && !meq(f, bcast_mac, 6) && !(f[0] & 1)) {
        s->stats.rx_nomatch++;
        return;
    }
    arp_touch(s, f + 6);
    et = rd16(f + 12);
    if (et == ETYPE_ARP) {
        arp_rx(s, f + ETH_HDR, len - ETH_HDR, f + 6);
    } else if (et == ETYPE_IP) {
        ip_rx(s, f + ETH_HDR, len - ETH_HDR, f + 6);
    } else {
        s->stats.rx_nomatch++;
    }
}

/* ---- setup / poll ---- */
void sdnet_init(struct sdnet *s, const struct sdnet_nic *nic,
                sdnet_clock_fn clock, void *clock_ctx)
{
    sdnet_u8 *b = (sdnet_u8 *)s;
    sdnet_u32 i;

    for (i = 0; i < sizeof(*s); ++i) {
        b[i] = 0;
    }
    s->nic = *nic;
    s->clock = clock;
    s->clock_ctx = clock_ctx;
    s->rng = 0x9e3779b9u ^ rd32(nic->mac) ^ ((sdnet_u32)nic->mac[4] << 8 |
             nic->mac[5]);
    if (!s->rng) {
        s->rng = 1;
    }
    tick(s);
}

void sdnet_set_static(struct sdnet *s, sdnet_u32 ip, sdnet_u32 mask,
                      sdnet_u32 gw)
{
    s->dhcp.state = SDNET_DHCP_IDLE;
    s->ip = ip;
    s->mask = mask;
    s->gw = gw;
    s->announce = 1;
}

void sdnet_poll(struct sdnet *s)
{
    int n, r;

    tick(s);
    for (n = 0; n < SDNET_RX_BUDGET; ++n) {
        r = s->nic.recv ? s->nic.recv(s->nic.ctx, s->rxbuf, SDNET_BUF_SIZE) : 0;
        if (r <= 0) {
            break;
        }
        if (r > SDNET_BUF_SIZE) {   /* NIC contract violation: never trust it */
            s->stats.rx_bad++;
            continue;
        }
        rx_frame(s, s->rxbuf, (sdnet_u32)r);
    }
    arp_age(s);
    pend_flush(s);
    if (s->announce && s->ip) {                     /* gratuitous ARP: neighbours learn (or refresh) our address at once */
        static const sdnet_u8 zero[6] = { 0, 0, 0, 0, 0, 0 };
        s->announce = 0;
        send_arp(s, 1, bcast_mac, zero, s->ip);
    }
    dhcp_tick(s);
#if SDNET_ENABLE_TCP
    if (s->tcp_poll) {
        s->tcp_poll(s->tcp);
    }
#endif
}

#if SDNET_ENABLE_TCP
int sdnet_tx_ip(struct sdnet *s, sdnet_u8 proto, sdnet_u32 dst_ip, sdnet_u16 plen)
{
    sdnet_u8 mac[6];
    sdnet_u32 hop;

    if (!s->ip) {
        return SDNET_E_NOIP;
    }
    if (plen > SDNET_BUF_SIZE - SDNET_L4_OFF) {
        return SDNET_E_TOOBIG;
    }
    hop = ((dst_ip ^ s->ip) & s->mask) == 0 ? dst_ip : s->gw;
    if (!hop) {
        return SDNET_E_NOROUTE;
    }
    if (!sdnet_arp_resolve(s, hop, mac)) {
        return SDNET_E_ARP;
    }
    eth_hdr(s, mac, ETYPE_IP);
    ip_hdr(s, (sdnet_u16)(IP_HDR + plen), proto, s->ip, dst_ip);
    return send_frame(s, (sdnet_u32)(SDNET_L4_OFF + plen));
}
#endif
