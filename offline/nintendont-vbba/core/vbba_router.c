/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_router: frames <-> datagrams, see vbba_router.h.  Written from RFC 826 (ARP), 791 (IPv4), 768 (UDP), 792 (ICMP),
   2131/2132 (DHCP).  Project-owned C99, no libc, no heap; explicit big-endian byte access (endian-neutral). */
#include "vbba_router.h"

#define ETH_ARP 0x0806u
#define ETH_IP 0x0800u
#define DHCP_COOKIE 0x63825363u

static void zero(vbba_u8 *p, vbba_u32 n) { while (n--) *p++ = 0; }
static void cpy(vbba_u8 *d, const vbba_u8 *s, vbba_u32 n) { while (n--) *d++ = *s++; }
static vbba_u32 g16(const vbba_u8 *p) { return ((vbba_u32)p[0] << 8) | p[1]; }
static vbba_u32 g32(const vbba_u8 *p) { return ((vbba_u32)p[0] << 24) | ((vbba_u32)p[1] << 16) | ((vbba_u32)p[2] << 8) | p[3]; }
static void p16(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 8); p[1] = (vbba_u8)v; }
static void p32(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }

static vbba_u32 csum_add(vbba_u32 s, const vbba_u8 *p, vbba_u32 n)
{
    vbba_u32 i;
    for (i = 0; i + 1 < n; i += 2) s += ((vbba_u32)p[i] << 8) | p[i + 1];
    if (n & 1u) s += (vbba_u32)p[n - 1] << 8;
    return s;
}
static vbba_u16 csum_fold(vbba_u32 s) { while (s >> 16) s = (s & 0xffffu) + (s >> 16); return (vbba_u16)~s; }
vbba_u16 vbba_csum(const vbba_u8 *p, vbba_u32 n, vbba_u32 sum) { return csum_fold(csum_add(sum, p, n)); }

#if VBBA_TCP
static void tcp_input(struct vbba_router *r, const vbba_u8 *f, vbba_u32 ihl, vbba_u32 tot, vbba_u32 t);
static vbba_u32 tcp_output(struct vbba_router *r, vbba_u8 *f, vbba_u16 cap, vbba_u32 t);
static void tcp_poll(struct vbba_router *r, vbba_u32 t);
static void tcp_reset_all(struct vbba_router *r);
#endif

static vbba_u32 now(struct vbba_router *r) { return r->tp && r->tp->now_ms ? r->tp->now_ms(r->tp->ctx) : 0; }

/* per-second budgets (a window that restarts every 1000 ms) */
static void window(struct vbba_router *r, vbba_u32 t)
{
    if ((vbba_u32)(t - r->win_ms) >= 1000u) { r->win_ms = t; r->win_tx = 0; r->win_local = 0; }
}

/* ---- lease view ---- */
static void refresh(struct vbba_router *r)
{
    struct vbba_tp_status s;
    if (!(r->cfg.flags & VBBA_RF_MIRROR)) {
        r->lease_ip = r->cfg.guest_ip;
        r->mask = r->cfg.mask;
        r->router_ip = r->cfg.gw;
        return;
    }
    s.up = 0; s.ip = s.mask = s.gw = 0;
    if (r->tp && r->tp->status) r->tp->status(r->tp->ctx, &s);
    if (!s.up || !s.ip) { r->lease_ip = 0; return; }
    r->lease_ip = s.ip;
    r->mask = s.mask ? s.mask : (r->cfg.mask ? r->cfg.mask : 0xffffff00u);
    r->router_ip = (s.gw && s.gw != s.ip) ? s.gw : (s.ip & r->mask) | 1u;
    if (r->router_ip == s.ip) r->router_ip = (s.ip & r->mask) | 2u;
}

int vbba_router_allowed(const struct vbba_router *r, vbba_u32 ip, vbba_u16 port)
{
    vbba_u32 i;
    for (i = 0; i < r->cfg.nallow && i < VBBA_ALLOW_MAX; i++) {
        const struct vbba_allow_rule *a = &r->cfg.allow[i];
        if ((ip & a->mask) == (a->ip & a->mask) && port >= a->port_lo && port <= a->port_hi) return 1;
    }
    return 0;
}
static int allowed(struct vbba_router *r, vbba_u32 ip, vbba_u16 port, int out)
{
    if (!vbba_router_allowed(r, ip, port)) return 0;
    if (r->tp->allow && !r->tp->allow(r->tp->ctx, ip, port, out)) return 0;
    return 1;
}

/* ---- router-generated frames ---- */
static vbba_u8 *lq_slot(struct vbba_router *r, vbba_u32 t)
{
    window(r, t);
    if (r->win_local >= r->cfg.arp_per_s) { r->st.local_rate++; return 0; }
    if (r->lq_cnt >= VBBA_LOCALQ) { r->st.localq_drop++; return 0; }
    r->win_local++;
    return r->lq[(r->lq_head + r->lq_cnt) % VBBA_LOCALQ];
}
static void lq_commit(struct vbba_router *r, vbba_u32 len)
{
    r->lq_len[(r->lq_head + r->lq_cnt) % VBBA_LOCALQ] = (vbba_u16)len;
    r->lq_cnt++;
}

/* IPv4 + UDP headers around a payload already at f + 42; returns the frame length (padded to 60) */
static vbba_u32 build_udp(struct vbba_router *r, vbba_u8 *f, const vbba_u8 *dmac, vbba_u32 sip, vbba_u32 dip,
                          vbba_u32 sp, vbba_u32 dp, vbba_u32 plen)
{
    vbba_u8 *ip = f + 14, *u = f + 34, ph[12];
    vbba_u32 s, n;
    cpy(f, dmac, 6); cpy(f + 6, r->cfg.router_mac, 6); p16(f + 12, ETH_IP);
    ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 28 + plen); p16(ip + 4, ++r->ip_id); p16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17; p16(ip + 10, 0); p32(ip + 12, sip); p32(ip + 16, dip);
    p16(ip + 10, vbba_csum(ip, 20, 0));
    p16(u, sp); p16(u + 2, dp); p16(u + 4, 8 + plen); p16(u + 6, 0);
    cpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 17; p16(ph + 10, 8 + plen);
    s = csum_add(0, ph, 12);
    s = csum_fold(csum_add(s, u, 8 + plen));
    p16(u + 6, s ? s : 0xffffu);
    n = 42 + plen;
    while (n < 60) f[n++] = 0;
    return n;
}

/* ---- ARP ---- */
static void on_arp(struct vbba_router *r, const vbba_u8 *f, vbba_u32 len, vbba_u32 t)
{
    vbba_u32 spa, tpa;
    vbba_u8 *o;
    if (len < 42 || g16(f + 14) != 1 || g16(f + 16) != ETH_IP || f[18] != 6 || f[19] != 4) { r->st.tx_bad++; return; }
    if (g16(f + 20) != 1) { r->st.arp_ignored++; return; }          /* replies: nothing to learn, all peers are us */
    r->st.arp_req++;
    spa = g32(f + 28); tpa = g32(f + 38);
    if (spa == 0 || spa == tpa || tpa == 0 || (r->leased && tpa == r->leased) || (tpa >> 24) == 0xE0u || tpa == 0xffffffffu) {
        r->st.arp_ignored++;                                         /* probe, announcement, our own address, multicast */
        return;
    }
    if (!(o = lq_slot(r, t))) return;
    zero(o, 60);
    cpy(o, f + 22, 6); cpy(o + 6, r->cfg.router_mac, 6); p16(o + 12, ETH_ARP);
    p16(o + 14, 1); p16(o + 16, ETH_IP); o[18] = 6; o[19] = 4; p16(o + 20, 2);
    cpy(o + 22, r->cfg.router_mac, 6); p32(o + 28, tpa); cpy(o + 32, f + 22, 6); cpy(o + 38, f + 28, 4);
    lq_commit(r, 60);
    r->st.arp_replies++;
}

/* ---- DHCP server (one lease) ---- */
static void dhcp_reply(struct vbba_router *r, const vbba_u8 *req, int type, vbba_u32 t, int unicast)
{
    vbba_u8 *o, *m;
    vbba_u32 k = 240, lease = r->cfg.lease_s ? r->cfg.lease_s : 86400u;
    static const vbba_u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (!(o = lq_slot(r, t))) return;
    m = o + 42;
    zero(m, 300);
    m[0] = 2; m[1] = 1; m[2] = 6;
    cpy(m + 4, req + 4, 4);                                          /* xid */
    cpy(m + 10, req + 10, 2);                                        /* flags */
    if (type != 6) p32(m + 16, r->lease_ip);                         /* yiaddr */
    p32(m + 20, r->router_ip);                                       /* siaddr */
    cpy(m + 28, req + 28, 16);                                       /* chaddr */
    p32(m + 236, DHCP_COOKIE);
    m[k++] = 53; m[k++] = 1; m[k++] = (vbba_u8)type;
    m[k++] = 54; m[k++] = 4; p32(m + k, r->router_ip); k += 4;
    if (type != 6) {
        m[k++] = 51; m[k++] = 4; p32(m + k, lease); k += 4;
        m[k++] = 1; m[k++] = 4; p32(m + k, r->mask); k += 4;
        m[k++] = 3; m[k++] = 4; p32(m + k, r->router_ip); k += 4;
        if (r->cfg.dns) { m[k++] = 6; m[k++] = 4; p32(m + k, r->cfg.dns); k += 4; }     /* resolver: the guest resolves the server name through it only */
    }
    m[k++] = 255;
    lq_commit(r, build_udp(r, o, unicast ? r->guest_mac : bc, r->router_ip, unicast ? r->lease_ip : 0xffffffffu, 67, 68, 300));
    if (type == 2) r->st.dhcp_offer++; else if (type == 5) r->st.dhcp_ack++; else r->st.dhcp_nak++;
}

static void on_dhcp(struct vbba_router *r, const vbba_u8 *m, vbba_u32 n, vbba_u32 t)
{
    vbba_u32 i = 240, type = 0, req_ip = 0, sid = 0, ciaddr;
    if (n < 240 || m[0] != 1 || m[1] != 1 || m[2] != 6 || g32(m + 236) != DHCP_COOKIE) { r->st.dhcp_bad++; return; }
    while (i < n) {
        vbba_u32 c = m[i++], l;
        if (c == 255) break;
        if (c == 0) continue;
        if (i >= n) break;
        l = m[i++];
        if (i + l > n) { r->st.dhcp_bad++; return; }
        if (c == 53 && l >= 1) type = m[i];
        if (c == 50 && l >= 4) req_ip = g32(m + i);
        if (c == 54 && l >= 4) sid = g32(m + i);
        i += l;
    }
    ciaddr = g32(m + 12);
    refresh(r);
    if (type == 1) {
        r->st.dhcp_disc++;
        if (!r->lease_ip) {
            r->st.dhcp_wait_ip++;
            if (n <= VBBA_DHCP_PEND) { cpy(r->dh_msg, m, n); r->dh_len = (vbba_u16)n; r->dh_t = t; r->dh_pend = 1; }   /* answered by be_poll once the address exists */
            return;
        }
        dhcp_reply(r, m, 2, t, 0);
    } else if (type == 3) {
        r->st.dhcp_req++;
        if (sid && sid != r->router_ip) return;                      /* addressed to another server */
        if (!r->lease_ip) { r->st.dhcp_wait_ip++; return; }
        if (!req_ip) req_ip = ciaddr;
        if (req_ip == r->lease_ip) { r->leased = r->lease_ip; dhcp_reply(r, m, 5, t, ciaddr != 0); }
        else dhcp_reply(r, m, 6, t, 0);
    } else {
        r->st.dhcp_bad++;                                            /* RELEASE / DECLINE / INFORM: nothing to do */
    }
}

/* ---- port maps and conntrack ---- */
static void map_drop(struct vbba_router *r, int i)
{
    int h;
    if (r->tp->close) r->tp->close(r->tp->ctx, r->map[i].h);
    r->map[i].used = 0;
    for (h = 0; h < VBBA_CT; h++) if (r->ct[h].map == i) r->ct[h].used = 0;
#if VBBA_HOLD
    if (r->hold_used && r->hold_map == i) { r->hold_used = 0; r->st.tx_hold_drop++; }
#endif
}

static int map_for(struct vbba_router *r, vbba_u16 gport, vbba_u32 t)
{
    int i, lru = -1, h, n = 0;
    for (i = 0; i < VBBA_MAPS; i++) if (r->map[i].used && r->map[i].gport == gport) { r->map[i].last = t; return i; }
    for (i = 0; i < VBBA_MAPS; i++) if (!r->map[i].used) break;
    if (i == VBBA_MAPS) {                                            /* evict the least recently used, if it has been quiet */
        for (i = 0; i < VBBA_MAPS; i++) if (lru < 0 || (vbba_s32)(r->map[i].last - r->map[lru].last) < 0) lru = i;
        if ((vbba_u32)(t - r->map[lru].last) < VBBA_MAP_KEEP_MS) { r->st.maps_full++; return -1; }
        r->st.evict_port = r->map[lru].gport; r->st.evict_idle_ms = t - r->map[lru].last;
        map_drop(r, lru);
        r->st.maps_evicted++;
        i = lru;
    }
    h = r->tp->open(r->tp->ctx, gport);
    if (h < 0) { r->st.maps_failed++; return -1; }                  /* the slot stays free: the port's next datagram tries again */
    r->map[i].used = 1; r->map[i].gport = gport; r->map[i].h = h; r->map[i].last = t;
    r->st.maps_opened++;
    for (h = 0; h < VBBA_MAPS; h++) n += r->map[h].used;
    if ((vbba_u32)n > r->st.maps_peak) r->st.maps_peak = (vbba_u32)n;
    return i;
}

static void ct_note(struct vbba_router *r, int map, vbba_u32 ip, vbba_u16 port, vbba_u32 t)
{
    int i, old = 0;
    for (i = 0; i < VBBA_CT; i++)
        if (r->ct[i].used && r->ct[i].map == map && r->ct[i].ip == ip && r->ct[i].port == port) { r->ct[i].last = t; return; }
    for (i = 0; i < VBBA_CT; i++) {
        if (!r->ct[i].used) { old = i; break; }
        if ((vbba_s32)(r->ct[i].last - r->ct[old].last) < 0) old = i;
    }
    r->ct[old].used = 1; r->ct[old].map = (vbba_u8)map; r->ct[old].ip = ip; r->ct[old].port = port; r->ct[old].last = t;
}

static int ct_ok(struct vbba_router *r, int map, vbba_u32 ip, vbba_u16 port, vbba_u32 t)
{
    int i;
    for (i = 0; i < VBBA_CT; i++) {
        struct vbba_ct *c = &r->ct[i];
        if (!c->used || c->map != map || c->ip != ip) continue;
        if (c->port != port && !(r->cfg.flags & VBBA_RF_CT_ANY_PORT)) continue;
        if ((vbba_u32)(t - c->last) > r->cfg.ct_ms) { c->used = 0; continue; }
        return 1;
    }
    return 0;
}

/* ---- guest -> network ---- */
static void on_udp(struct vbba_router *r, const vbba_u8 *f, vbba_u32 ihl, vbba_u32 tot, vbba_u32 t)
{
    const vbba_u8 *ip = f + 14, *u = ip + ihl;
    vbba_u32 ulen, sip = g32(ip + 12), dip = g32(ip + 16), sp, dp, s;
    vbba_u8 ph[12];
    int m, rc;
    if (tot < ihl + 8) { r->st.tx_bad++; return; }
    ulen = g16(u + 4);
    if (ulen < 8 || ulen > tot - ihl) { r->st.tx_bad++; return; }
    if (g16(u + 6)) {
        cpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 17; p16(ph + 10, ulen);
        s = csum_add(0, ph, 12);
        if (csum_fold(csum_add(s, u, ulen)) != 0) { r->st.tx_bad_csum++; return; }
    }
    sp = g16(u); dp = g16(u + 2);
    if (dp == 53) r->st.tx_dns++;
    if (dp == 67 && sp == 68) {                                      /* DHCP to us (broadcast or unicast renew) */
        if (dip == 0xffffffffu || (r->router_ip && dip == r->router_ip)) on_dhcp(r, u + 8, ulen - 8, t);
        else r->st.tx_denied++;
        return;
    }
    refresh(r);
    if (!r->leased || sip != r->leased || (r->cfg.flags & VBBA_RF_MIRROR ? sip != r->lease_ip : 0)) { r->st.tx_spoof++; return; }
    if (dip == 0xffffffffu || (dip >> 28) == 0xEu || (dip | r->mask) == 0xffffffffu) { r->st.tx_bcast++; return; }
    if (dip == r->leased || dip == r->router_ip || sp == 0 || dp == 0 || !allowed(r, dip, (vbba_u16)dp, 1)) { r->st.tx_denied++; return; }
    window(r, t);
    if (r->win_tx >= r->cfg.tx_per_s) { r->st.tx_rate++; return; }
    r->win_tx++;
    if ((m = map_for(r, (vbba_u16)sp, t)) < 0) { r->st.tx_nomap++; return; }
    if (r->tp->state) {
        int s2 = r->tp->state(r->tp->ctx, r->map[m].h);
        int queued_by_tp = s2 == VBBA_TP_OPENING && r->tp->queues_opening;           /* the transport keeps it (FIFO) until the socket is open */
        if (queued_by_tp) r->st.tx_opening++;
        else if (s2 == VBBA_TP_OPENING) {
            r->st.tx_opening++;
#if VBBA_HOLD
            if (!r->hold_used || r->hold_map == m) {                 /* newest datagram wins; sent once the socket is up */
                if (r->hold_used) r->st.tx_hold_drop++;
                cpy(r->hold, u + 8, ulen - 8);
                r->hold_used = 1; r->hold_map = (vbba_u8)m; r->hold_ip = dip; r->hold_port = (vbba_u16)dp;
                r->hold_len = (vbba_u16)(ulen - 8); r->hold_t = t;
                ct_note(r, m, dip, (vbba_u16)dp, t);
                r->st.tx_held++;
            } else r->st.tx_hold_busy++;
#endif
            return;
        }
        if (s2 != VBBA_TP_OPEN && !queued_by_tp) {
            map_drop(r, m); r->st.maps_failed++; r->st.tx_tp_err++;  /* the port's next datagram opens a new socket */
            return;
        }
    }
    ct_note(r, m, dip, (vbba_u16)dp, t);
    rc = r->tp->send(r->tp->ctx, r->map[m].h, dip, (vbba_u16)dp, u + 8, (vbba_u16)(ulen - 8));
    if (rc == VBBA_OK) r->st.tx_udp++;
    else if (rc == VBBA_WOULD_BLOCK) r->st.tx_tp_busy++;
    else r->st.tx_tp_err++;
}

static void on_icmp(struct vbba_router *r, const vbba_u8 *f, vbba_u32 ihl, vbba_u32 tot, vbba_u32 t)
{
    const vbba_u8 *ip = f + 14, *ic = ip + ihl;
    vbba_u32 n = tot - ihl;
    vbba_u8 *o, *oi;
    if (!(r->cfg.flags & VBBA_RF_ICMP) || n < 8 || ic[0] != 8 || g32(ip + 16) != r->router_ip || !r->leased ||
        g32(ip + 12) != r->leased || vbba_csum(ic, n, 0) != 0 || 34 + n > VBBA_LOCAL_FRAME) { r->st.icmp_drop++; return; }
    if (!(o = lq_slot(r, t))) return;
    cpy(o, f + 6, 6); cpy(o + 6, r->cfg.router_mac, 6); p16(o + 12, ETH_IP);
    oi = o + 14;
    oi[0] = 0x45; oi[1] = 0; p16(oi + 2, 20 + n); p16(oi + 4, ++r->ip_id); p16(oi + 6, 0); oi[8] = 64; oi[9] = 1;
    p16(oi + 10, 0); p32(oi + 12, r->router_ip); p32(oi + 16, r->leased); p16(oi + 10, vbba_csum(oi, 20, 0));
    cpy(o + 34, ic, n);
    o[34] = 0; o[36] = 0; o[37] = 0;
    p16(o + 36, vbba_csum(o + 34, n, 0));
    n += 34;
    while (n < 60) o[n++] = 0;
    lq_commit(r, n);
    r->st.icmp_replies++;
}

/* (diagnostic only): classify the frame and remember the first distinct headers; changes nothing about how the frame is handled */
static void note_tx(struct vbba_router *r, const vbba_u8 *f, vbba_u16 len)
{
    struct vbba_txhdr h;
    vbba_u32 i, ihl;
    h.eth = 0; h.sport = h.dport = 0; h.len = len; h.proto = 0; h.op = 0; h.dip = 0;
    if (len >= 14) h.eth = (vbba_u16)g16(f + 12);
    if (h.eth == ETH_ARP) { r->st.tx_f_arp++; if (len >= 42) { h.op = (vbba_u8)g16(f + 20); h.dip = g32(f + 38); } }
    else if (h.eth == ETH_IP && len >= 34) {
        const vbba_u8 *ip = f + 14;
        h.proto = ip[9]; h.dip = g32(ip + 16); ihl = (ip[0] & 15u) * 4u;
        if ((h.proto == 17 || h.proto == 6) && ihl >= 20 && len >= 14u + ihl + 4u) { h.sport = (vbba_u16)g16(ip + ihl); h.dport = (vbba_u16)g16(ip + ihl + 2); }
        if (h.proto == 17) r->st.tx_f_udp++; else r->st.tx_f_ip_other++;
    } else r->st.tx_f_nonip++;
    h.n = 1;
    for (i = 0; i < r->txhdr_n; i++) { struct vbba_txhdr *o = &r->txhdr[i]; if (o->eth == h.eth && o->proto == h.proto && o->dip == h.dip && o->dport == h.dport) { o->n++; return; } }
    if (r->txhdr_n < VBBA_TXHDR) r->txhdr[r->txhdr_n++] = h;
}

static int be_tx(void *ctx, const vbba_u8 *f, vbba_u16 len)
{
    struct vbba_router *r = (struct vbba_router *)ctx;
    vbba_u32 t = now(r), type, ihl, tot, i;
    const vbba_u8 *ip;
    r->st.tx_frames++;
    note_tx(r, f, len);
    if (len < 14 || len > 1518) { r->st.tx_bad++; return VBBA_OK; }
    if (f[6] & 1u) { r->st.tx_bad++; return VBBA_OK; }              /* multicast source MAC */
    type = g16(f + 12);
    if (type == ETH_ARP) {
        if (!r->have_mac) { for (i = 0; i < 6; i++) r->guest_mac[i] = f[6 + i]; r->have_mac = 1; }
        on_arp(r, f, len, t);
        return VBBA_OK;
    }
    if (type != ETH_IP) { r->st.tx_ethertype++; return VBBA_OK; }
    ip = f + 14;
    if (len < 34 || (ip[0] >> 4) != 4) { r->st.tx_bad++; return VBBA_OK; }
    ihl = (ip[0] & 15u) * 4u;
    tot = g16(ip + 2);
    if (ihl < 20 || tot < ihl || tot > (vbba_u32)len - 14u) { r->st.tx_bad++; return VBBA_OK; }
    if (vbba_csum(ip, ihl, 0) != 0) { r->st.tx_bad_csum++; return VBBA_OK; }
    if (g16(ip + 6) & 0x3fffu) { r->st.tx_frag++; return VBBA_OK; }  /* MF or a fragment offset */
    for (i = 0; i < 6; i++) r->guest_mac[i] = f[6 + i];            /* the guest's MAC (DHCP replies, inbound frames) */
    r->have_mac = 1;
    if (ip[9] == 17) on_udp(r, f, ihl, tot, t);
    else if (ip[9] == 1) on_icmp(r, f, ihl, tot, t);
#if VBBA_TCP
    else if (ip[9] == 6) { r->st.tx_tcp++; tcp_input(r, f, ihl, tot, t); }
#else
    else if (ip[9] == 6) r->st.tx_tcp++;
#endif
    else r->st.tx_proto++;
    return VBBA_OK;
}

/* ---- network -> guest ---- */
static int be_rx(void *ctx, vbba_u8 *f, vbba_u16 cap, vbba_u16 *len)
{
    struct vbba_router *r = (struct vbba_router *)ctx;
    vbba_u32 t, tries;
    *len = 0;
    while (r->lq_cnt) {
        vbba_u32 n = r->lq_len[r->lq_head], h = r->lq_head;
        r->lq_head = (vbba_u8)((r->lq_head + 1) % VBBA_LOCALQ);
        r->lq_cnt--;
        if (n > cap) { r->st.localq_drop++; continue; }       /* never hand out a cut frame */
        cpy(f, r->lq[h], n);
        *len = (vbba_u16)n;
        return VBBA_OK;
    }
    if (cap < 60 + 42) return VBBA_EMPTY;
    t = now(r);
    for (tries = 0; tries < 4u * VBBA_MAPS; tries++) {
        struct vbba_map *m = &r->map[r->rr % VBBA_MAPS];
        vbba_u32 ip = 0;
        vbba_u16 port = 0, room = (vbba_u16)(cap - 42u);
        int n, mi = r->rr % VBBA_MAPS;
        if (room > VBBA_UDP_MAX) room = VBBA_UDP_MAX;
        if (!m->used || (r->tp->state && r->tp->state(r->tp->ctx, m->h) != VBBA_TP_OPEN)) { r->rr++; continue; }
        n = r->tp->recv(r->tp->ctx, m->h, f + 42, room, &ip, &port);
        if (n == 0) { r->rr++; continue; }
        if (n == VBBA_TP_TRUNC) { r->st.rx_trunc++; continue; }
        if (n < 0) { r->st.rx_err++; r->rr++; continue; }
        if (!r->leased || !r->have_mac) { r->st.rx_no_lease++; continue; }
        if (!ct_ok(r, mi, ip, port, t) && !((r->cfg.flags & VBBA_RF_OPEN_INBOUND) && allowed(r, ip, port, 0))) {
            r->st.rx_unsolicited++;
            r->st.unsol_ip = ip; r->st.unsol_port = port; r->st.unsol_n++;
            continue;
        }
        r->st.rx_dgrams++;
        m->last = t;                                                 /* a port that only receives is in use too */
        { vbba_u32 q; for (q = 0; q < r->rxsrc_n; q++) if (r->rxsrc[q].ip == ip && r->rxsrc[q].port == port) { r->rxsrc[q].n++; break; }
          if (q == r->rxsrc_n && r->rxsrc_n < VBBA_RXSRC) { r->rxsrc[q].ip = ip; r->rxsrc[q].port = port; r->rxsrc[q].n = 1; r->rxsrc_n++; } }
        *len = (vbba_u16)build_udp(r, f, r->guest_mac, ip, r->leased, port, m->gport, (vbba_u32)n);
        return VBBA_OK;
    }
#if VBBA_TCP
    if (r->leased && r->have_mac) {                                  /* TCP segments after the datagrams: game traffic first */
        vbba_u32 n = tcp_output(r, f, cap, t);
        if (n) { *len = (vbba_u16)n; return VBBA_OK; }
    }
#endif
    return VBBA_EMPTY;
}

static int be_caps(void *ctx, struct vbba_frame_caps *o)
{
    struct vbba_router *r = (struct vbba_router *)ctx;
    int i;
    for (i = 0; i < 6; i++) o->mac[i] = r->guest_mac[i];
    o->max_tx_frame = 1514; o->max_rx_frame = 1514;
    o->tx_queue_depth = 1; o->rx_queue_depth = VBBA_LOCALQ;
    refresh(r);
    o->link_up = (vbba_u8)(r->lease_ip != 0);   /* the guest sees 'no link' until IOS has an address (the engine's driver waits in its link state and restarts DHCP when the link comes up); fixed-address configs are always up */
    return VBBA_OK;
}

static int be_poll(void *ctx)
{
    struct vbba_router *r = (struct vbba_router *)ctx;
    if (r->tp->poll) r->tp->poll(r->tp->ctx);
#if VBBA_TCP
    tcp_poll(r, now(r));
#endif
    if (r->dh_pend) {                                                /* a DISCOVER arrived before IOS had an address: offer now that it has one */
        vbba_u32 t = now(r);
        refresh(r);
        if ((vbba_u32)(t - r->dh_t) > 30000u) r->dh_pend = 0;        /* the guest has given up on that attempt */
        else if (r->lease_ip && r->lq_cnt < VBBA_LOCALQ) { r->dh_pend = 0; r->st.dhcp_deferred++; dhcp_reply(r, r->dh_msg, 2, t, 0); }
    }
#if VBBA_HOLD
    if (r->hold_used) {
        struct vbba_map *m = &r->map[r->hold_map];
        int s = (!m->used) ? VBBA_TP_FAILED : (r->tp->state ? r->tp->state(r->tp->ctx, m->h) : VBBA_TP_OPEN);
        if (s == VBBA_TP_OPEN) {
            int rc = r->tp->send(r->tp->ctx, m->h, r->hold_ip, r->hold_port, r->hold, r->hold_len);
            if (rc == VBBA_WOULD_BLOCK) return VBBA_OK;
            if (rc == VBBA_OK) { r->st.tx_hold_sent++; r->st.tx_udp++; } else r->st.tx_tp_err++;
            r->hold_used = 0;
        } else if (s != VBBA_TP_OPENING || (vbba_u32)(now(r) - r->hold_t) > 2000u) {
            r->st.tx_hold_drop++;
            r->hold_used = 0;
        }
    }
#endif
    return VBBA_OK;
}

static void be_reset(void *ctx)
{
    struct vbba_router *r = (struct vbba_router *)ctx;
    int i;
    for (i = 0; i < VBBA_MAPS; i++) if (r->map[i].used && r->tp->close) r->tp->close(r->tp->ctx, r->map[i].h);
    zero((vbba_u8 *)r->map, sizeof r->map);
    zero((vbba_u8 *)r->ct, sizeof r->ct);
#if VBBA_TCP
    tcp_reset_all(r);
#endif
    r->lq_cnt = 0;
    r->leased = 0;
    r->dh_pend = 0;
#if VBBA_HOLD
    r->hold_used = 0;
#endif
}

void vbba_router_init(struct vbba_router *r, const struct vbba_router_cfg *cfg, const struct vbba_transport *tp)
{
    zero((vbba_u8 *)r, sizeof *r);
    cpy((vbba_u8 *)&r->cfg, (const vbba_u8 *)cfg, sizeof *cfg);
    if (r->cfg.nallow > VBBA_ALLOW_MAX) r->cfg.nallow = VBBA_ALLOW_MAX;
    if (!r->cfg.ct_ms) r->cfg.ct_ms = 120000u;
    if (!r->cfg.tx_per_s) r->cfg.tx_per_s = 1000u;
    if (!r->cfg.arp_per_s) r->cfg.arp_per_s = 50u;
    r->tp = tp;
    r->be.context = r;
    r->be.get_capabilities = be_caps;
    r->be.submit_tx = be_tx;
    r->be.receive_rx = be_rx;
    r->be.poll = be_poll;
    r->be.reset = be_reset;
    r->win_ms = now(r);
    refresh(r);
}

const struct vbba_frame_backend *vbba_router_backend(struct vbba_router *r) { return &r->be; }

#if VBBA_TCP
#include "vbba_tcp.c"
#endif
