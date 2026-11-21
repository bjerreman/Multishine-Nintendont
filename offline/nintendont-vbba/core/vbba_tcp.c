/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_tcp: the router's TCP splice, see vbba_tcp.h.  Written from RFC 793 / 9293.  NOT a translation unit of its own: vbba_router.c
   includes it at its end when VBBA_TCP is 1 (it uses the router's static helpers: g16/g32/p16/p32/cpy/csum_*, allowed, refresh). */

#define TF_FIN 0x01u
#define TF_SYN 0x02u
#define TF_RST 0x04u
#define TF_PSH 0x08u
#define TF_ACK 0x10u
#define SEQ_LT(a, b) ((vbba_s32)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((vbba_s32)((a) - (b)) <= 0)
#define TMASK (VBBA_TCP_BUF - 1u)

#if (VBBA_TCP_BUF & (VBBA_TCP_BUF - 1u)) != 0
#error "VBBA_TCP_BUF must be a power of two"
#endif

static int tcp_have(const struct vbba_router *r)
{
    return r->tp && r->tp->s_open && r->tp->s_state && r->tp->s_send && r->tp->s_recv && r->tp->s_close;
}

/* IPv4 + TCP headers (+ the MSS option when mss != 0) around n payload bytes; returns the frame length (padded to 60) */
static vbba_u32 build_tcp(struct vbba_router *r, vbba_u8 *f, vbba_u32 sip, vbba_u16 sp, vbba_u16 dp, vbba_u32 seq, vbba_u32 ack,
                          vbba_u8 fl, vbba_u32 wnd, const vbba_u8 *d, vbba_u32 n, vbba_u32 mss)
{
    vbba_u8 *ip = f + 14, *th = f + 34, ph[12];
    vbba_u32 hl = mss ? 24u : 20u, tl = hl + n, s, k, len;
    cpy(f, r->guest_mac, 6); cpy(f + 6, r->cfg.router_mac, 6); p16(f + 12, ETH_IP);
    ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 20 + tl); p16(ip + 4, ++r->ip_id); p16(ip + 6, 0x4000u);
    ip[8] = 64; ip[9] = 6; p16(ip + 10, 0); p32(ip + 12, sip); p32(ip + 16, r->leased);
    p16(ip + 10, vbba_csum(ip, 20, 0));
    p16(th, sp); p16(th + 2, dp); p32(th + 4, seq); p32(th + 8, ack);
    th[12] = (vbba_u8)((hl / 4u) << 4); th[13] = fl;
    p16(th + 14, wnd > 0xffffu ? 0xffffu : wnd); p16(th + 16, 0); p16(th + 18, 0);
    if (mss) { th[20] = 2; th[21] = 4; p16(th + 22, mss); }
    if (d != th + hl) for (k = 0; k < n; k++) th[hl + k] = d[k];
    cpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 6; p16(ph + 10, tl);
    s = csum_add(0, ph, 12);
    p16(th + 16, csum_fold(csum_add(s, th, tl)));
    len = 34 + tl;
    while (len < 60) f[len++] = 0;
    r->tcp.st.seg_tx++;
    return len;
}

static vbba_u32 tc_wnd(const struct vbba_tcp_conn *c) { return VBBA_TCP_BUF - c->ts_n; }

static void tc_free(struct vbba_router *r, struct vbba_tcp_conn *c)
{
    if (c->h >= 0 && tcp_have(r)) r->tp->s_close(r->tp->ctx, c->h);
    c->h = -1;
    c->st = VTC_FREE;
}

/* the socket is gone (or the connection is given up): tell the guest with RST|ACK (sent by tcp_output, then freed) */
static void tc_abort(struct vbba_router *r, struct vbba_tcp_conn *c)
{
    if (c->h >= 0) r->tp->s_close(r->tp->ctx, c->h);
    c->h = -1;
    c->st = VTC_RST;
}

static void tc_orphan_rst(struct vbba_router *r, vbba_u32 ip, vbba_u16 sp, vbba_u16 dp, vbba_u32 seq, vbba_u32 ack, vbba_u8 fl)
{
    if (r->tcp.orst) return;                                   /* one at a time: the guest repeats */
    r->tcp.orst = 1; r->tcp.orst_ip = ip; r->tcp.orst_sp = sp; r->tcp.orst_dp = dp;
    r->tcp.orst_seq = seq; r->tcp.orst_ack = ack; r->tcp.orst_flags = fl;
}

static void tcp_input(struct vbba_router *r, const vbba_u8 *f, vbba_u32 ihl, vbba_u32 tot, vbba_u32 t)
{
    const vbba_u8 *ip = f + 14, *th = ip + ihl;
    vbba_u32 tl = tot - ihl, doff, sip = g32(ip + 12), dip = g32(ip + 16), seq, ack, dlen, s, i;
    vbba_u16 sp, dp;
    vbba_u8 fl, ph[12];
    struct vbba_tcp_conn *c = 0;
    if (tl < 20) { r->tcp.st.bad++; return; }
    doff = (vbba_u32)(th[12] >> 4) * 4u;
    if (doff < 20 || doff > tl) { r->tcp.st.bad++; return; }
    cpy(ph, ip + 12, 8); ph[8] = 0; ph[9] = 6; p16(ph + 10, tl);
    s = csum_add(0, ph, 12);
    if (csum_fold(csum_add(s, th, tl)) != 0) { r->tcp.st.bad++; r->st.tx_bad_csum++; return; }
    refresh(r);
    if (!r->leased || sip != r->leased) { r->st.tx_spoof++; return; }
    sp = (vbba_u16)g16(th); dp = (vbba_u16)g16(th + 2); seq = g32(th + 4); ack = g32(th + 8); fl = th[13];
    dlen = tl - doff;
    r->tcp.st.seg_rx++;
    for (i = 0; i < VBBA_TCP_CONNS; i++) {
        struct vbba_tcp_conn *k = &r->tcp.c[i];
        if (k->st != VTC_FREE && k->gport == sp && k->rport == dp && k->rip == dip) { c = k; break; }
    }
    if (!c) {
        if (fl & TF_RST) return;
        if ((fl & (TF_SYN | TF_ACK)) == TF_SYN) {
            r->tcp.st.syn++;
            if (dip == r->leased || dip == r->router_ip || dip == 0xffffffffu || (dip >> 28) == 0xEu || sp == 0 || dp == 0 ||
                !tcp_have(r) || !allowed(r, dip, dp, 1)) {
                r->tcp.st.denied++; r->st.tx_denied++;
                tc_orphan_rst(r, dip, dp, sp, 0, seq + 1u, TF_RST | TF_ACK);
                return;
            }
            for (i = 0; i < VBBA_TCP_CONNS; i++) if (r->tcp.c[i].st == VTC_FREE) { c = &r->tcp.c[i]; break; }
            if (!c) {                                          /* every slot taken: the longest-idle one goes if it has been quiet for VBBA_TCP_EVICT_MS */
                struct vbba_tcp_conn *o = 0;
                for (i = 0; i < VBBA_TCP_CONNS; i++) if (r->tcp.c[i].st != VTC_RST && (!o || (vbba_s32)(r->tcp.c[i].t_last - o->t_last) < 0)) o = &r->tcp.c[i];
                if (o && (vbba_u32)(t - o->t_last) >= VBBA_TCP_EVICT_MS) { r->tcp.st.evicted++; tc_free(r, o); c = o; }
                else { r->tcp.st.noslot++; return; }           /* the guest's SYN retry may find a slot */
            }
            c->ack_due = c->synack_due = c->guest_fin = c->sock_eof = c->fin_sent = c->fin_acked = c->shut = 0;
            c->gport = sp; c->rport = dp; c->rip = dip;
            c->irs = seq; c->rcv_nxt = seq + 1u; c->wnd_adv = VBBA_TCP_BUF;
            c->mss = 536;
            for (i = 20; i + 1 < doff; ) {                     /* options: only MSS matters */
                vbba_u8 kind = th[i];
                if (kind == 0) break;
                if (kind == 1) { i++; continue; }
                if (th[i + 1] < 2 || i + th[i + 1] > doff) break;
                if (kind == 2 && th[i + 1] == 4) c->mss = (vbba_u16)g16(th + i + 2);
                i += th[i + 1];
            }
            if (c->mss > VBBA_TCP_MSS) c->mss = VBBA_TCP_MSS;
            if (c->mss < 64) c->mss = 64;
            c->snd_wnd = 0; c->tg_head = c->tg_n = c->ts_head = c->ts_n = 0; c->tries = 0; c->rto = VBBA_TCP_RTO_MS;
            c->t_open = c->t_last = c->t_send = t;
            c->snd_una = c->snd_nxt = c->snd_max = c->iss = 0;
            c->h = r->tp->s_open(r->tp->ctx, dip, dp);
            if (c->h < 0) { r->tcp.st.open_fail++; c->st = VTC_RST; return; }
            c->st = VTC_CONNECTING;
            return;
        }
        r->tcp.st.orphan_rst++;
        if (fl & TF_ACK) tc_orphan_rst(r, dip, dp, sp, ack, 0, TF_RST);
        else tc_orphan_rst(r, dip, dp, sp, 0, seq + dlen + ((fl & TF_SYN) ? 1u : 0u) + ((fl & TF_FIN) ? 1u : 0u), TF_RST | TF_ACK);
        return;
    }
    c->t_last = t;
    if (fl & TF_RST) {                                         /* in window: the guest gave up on this connection */
        if (SEQ_LE(c->rcv_nxt, seq) && SEQ_LT(seq, c->rcv_nxt + VBBA_TCP_BUF + 1u)) { r->tcp.st.rst_rx++; r->tcp.st.closed++; tc_free(r, c); }
        return;
    }
    if (fl & TF_SYN) {                                         /* a repeated SYN: our SYN-ACK was lost (or not sent yet) */
        if (c->st == VTC_SYNACK && seq == c->irs) c->synack_due = 1;
        else if (c->st == VTC_EST) c->ack_due = 1;
        return;
    }
    if (c->st == VTC_CONNECTING || c->st == VTC_RST) return;
    if (!(fl & TF_ACK)) return;
    if (c->st == VTC_SYNACK) {
        if (ack != c->iss + 1u) { tc_orphan_rst(r, dip, dp, sp, ack, 0, TF_RST); return; }
        c->st = VTC_EST;
        c->snd_una = c->snd_nxt = c->snd_max = c->iss + 1u;
        c->tries = 0; c->rto = VBBA_TCP_RTO_MS;
    }
    /* ACK of our data / FIN */
    if (SEQ_LE(c->snd_una, ack) && SEQ_LE(ack, c->snd_max)) {
        vbba_u32 n = ack - c->snd_una, fin_seq = c->snd_una + c->tg_n;
        if (n) {
            if (c->sock_eof && ack == fin_seq + 1u) { n = c->tg_n; c->fin_acked = 1; }       /* our FIN is acknowledged (even if a go-back made us forget we sent it) */
            c->tg_head = (c->tg_head + n) & TMASK; c->tg_n -= n;
            c->snd_una = ack;
            if (SEQ_LT(c->snd_nxt, c->snd_una)) c->snd_nxt = c->snd_una;
            c->tries = 0; c->rto = VBBA_TCP_RTO_MS; c->t_send = t;
        }
        c->snd_wnd = g16(th + 14);
    }
    /* data / FIN from the guest */
    if (dlen || (fl & TF_FIN)) {
        if (seq == c->rcv_nxt && !c->guest_fin) {
            vbba_u32 room = VBBA_TCP_BUF - c->ts_n, take = dlen < room ? dlen : room, k, at = (c->ts_head + c->ts_n) & TMASK;
            const vbba_u8 *d = th + doff;
            for (k = 0; k < take; k++) { c->ts[at] = d[k]; at = (at + 1u) & TMASK; }
            c->ts_n += take; c->rcv_nxt += take; r->tcp.st.bytes_up += take;
            if (take == dlen && (fl & TF_FIN)) { c->rcv_nxt++; c->guest_fin = 1; }
        } else r->tcp.st.ooo++;
        c->ack_due = 1;
    }
}

/* the next segment for the guest, if any: 0 = nothing */
static vbba_u32 tcp_output(struct vbba_router *r, vbba_u8 *f, vbba_u16 cap, vbba_u32 t)
{
    vbba_u32 i;
    if (cap < 60 + 24) return 0;
    if (r->tcp.orst) {
        r->tcp.orst = 0; r->tcp.st.rst_tx++;
        return build_tcp(r, f, r->tcp.orst_ip, r->tcp.orst_sp, r->tcp.orst_dp, r->tcp.orst_seq, r->tcp.orst_ack, r->tcp.orst_flags, 0, 0, 0, 0);
    }
    for (i = 0; i < VBBA_TCP_CONNS; i++) {
        struct vbba_tcp_conn *c = &r->tcp.c[(r->tcp.rr + i) % VBBA_TCP_CONNS];
        vbba_u32 fin_seq, off, wnd, room, n, k, at;
        if (c->st == VTC_FREE || c->st == VTC_CONNECTING) continue;
        r->tcp.rr = (vbba_u8)((r->tcp.rr + i + 1u) % VBBA_TCP_CONNS);
        if (c->st == VTC_RST) {
            n = build_tcp(r, f, c->rip, c->rport, c->gport, c->snd_nxt, c->rcv_nxt, TF_RST | TF_ACK, 0, 0, 0, 0);
            r->tcp.st.rst_tx++; r->tcp.st.closed++;
            tc_free(r, c);
            return n;
        }
        if (c->st == VTC_SYNACK) {
            if (!c->synack_due) continue;
            c->synack_due = 0; c->snd_nxt = c->snd_max = c->iss + 1u; c->t_send = t;
            c->wnd_adv = tc_wnd(c);
            return build_tcp(r, f, c->rip, c->rport, c->gport, c->iss, c->rcv_nxt, TF_SYN | TF_ACK, c->wnd_adv, 0, 0, VBBA_TCP_MSS);
        }
        fin_seq = c->snd_una + c->tg_n;
        off = c->snd_nxt - c->snd_una;
        if (SEQ_LT(c->snd_nxt, fin_seq)) {
            wnd = c->snd_wnd < VBBA_TCP_INFLIGHT ? c->snd_wnd : VBBA_TCP_INFLIGHT;
            room = wnd > off ? wnd - off : 0;
            if (!c->snd_wnd && !off && (vbba_u32)(t - c->t_send) >= c->rto) room = 1;     /* zero-window probe */
            n = fin_seq - c->snd_nxt;
            if (n > room) n = room;
            if (n > c->mss) n = c->mss;
            if (n > (vbba_u32)cap - 58u) n = (vbba_u32)cap - 58u;
            if (n) {
                vbba_u8 *pl = f + 54;                        /* the payload goes straight to its place in the frame (no stack buffer on the ARM) */
                at = (c->tg_head + off) & TMASK;
                for (k = 0; k < n; k++) { pl[k] = c->tg[at]; at = (at + 1u) & TMASK; }
                if (!off) c->t_send = t;
                if (SEQ_LT(c->snd_nxt, c->snd_max)) r->tcp.st.retrans++;
                k = c->snd_nxt;
                c->snd_nxt += n;
                if (SEQ_LT(c->snd_max, c->snd_nxt)) c->snd_max = c->snd_nxt;
                c->ack_due = 0; c->wnd_adv = tc_wnd(c);
                return build_tcp(r, f, c->rip, c->rport, c->gport, k, c->rcv_nxt, TF_ACK | TF_PSH, c->wnd_adv, pl, n, 0);
            }
        } else if (c->sock_eof && c->snd_nxt == fin_seq && !c->fin_acked) {
            if (!off) c->t_send = t;
            c->snd_nxt = fin_seq + 1u; c->fin_sent = 1;
            if (SEQ_LT(c->snd_max, c->snd_nxt)) c->snd_max = c->snd_nxt;
            c->ack_due = 0; c->wnd_adv = tc_wnd(c);
            return build_tcp(r, f, c->rip, c->rport, c->gport, fin_seq, c->rcv_nxt, TF_FIN | TF_ACK, c->wnd_adv, 0, 0, 0);
        }
        if (c->ack_due) {
            c->ack_due = 0; c->wnd_adv = tc_wnd(c);
            return build_tcp(r, f, c->rip, c->rport, c->gport, c->snd_nxt, c->rcv_nxt, TF_ACK, c->wnd_adv, 0, 0, 0);
        }
    }
    return 0;
}

static void tcp_poll(struct vbba_router *r, vbba_u32 t)
{
    vbba_u32 i;
    if (!tcp_have(r)) return;
    for (i = 0; i < VBBA_TCP_CONNS; i++) {
        struct vbba_tcp_conn *c = &r->tcp.c[i];
        if (c->st == VTC_CONNECTING) {
            int s = r->tp->s_state(r->tp->ctx, c->h);
            if (s == VBBA_TP_OPEN) {
                c->st = VTC_SYNACK; c->synack_due = 1; c->rto = VBBA_TCP_RTO_MS; c->tries = 0;
                c->iss = (t * 2654435761u) ^ (++r->tcp.isn_ctr * 0x9E3779B9u) ^ ((vbba_u32)c->gport << 16) ^ c->irs;
                r->tcp.st.opened++;
            } else if (s != VBBA_TP_OPENING || (vbba_u32)(t - c->t_open) > VBBA_TCP_CONNECT_MS) {
                r->tcp.st.open_fail++;
                if (s == VBBA_TP_OPENING) r->tcp.st.abort_connect++;
                c->snd_nxt = 0;
                tc_abort(r, c);
            }
            continue;
        }
        if (c->st == VTC_SYNACK) {
            if (!c->synack_due && (vbba_u32)(t - c->t_send) >= c->rto) {
                if (++c->tries > VBBA_TCP_SYNACK_TRIES) { r->tcp.st.abort_tries++; tc_abort(r, c); continue; }
                c->synack_due = 1; r->tcp.st.synack_rtx++;
                c->rto = c->rto * 2u > VBBA_TCP_RTO_MAX_MS ? VBBA_TCP_RTO_MAX_MS : c->rto * 2u;
            }
            continue;
        }
        if (c->st != VTC_EST) continue;
        /* guest -> socket */
        if (c->ts_n) {
            vbba_u32 run = VBBA_TCP_BUF - c->ts_head;
            int n = r->tp->s_send(r->tp->ctx, c->h, c->ts + c->ts_head, (vbba_u16)(c->ts_n < run ? c->ts_n : run));
            if (n < 0) { r->tcp.st.sock_err++; tc_abort(r, c); continue; }
            if (n > 0) {
                c->ts_head = (c->ts_head + (vbba_u32)n) & TMASK; c->ts_n -= (vbba_u32)n;
                if (c->wnd_adv < c->mss && tc_wnd(c) >= c->wnd_adv + c->mss) c->ack_due = 1;   /* window update after a (nearly) closed window */
            }
        }
        /* socket -> guest */
        if (!c->sock_eof && c->tg_n < VBBA_TCP_BUF) {
            vbba_u32 tail = (c->tg_head + c->tg_n) & TMASK, room = VBBA_TCP_BUF - c->tg_n, run = VBBA_TCP_BUF - tail;
            int n = r->tp->s_recv(r->tp->ctx, c->h, c->tg + tail, (vbba_u16)(room < run ? room : run));
            if (n > 0) { c->tg_n += (vbba_u32)n; r->tcp.st.bytes_down += (vbba_u32)n; c->t_last = t; }
            else if (n == VBBA_TP_EOF) { c->sock_eof = 1; r->tcp.st.sock_eof++; }
            else if (n < 0) { r->tcp.st.sock_err++; tc_abort(r, c); continue; }
        }
        /* retransmission toward the guest (go-back-N) */
        if (c->snd_nxt != c->snd_una && (vbba_u32)(t - c->t_send) >= c->rto) {
            if (++c->tries > VBBA_TCP_TRIES) { r->tcp.st.abort_tries++; tc_abort(r, c); continue; }
            c->snd_nxt = c->snd_una; c->t_send = t;
            c->rto = c->rto * 2u > VBBA_TCP_RTO_MAX_MS ? VBBA_TCP_RTO_MAX_MS : c->rto * 2u;
        }
        /* closing: the guest's FIN goes on as a half-close once its bytes are with the socket; the slot is freed when both sides are done and our last ACK is out */
        if (c->guest_fin && !c->ts_n && !c->shut) { c->shut = 1; r->tcp.st.shutdowns++; if (r->tp->s_shutdown) r->tp->s_shutdown(r->tp->ctx, c->h); }
        if (c->guest_fin && c->sock_eof && c->fin_acked && !c->ts_n && !c->ack_due) { r->tcp.st.closed++; tc_free(r, c); continue; }
        if ((c->guest_fin || c->fin_acked) && (vbba_u32)(t - c->t_last) > VBBA_TCP_LINGER_MS) { r->tcp.st.abort_linger++; tc_abort(r, c); continue; }
        if ((vbba_u32)(t - c->t_last) > VBBA_TCP_IDLE_MS) { r->tcp.st.abort_idle++; tc_abort(r, c); continue; }
    }
}

static void tcp_reset_all(struct vbba_router *r)
{
    vbba_u32 i;
    for (i = 0; i < VBBA_TCP_CONNS; i++) if (r->tcp.c[i].st != VTC_FREE) tc_free(r, &r->tcp.c[i]);
    r->tcp.orst = 0;
}
