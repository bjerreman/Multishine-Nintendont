/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* sdtcp: see sdtcp.h.  Strict C89. */
#include "sdtcp.h"

#if SDNET_ENABLE_TCP

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

/* conn->flags */
#define FL_RTO 0x0001           /* retransmit timer armed */
#define FL_ACK_NOW 0x0002
#define FL_DELACK 0x0004        /* delayed ACK timer armed */
#define FL_PERSIST 0x0008       /* zero-window probe timer armed */
#define FL_FIN_QUEUED 0x0010    /* app closed; FIN sits at wr_seq */
#define FL_FIN_RCVD 0x0020
#define FL_RECOVERY 0x0040
#define FL_SACK_OK 0x0080       /* peer offered SACK-permitted */
#define FL_RTT_ON 0x0100
#define FL_KEEP 0x0200
#define FL_TW 0x0400            /* t_tw armed (TIME_WAIT / FIN_WAIT_2 / LAST_ACK / CLOSING give-up) */
#define FL_NEWCHILD 0x0800      /* listener child waiting for accept */
#define FL_RETX_SENT 0x1000     /* something was retransmitted since the last RTT sample (Karn) */
#define FL_ACK_URGENT 0x2000    /* ACK_NOW that must not wait for the end of the poll (duplicate/out-of-order/FIN/challenge) */

#define SEQ_LT(a, b) ((sdnet_s32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((sdnet_s32)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((sdnet_s32)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((sdnet_s32)((a) - (b)) >= 0)

static sdtcp_u16 rd16(const sdtcp_u8 *p) { return (sdtcp_u16)((p[0] << 8) | p[1]); }
static sdtcp_u32 rd32(const sdtcp_u8 *p)
{
    return ((sdtcp_u32)p[0] << 24) | ((sdtcp_u32)p[1] << 16) | ((sdtcp_u32)p[2] << 8) | p[3];
}
static void wr16(sdtcp_u8 *p, sdtcp_u16 v) { p[0] = (sdtcp_u8)(v >> 8); p[1] = (sdtcp_u8)v; }
static void wr32(sdtcp_u8 *p, sdtcp_u32 v)
{
    p[0] = (sdtcp_u8)(v >> 24); p[1] = (sdtcp_u8)(v >> 16); p[2] = (sdtcp_u8)(v >> 8); p[3] = (sdtcp_u8)v;
}
static sdtcp_u32 umin(sdtcp_u32 a, sdtcp_u32 b) { return a < b ? a : b; }
static sdtcp_u32 umax(sdtcp_u32 a, sdtcp_u32 b) { return a > b ? a : b; }
static int due(sdtcp_u32 now, sdtcp_u32 dl) { return (sdnet_s32)(now - dl) >= 0; }

/* ---- ring helpers: byte with sequence number q lives at buf[q & mask] ---- */
static void ring_put(sdtcp_u8 *buf, sdtcp_u32 mask, sdtcp_u32 seq, const sdtcp_u8 *src, sdtcp_u32 n)
{
    sdtcp_u32 i;
    for (i = 0; i < n; i++) {
        buf[(seq + i) & mask] = src[i];
    }
}
static void ring_get(const sdtcp_u8 *buf, sdtcp_u32 mask, sdtcp_u32 seq, sdtcp_u8 *dst, sdtcp_u32 n)
{
    sdtcp_u32 i, o = seq & mask, first;

    first = mask + 1 - o;
    if (first >= n) {
        for (i = 0; i < n; i++) dst[i] = buf[o + i];
    } else {
        for (i = 0; i < first; i++) dst[i] = buf[o + i];
        for (i = first; i < n; i++) dst[i] = buf[i - first];
    }
}

int sdtcp_conn_init(struct sdtcp_conn *c, void *rx, sdtcp_u32 rxlen, void *tx, sdtcp_u32 txlen)
{
    sdtcp_u8 *b = (sdtcp_u8 *)c;
    sdtcp_u32 i;

    if (!rx || !tx || rxlen < 1024 || txlen < 1024 || (rxlen & (rxlen - 1)) || (txlen & (txlen - 1)) ||
        rxlen > 0x10000u || txlen > 0x10000u) {
        return SDTCP_E_PARAM;
    }
    for (i = 0; i < sizeof(*c); i++) b[i] = 0;
    c->rxbuf = (sdtcp_u8 *)rx;
    c->txbuf = (sdtcp_u8 *)tx;
    c->rxmask = rxlen - 1;
    c->txmask = txlen - 1;
    return 0;
}

/* ---- keyed mixing (ISN and ephemeral port).  NOT cryptographic: it only stops off-path guessing of sequence numbers.
   A TLS layer on top never relies on TCP for secrecy or integrity. ---- */
static sdtcp_u32 mix(sdtcp_u32 h, sdtcp_u32 x)
{
    h ^= x;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}
static sdtcp_u32 isn_for(const struct sdtcp *t, sdtcp_u32 lip, sdtcp_u16 lport, sdtcp_u32 rip, sdtcp_u16 rport)
{
    sdtcp_u32 h = t->secret[0];

    h = mix(h ^ t->secret[1], lip);
    h = mix(h ^ t->secret[2], rip);
    h = mix(h ^ t->secret[3], ((sdtcp_u32)lport << 16) | rport);
    return h + t->net->now * 250u;    /* RFC 6528: a 4 microsecond clock, here 250 per ms */
}

static void rx_thunk(void *t, sdnet_u32 src, sdnet_u32 dst, const sdnet_u8 *seg, sdnet_u32 len) { sdtcp_rx((struct sdtcp *)t, src, dst, seg, len); }
static void poll_thunk(void *t) { sdtcp_poll((struct sdtcp *)t); }

int sdtcp_init(struct sdtcp *t, struct sdnet *net, struct sdtcp_conn *conns, int nconns, const sdtcp_u8 seed[16])
{
    sdtcp_u8 *b = (sdtcp_u8 *)t;
    sdtcp_u32 i;

    if (!t || !net || !conns || nconns < 1 || !seed) return SDTCP_E_PARAM;
    for (i = 0; i < sizeof(*t); i++) b[i] = 0;
    t->net = net;
    t->conn = conns;
    t->nconn = nconns;
    for (i = 0; i < 4; i++) t->secret[i] = rd32(seed + 4 * i);
    t->mss_adv = SDTCP_MSS;
    t->wnd_cap = 0xffffu;
    t->last_rst = net->now - 1000u;
    net->tcp = t;
    net->tcp_rx = rx_thunk;
    net->tcp_poll = poll_thunk;
    return 0;
}

void sdtcp_set_window_cap(struct sdtcp *t, sdtcp_u32 cap)
{
    t->wnd_cap = cap < 1u ? 1u : cap;
}

void sdtcp_set_mss(struct sdtcp *t, sdtcp_u16 mss)
{
    if (mss >= 536 && mss <= SDTCP_MSS) t->mss_adv = mss;
}

/* ---- segment transmit ---- */
static sdtcp_u16 rcv_window(const struct sdtcp *t, const struct sdtcp_conn *c)
{
    sdtcp_u32 end, used, size, free_, thr;

    if (t->mode != SDTCP_RUN) return 0;
    size = c->rxmask + 1;
    end = c->rcv_nxt - ((c->flags & FL_FIN_RCVD) ? 1u : 0u);
    used = end - c->rd_seq;
    free_ = size - used;
    thr = umin(size / 2, c->mss);
    if (free_ < thr) return 0;                 /* receiver silly-window avoidance */
    return (sdtcp_u16)umin(umin(free_, 0xffffu), t->wnd_cap);
}

/* Build and send one segment.  dlen bytes of data are taken from the tx ring at sequence seq. */
static int seg_send(struct sdtcp *t, struct sdtcp_conn *c, sdtcp_u32 seq, sdtcp_u8 flags, sdtcp_u32 dlen, int sack_opt)
{
    struct sdnet *s = t->net;
    sdtcp_u8 *p = s->txbuf + SDNET_L4_OFF;
    sdtcp_u32 hl = 20, i, sum;
    sdtcp_u8 ph[12];
    sdtcp_u16 win, ck;
    int r;

    wr16(p, c->lport);
    wr16(p + 2, c->rport);
    wr32(p + 4, seq);
    wr32(p + 8, (flags & TH_ACK) ? c->rcv_nxt : 0);
    win = rcv_window(t, c);
    if (flags & TH_SYN) {
        win = (sdtcp_u16)umin(umin(c->rxmask + 1, 0xffffu), t->wnd_cap);
        p[20] = 2; p[21] = 4; wr16(p + 22, t->mss_adv);       /* MSS */
        p[24] = 4; p[25] = 2; p[26] = 1; p[27] = 1;           /* SACK-permitted, NOP, NOP */
        hl = 28;
    } else if (sack_opt && (c->flags & FL_SACK_OK) && c->ooo_lo != c->ooo_hi) {
        p[20] = 1; p[21] = 1; p[22] = 5; p[23] = 10;
        wr32(p + 24, c->ooo_lo);
        wr32(p + 28, c->ooo_hi);
        hl = 32;
    }
    p[12] = (sdtcp_u8)((hl / 4) << 4);
    p[13] = flags;
    wr16(p + 14, win);
    wr16(p + 16, 0);
    wr16(p + 18, 0);
    if (dlen) {
        ring_get(c->txbuf, c->txmask, seq, p + hl, dlen);
    }
    wr32(ph, s->ip);
    wr32(ph + 4, c->rip);
    ph[8] = 0;
    ph[9] = 6;
    wr16(ph + 10, (sdtcp_u16)(hl + dlen));
    sum = sdnet_csum_add(0, ph, 12);
    sum = sdnet_csum_add(sum, p, hl + dlen);
    ck = sdnet_csum_fold(sum);
    wr16(p + 16, ck);
    (void)i;
    r = sdnet_tx_ip(s, 6, c->rip, (sdnet_u16)(hl + dlen));
    if (r == SDNET_E_ARP) {
        t->st.arp_wait++;
        return r;
    }
    if (r < 0) return r;
    t->st.seg_tx++;
    t->st.data_tx_bytes += dlen;
    c->t_last_tx = s->now;
    c->rcv_adv = c->rcv_nxt + win;
    c->flags &= (sdtcp_u16)~(FL_ACK_NOW | FL_DELACK | FL_ACK_URGENT);
    c->unacked_bytes = 0;
    return 0;
}

/* bare RST in answer to a segment that matches no connection */
static void send_rst_raw(struct sdtcp *t, sdtcp_u32 dst, sdtcp_u16 sport, sdtcp_u16 dport, sdtcp_u32 seq, sdtcp_u32 ack, int use_ack)
{
    struct sdnet *s = t->net;
    sdtcp_u8 *p = s->txbuf + SDNET_L4_OFF;
    sdtcp_u8 ph[12];
    sdtcp_u32 sum;

    if (!due(s->now, t->last_rst + 20u)) return;     /* at most 50 RST/s */
    t->last_rst = s->now;
    wr16(p, sport); wr16(p + 2, dport);
    wr32(p + 4, seq); wr32(p + 8, ack);
    p[12] = 0x50;
    p[13] = (sdtcp_u8)(TH_RST | (use_ack ? TH_ACK : 0));
    wr16(p + 14, 0); wr16(p + 16, 0); wr16(p + 18, 0);
    wr32(ph, s->ip); wr32(ph + 4, dst); ph[8] = 0; ph[9] = 6; wr16(ph + 10, 20);
    sum = sdnet_csum_add(0, ph, 12);
    sum = sdnet_csum_add(sum, p, 20);
    wr16(p + 16, sdnet_csum_fold(sum));
    if (sdnet_tx_ip(s, 6, dst, 20) == 0) {
        t->st.seg_tx++;
        t->st.rst_tx++;
    }
}

/* ---- timers ---- */
static void rto_arm(struct sdtcp *t, struct sdtcp_conn *c)
{
    c->t_rto = t->net->now + c->rto;
    c->flags |= FL_RTO;
}
static void rto_clear(struct sdtcp_conn *c) { c->flags &= (sdtcp_u16)~FL_RTO; }

static void conn_reset_state(struct sdtcp_conn *c, int err)
{
    c->state = SDTCP_CLOSED;
    c->err = err;
    c->flags = 0;
}

static void rto_update(struct sdtcp_conn *c, sdtcp_u32 r)
{
    sdtcp_u32 r8 = r * 8u, av;
    sdnet_s32 e;

    if (r8 == 0) r8 = 1;
    if (c->srtt == 0) {
        c->srtt = r8;
        c->rttvar = r8 / 2;
    } else {
        e = (sdnet_s32)(r8 - c->srtt);
        c->srtt = (sdtcp_u32)((sdnet_s32)c->srtt + e / 8);
        av = (sdtcp_u32)(e < 0 ? -e : e);
        c->rttvar = (sdtcp_u32)((sdnet_s32)c->rttvar + ((sdnet_s32)av - (sdnet_s32)c->rttvar) / 4);
    }
    c->rto = (c->srtt + 4u * c->rttvar) / 8u;
    if (c->rto < SDTCP_RTO_MIN_MS) c->rto = SDTCP_RTO_MIN_MS;
    if (c->rto > SDTCP_RTO_MAX_MS) c->rto = SDTCP_RTO_MAX_MS;
}

#ifdef SDTCP_TEST_HOOKS
static sdtcp_u32 g_test_iw;
void sdtcp_test_set_iw(sdtcp_u32 segs) { g_test_iw = segs; }
#endif
static sdtcp_u32 init_window(const struct sdtcp_conn *c)
{
#ifdef SDTCP_TEST_HOOKS
    if (g_test_iw) return g_test_iw * c->mss;
#endif
    return umin(4u * c->mss, umax(2u * c->mss, 4380u));
}

/* ---- output ---- */
static int in_data_state(const struct sdtcp_conn *c)
{
    return c->state == SDTCP_ESTABLISHED || c->state == SDTCP_CLOSE_WAIT || c->state == SDTCP_FIN_WAIT_1 ||
           c->state == SDTCP_LAST_ACK || c->state == SDTCP_CLOSING;
}

static void send_syn(struct sdtcp *t, struct sdtcp_conn *c)
{
    sdtcp_u8 fl = (sdtcp_u8)(TH_SYN | (c->state == SDTCP_SYN_RCVD ? TH_ACK : 0));

    if (seg_send(t, c, c->iss, fl, 0, 0) == 0) {
        if (c->snd_nxt == c->iss) c->snd_nxt = c->iss + 1;
        if (SEQ_LT(c->snd_max, c->snd_nxt)) c->snd_max = c->snd_nxt;
        if (!(c->flags & FL_RTO)) rto_arm(t, c);
        t->budget--;
    }
}

/* final=0 from the receive path: new data may go out, but a plain ACK waits for the end-of-poll call unless urgent */
static void conn_output(struct sdtcp *t, struct sdtcp_conn *c, int final)
{
    sdtcp_u32 fin_seq, avail, inflight, wnd, room, len;
    int fin, sent = 0;
    sdtcp_u8 fl;

    if (c->state == SDTCP_SYN_SENT || c->state == SDTCP_SYN_RCVD) {
        if (c->snd_nxt == c->iss && t->budget > 0) send_syn(t, c);
        return;
    }
    if (!in_data_state(c)) {
        if ((c->flags & FL_ACK_NOW) && (final || (c->flags & FL_ACK_URGENT)) && c->state != SDTCP_CLOSED && c->state != SDTCP_LISTEN_ST && t->budget > 0) {
            if (seg_send(t, c, c->snd_nxt, TH_ACK, 0, 1) == 0) t->budget--;
        }
        return;
    }
    fin_seq = c->wr_seq;
    while (t->budget > 0) {
        avail = SEQ_GT(c->wr_seq, c->snd_nxt) ? c->wr_seq - c->snd_nxt : 0;
        inflight = c->snd_nxt - c->snd_una;
        wnd = umin(c->cwnd, c->snd_wnd);
        if (t->mode != SDTCP_RUN) wnd = 0;
        room = inflight < wnd ? wnd - inflight : 0;
        len = umin(umin(avail, c->mss), room);
        fin = (c->flags & FL_FIN_QUEUED) && c->snd_nxt + len == fin_seq && t->mode == SDTCP_RUN;
        if (len == 0 && !fin) break;
        if (len < c->mss / 2u && len < avail && inflight > 0) break;     /* sender silly-window avoidance */
        fl = TH_ACK;
        if (len && len == avail) fl |= TH_PSH;
        if (fin) fl |= TH_FIN;
        if (seg_send(t, c, c->snd_nxt, fl, len, 0) != 0) break;
        t->budget--;
        sent++;
        if (SEQ_LT(c->snd_nxt, c->snd_max)) {
            t->st.retrans++;
            c->flags |= FL_RETX_SENT;
            c->flags &= (sdtcp_u16)~FL_RTT_ON;
        } else if (!(c->flags & FL_RTT_ON) && (len || fin)) {
            c->flags |= FL_RTT_ON;
            c->flags &= (sdtcp_u16)~FL_RETX_SENT;
            c->rtt_seq = c->snd_nxt + len + (fin ? 1u : 0u);
            c->rtt_t0 = t->net->now;
        }
        c->snd_nxt += len + (fin ? 1u : 0u);
        if (SEQ_LT(c->snd_max, c->snd_nxt)) c->snd_max = c->snd_nxt;
        if (!(c->flags & FL_RTO)) rto_arm(t, c);
        c->flags &= (sdtcp_u16)~FL_PERSIST;
        if (fin) break;
    }
    /* zero-window persist */
    if (!sent && c->snd_wnd == 0 && SEQ_GT(c->wr_seq, c->snd_nxt) && c->snd_una == c->snd_nxt && !(c->flags & FL_PERSIST)) {
        c->flags |= FL_PERSIST;
        c->t_persist = t->net->now + c->rto;
    }
    if ((c->flags & FL_ACK_NOW) && (final || (c->flags & FL_ACK_URGENT)) && t->budget > 0) {
        if (seg_send(t, c, c->snd_nxt, TH_ACK, 0, 1) == 0) t->budget--;
    }
}

/* ---- connection setup ---- */
static struct sdtcp_conn *alloc_conn(struct sdtcp *t, int *h)
{
    int i;
    for (i = 0; i < t->nconn; i++) {
        if (t->conn[i].state == SDTCP_CLOSED) {
            if (h) *h = i;
            return &t->conn[i];
        }
    }
    /* A TIME_WAIT slot is reclaimable at once: every connect picks a fresh random local port and ISN, so no old duplicate can be mistaken
       for the new connection, and a lost final ACK only costs the peer a harmless RST. */
    for (i = 0; i < t->nconn; i++) {
        if (t->conn[i].state == SDTCP_TIME_WAIT) {
            if (h) *h = i;
            conn_reset_state(&t->conn[i], 0);
            return &t->conn[i];
        }
    }
    return 0;
}

static void conn_reinit(struct sdtcp_conn *c)
{
    sdtcp_u8 *rx = c->rxbuf, *tx = c->txbuf;
    sdtcp_u32 rm = c->rxmask, tm = c->txmask;
    sdtcp_u8 *b = (sdtcp_u8 *)c;
    sdtcp_u32 i;

    for (i = 0; i < sizeof(*c); i++) b[i] = 0;
    c->rxbuf = rx; c->txbuf = tx; c->rxmask = rm; c->txmask = tm;
}

static int port_in_use(const struct sdtcp *t, sdtcp_u16 port)
{
    int i;
    for (i = 0; i < t->nconn; i++) {
        if (t->conn[i].state != SDTCP_CLOSED && t->conn[i].lport == port) return 1;
    }
    return 0;
}

static void conn_start(struct sdtcp *t, struct sdtcp_conn *c, sdtcp_u32 rip, sdtcp_u16 rport, sdtcp_u16 lport)
{
    conn_reinit(c);
    c->rip = rip;
    c->rport = rport;
    c->lport = lport;
    c->mss = t->mss_adv;
    c->iss = isn_for(t, t->net->ip, lport, rip, rport);
#ifdef SDTCP_TEST_HOOKS
    if (t->test_isn_on) c->iss = t->test_isn;
#endif
    c->snd_una = c->snd_nxt = c->snd_max = c->iss;
    c->wr_seq = c->iss + 1;
    c->rto = SDTCP_RTO_INIT_MS;
    c->snd_wnd = c->mss;
    c->cwnd = init_window(c);
    c->ssthresh = 0xffffffffu;
    c->t_last_rx = c->t_last_tx = t->net->now;
}

int sdtcp_connect(struct sdtcp *t, sdtcp_u32 ip, sdtcp_u16 port)
{
    struct sdtcp_conn *c;
    int h = -1, tries;
    sdtcp_u16 lp = 0;

    if (!t || !ip || !port) return SDTCP_E_PARAM;
    if (!t->net->ip) return SDTCP_E_NET;
    c = alloc_conn(t, &h);
    if (!c) return SDTCP_E_NOSLOT;
    for (tries = 0; tries < 64; tries++) {
        t->ctr++;
        lp = (sdtcp_u16)(49152u + (mix(t->secret[0] ^ t->ctr, t->net->now + ip) % 16384u));
        if (!port_in_use(t, lp)) break;
        lp = 0;
    }
    if (!lp) return SDTCP_E_NOSLOT;
    conn_start(t, c, ip, port, lp);
    c->state = SDTCP_SYN_SENT;
    c->flags |= FL_TW;                       /* overall connect deadline (covers an ARP that never resolves) */
    c->t_tw = t->net->now + SDTCP_CONNECT_MS;
    if (t->budget <= 0) t->budget = 1;
    conn_output(t, c, 1);
    return h;
}

#if SDTCP_LISTEN
int sdtcp_listen(struct sdtcp *t, sdtcp_u16 port)
{
    struct sdtcp_conn *c;
    int h = -1;

    c = alloc_conn(t, &h);
    if (!c) return SDTCP_E_NOSLOT;
    conn_reinit(c);
    c->state = SDTCP_LISTEN_ST;
    c->lport = port;
    return h;
}

int sdtcp_accept(struct sdtcp *t, int lh)
{
    int i;
    if (lh < 0 || lh >= t->nconn || t->conn[lh].state != SDTCP_LISTEN_ST) return -1;
    for (i = 0; i < t->nconn; i++) {
        struct sdtcp_conn *c = &t->conn[i];
        if ((c->flags & FL_NEWCHILD) && c->lport == t->conn[lh].lport && c->state == SDTCP_ESTABLISHED) {
            c->flags &= (sdtcp_u16)~FL_NEWCHILD;
            return i;
        }
    }
    return -1;
}
#endif

/* ---- API ---- */
static struct sdtcp_conn *get(const struct sdtcp *t, int h)
{
    if (!t || h < 0 || h >= t->nconn) return 0;
    return &t->conn[h];
}

int sdtcp_state(const struct sdtcp *t, int h)
{
    const struct sdtcp_conn *c = get(t, h);
    return c ? c->state : SDTCP_CLOSED;
}
int sdtcp_error(const struct sdtcp *t, int h)
{
    const struct sdtcp_conn *c = get(t, h);
    return c ? c->err : SDTCP_E_PARAM;
}
int sdtcp_established(const struct sdtcp *t, int h)
{
    const struct sdtcp_conn *c = get(t, h);
    return c && (c->state == SDTCP_ESTABLISHED || c->state == SDTCP_CLOSE_WAIT);
}

sdtcp_u32 sdtcp_tx_free(const struct sdtcp *t, int h)
{
    const struct sdtcp_conn *c = get(t, h);
    sdtcp_u32 used;
    if (!c) return 0;
    if (c->flags & FL_FIN_QUEUED) return 0;
    used = c->wr_seq - c->snd_una;
    return used >= c->txmask + 1 ? 0 : c->txmask + 1 - used;
}
sdtcp_u32 sdtcp_tx_pending(const struct sdtcp *t, int h)
{
    const struct sdtcp_conn *c = get(t, h);
    if (!c) return 0;
    return SEQ_GT(c->wr_seq, c->snd_una) ? c->wr_seq - c->snd_una - (c->snd_una == c->iss ? 1u : 0u) : 0;
}
sdtcp_u32 sdtcp_rx_avail(const struct sdtcp *t, int h)
{
    const struct sdtcp_conn *c = get(t, h);
    if (!c || c->state == SDTCP_CLOSED || c->state == SDTCP_SYN_SENT || c->state == SDTCP_LISTEN_ST) return 0;
    return (c->rcv_nxt - ((c->flags & FL_FIN_RCVD) ? 1u : 0u)) - c->rd_seq;
}

int sdtcp_send(struct sdtcp *t, int h, const void *buf, sdtcp_u32 len)
{
    struct sdtcp_conn *c = get(t, h);
    sdtcp_u32 fr;

    if (!c || (!buf && len)) return SDTCP_E_PARAM;
    if (c->err) return c->err;
    if (c->state != SDTCP_SYN_SENT && c->state != SDTCP_SYN_RCVD && c->state != SDTCP_ESTABLISHED && c->state != SDTCP_CLOSE_WAIT) {
        return SDTCP_E_STATE;
    }
    if (c->flags & FL_FIN_QUEUED) return SDTCP_E_STATE;
    fr = sdtcp_tx_free(t, h);
    if (len > fr) len = fr;
    if (!len) return 0;
    if (c->snd_una == c->snd_max && c->state == SDTCP_ESTABLISHED &&
        (sdnet_s32)(t->net->now - c->t_last_tx) > (sdnet_s32)c->rto) {
        c->cwnd = umin(c->cwnd, init_window(c));         /* RFC 5681 restart after idle */
    }
    ring_put(c->txbuf, c->txmask, c->wr_seq, (const sdtcp_u8 *)buf, len);
    c->wr_seq += len;
    if (t->budget <= 0) t->budget = 1;
    conn_output(t, c, 1);
    return (int)len;
}

int sdtcp_recv(struct sdtcp *t, int h, void *buf, sdtcp_u32 max)
{
    struct sdtcp_conn *c = get(t, h);
    sdtcp_u32 av, n, before;

    if (!c || (!buf && max)) return SDTCP_E_PARAM;
    av = sdtcp_rx_avail(t, h);
    if (av == 0) {
        if (c->flags & FL_FIN_RCVD) return SDTCP_E_EOF;
        if (c->err) return c->err;
        return 0;
    }
    n = umin(av, max);
    before = rcv_window(t, c);
    ring_get(c->rxbuf, c->rxmask, c->rd_seq, (sdtcp_u8 *)buf, n);
    c->rd_seq += n;
    /* window update: tell the peer when the window re-opens or grows by half a buffer / one segment */
    if (c->state != SDTCP_CLOSED && c->state != SDTCP_SYN_SENT) {
        sdtcp_u32 now_w = rcv_window(t, c), adv = c->rcv_adv - c->rcv_nxt;
        if (now_w >= adv + umin((c->rxmask + 1) / 2, c->mss) || (before == 0 && now_w > 0)) {
            c->flags |= FL_ACK_NOW;
            if (t->budget <= 0) t->budget = 1;
            conn_output(t, c, 1);
        }
    }
    return (int)n;
}

int sdtcp_close(struct sdtcp *t, int h)
{
    struct sdtcp_conn *c = get(t, h);

    if (!c) return SDTCP_E_PARAM;
    switch (c->state) {
    case SDTCP_CLOSED: case SDTCP_LISTEN_ST: case SDTCP_SYN_SENT:
        conn_reset_state(c, c->err);
        return 0;
    case SDTCP_SYN_RCVD: case SDTCP_ESTABLISHED:
        c->flags |= FL_FIN_QUEUED;
        c->state = SDTCP_FIN_WAIT_1;
        break;
    case SDTCP_CLOSE_WAIT:
        c->flags |= FL_FIN_QUEUED;
        c->state = SDTCP_LAST_ACK;
        break;
    default:
        return 0;
    }
    if (t->budget <= 0) t->budget = 1;
    conn_output(t, c, 1);
    return 0;
}

void sdtcp_abort(struct sdtcp *t, int h)
{
    struct sdtcp_conn *c = get(t, h);
    if (!c || c->state == SDTCP_CLOSED) return;
    if (c->state != SDTCP_LISTEN_ST && c->state != SDTCP_SYN_SENT) {
        if (t->budget <= 0) t->budget = 1;
        seg_send(t, c, c->snd_nxt, (sdtcp_u8)(TH_RST | TH_ACK), 0, 0);
        t->st.rst_tx++;
    }
    conn_reset_state(c, SDTCP_E_RESET);
}

void sdtcp_set_keepalive(struct sdtcp *t, int h, sdtcp_u32 idle_ms)
{
    struct sdtcp_conn *c = get(t, h);
    if (!c) return;
    c->keep_ms = idle_ms;
    if (idle_ms) { c->flags |= FL_KEEP; c->t_keep = t->net->now + idle_ms; c->keep_cnt = 0; }
    else c->flags &= (sdtcp_u16)~FL_KEEP;
}

void sdtcp_set_mode(struct sdtcp *t, int mode)
{
    int i;
    sdtcp_u32 d;

    if (mode == t->mode) return;
    if (mode == SDTCP_PAUSED) {
        t->pause_t0 = t->net->now;
        t->mode = SDTCP_PAUSED;
        return;
    }
    if (t->mode == SDTCP_PAUSED) {
        d = t->net->now - t->pause_t0;
        for (i = 0; i < t->nconn; i++) {
            struct sdtcp_conn *c = &t->conn[i];
            c->t_rto += d; c->t_ack += d; c->t_persist += d; c->t_keep += d; c->t_tw += d;
            c->t_last_rx += d; c->t_last_tx += d;
            if (c->state != SDTCP_CLOSED && c->state != SDTCP_LISTEN_ST) c->flags |= FL_ACK_NOW;   /* re-announce the window */
        }
    }
    t->mode = (sdtcp_u8)mode;
    if (mode == SDTCP_QUIESCE) {
        for (i = 0; i < t->nconn; i++) {
            if (t->conn[i].state != SDTCP_CLOSED && t->conn[i].state != SDTCP_LISTEN_ST) t->conn[i].flags |= FL_ACK_NOW;
        }
    }
}

/* ---- receive ---- */
static void parse_opts(struct sdtcp_conn *c, const sdtcp_u8 *o, sdtcp_u32 n)
{
    sdtcp_u32 i = 0, l;
    sdtcp_u32 peer_mss = 536;
    int got = 0;

    while (i < n) {
        if (o[i] == 0) break;
        if (o[i] == 1) { i++; continue; }
        if (i + 1 >= n) break;
        l = o[i + 1];
        if (l < 2 || i + l > n) break;
        if (o[i] == 2 && l == 4) { peer_mss = rd16(o + i + 2); got = 1; }
        else if (o[i] == 4 && l == 2) c->flags |= FL_SACK_OK;
        i += l;
    }
    if (!got) peer_mss = 536;
    if (peer_mss < 88) peer_mss = 88;
    if (peer_mss < c->mss) c->mss = (sdtcp_u16)peer_mss;
}

static void enter_established(struct sdtcp *t, struct sdtcp_conn *c, sdtcp_u32 win, sdtcp_u32 seq, sdtcp_u32 ack)
{
    c->state = SDTCP_ESTABLISHED;
    c->snd_wnd = win;
    c->snd_wl1 = seq;
    c->snd_wl2 = ack;
    c->rd_seq = c->rcv_nxt;
    c->cwnd = init_window(c);
    c->retries = 0;
    c->flags &= (sdtcp_u16)~FL_TW;
    c->t_last_rx = t->net->now;
}

static void do_ack_processing(struct sdtcp *t, struct sdtcp_conn *c, sdtcp_u32 seq, sdtcp_u32 ack, sdtcp_u32 win, sdtcp_u32 dlen, int has_fin_syn)
{
    sdtcp_u32 acked, flight, fin_seq;

    if (SEQ_GT(ack, c->snd_max)) {                 /* acks something never sent */
        c->flags |= FL_ACK_NOW;
        return;
    }
    if (SEQ_LEQ(ack, c->snd_una)) {
        if (ack == c->snd_una && dlen == 0 && !has_fin_syn && c->snd_una != c->snd_max && win == c->snd_wnd) {
            t->st.dup_acks_rx++;
            c->dupacks++;
            if (c->dupacks == 3 && !(c->flags & FL_RECOVERY) && SEQ_GEQ(c->snd_una, c->recover)) {
                flight = c->snd_max - c->snd_una;
                c->ssthresh = umax(flight / 2, 2u * c->mss);
                c->recover = c->snd_max;
                c->flags |= FL_RECOVERY;
                c->cwnd = c->ssthresh + 3u * c->mss;
                /* fast retransmit of the first unacked segment */
                {
                    sdtcp_u32 save = c->snd_nxt, ln = umin(umin(c->mss, c->wr_seq - c->snd_una), save - c->snd_una);
                    if (c->snd_una == c->iss) ln = 0;
                    if (ln && t->budget > 0 && seg_send(t, c, c->snd_una, TH_ACK, ln, 0) == 0) {
                        t->budget--;
                        t->st.retrans++;
                        t->st.fast_retrans++;
                        c->flags &= (sdtcp_u16)~FL_RTT_ON;
                    }
                }
                rto_arm(t, c);
            } else if ((c->flags & FL_RECOVERY) && c->dupacks > 3) {
                c->cwnd += c->mss;
            }
        }
    } else {
        acked = ack - c->snd_una;
        fin_seq = c->wr_seq;
        if ((c->flags & FL_FIN_QUEUED) && SEQ_GT(ack, fin_seq)) acked--;       /* FIN unit */
        if (c->snd_una == c->iss && acked) acked--;                             /* SYN unit */
        c->snd_una = ack;
        if (SEQ_LT(c->snd_nxt, c->snd_una)) c->snd_nxt = c->snd_una;
        if ((c->flags & FL_RTT_ON) && SEQ_GEQ(ack, c->rtt_seq)) {
            if (!(c->flags & FL_RETX_SENT)) rto_update(c, t->net->now - c->rtt_t0);
            c->flags &= (sdtcp_u16)~FL_RTT_ON;
        }
        c->retries = 0;
        c->flags &= (sdtcp_u16)~FL_PERSIST;
        if (c->flags & FL_RECOVERY) {
            if (SEQ_GEQ(ack, c->recover)) {                 /* full ack: leave recovery */
                c->flags &= (sdtcp_u16)~FL_RECOVERY;
                c->cwnd = umin(c->ssthresh, umax(c->snd_max - c->snd_una, c->mss) + c->mss);
                c->dupacks = 0;
            } else {                                         /* partial ack: retransmit the next hole, deflate */
                sdtcp_u32 ln = umin(umin(c->mss, c->wr_seq - c->snd_una), c->snd_max - c->snd_una);
                if (c->cwnd > acked) c->cwnd -= acked;
                c->cwnd += c->mss;
                if (ln && t->budget > 0 && seg_send(t, c, c->snd_una, TH_ACK, ln, 0) == 0) {
                    t->budget--;
                    t->st.retrans++;
                    c->flags &= (sdtcp_u16)~FL_RTT_ON;
                }
                c->dupacks = 0;
            }
        } else {
            c->dupacks = 0;
            if (c->cwnd < c->ssthresh) c->cwnd += umin(acked, c->mss);       /* slow start with ABC */
            else if (c->cwnd) c->cwnd += umax(1u, (c->mss * c->mss) / c->cwnd);   /* congestion avoidance */
            if (c->cwnd > 0x10000u * 4u) c->cwnd = 0x10000u * 4u;
        }
        if (c->snd_una == c->snd_max) rto_clear(c);
        else rto_arm(t, c);
    }
    /* window update (RFC 793) */
    if (SEQ_LT(c->snd_wl1, seq) || (c->snd_wl1 == seq && SEQ_LEQ(c->snd_wl2, ack))) {
        c->snd_wnd = win;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
    }
}

static void note_ack_of_fin(struct sdtcp *t, struct sdtcp_conn *c)
{
    if (!(c->flags & FL_FIN_QUEUED) || !SEQ_GT(c->snd_una, c->wr_seq)) return;
    if (c->state == SDTCP_FIN_WAIT_1) {
        c->state = SDTCP_FIN_WAIT_2;
        c->t_tw = t->net->now + SDTCP_FW2_MS;
        c->flags |= FL_TW;
    } else if (c->state == SDTCP_CLOSING) {
        c->state = SDTCP_TIME_WAIT;
        c->t_tw = t->net->now + SDTCP_TW_MS;
        c->flags |= FL_TW;
        rto_clear(c);
    } else if (c->state == SDTCP_LAST_ACK) {
        conn_reset_state(c, 0);
    }
}

static void conn_input(struct sdtcp *t, struct sdtcp_conn *c, sdtcp_u32 seq, sdtcp_u32 ack, sdtcp_u8 fl, sdtcp_u32 win,
                       const sdtcp_u8 *opts, sdtcp_u32 optlen, const sdtcp_u8 *data, sdtcp_u32 dlen)
{
    sdtcp_u32 cap, seglen, size, skip, fin_seq, i;
    int ok;

    c->t_last_rx = t->net->now;
    c->keep_cnt = 0;
    if (c->flags & FL_KEEP) c->t_keep = t->net->now + c->keep_ms;

    if (c->state == SDTCP_SYN_SENT) {
        if (fl & TH_ACK) {
            if (ack != c->iss + 1) {
                if (!(fl & TH_RST)) send_rst_raw(t, c->rip, c->lport, c->rport, ack, 0, 0);
                return;
            }
        }
        if (fl & TH_RST) {
            if (fl & TH_ACK) { conn_reset_state(c, SDTCP_E_REFUSED); }
            return;
        }
        if ((fl & TH_SYN) && (fl & TH_ACK)) {
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            parse_opts(c, opts, optlen);
            c->snd_una = ack;
            if (!(c->flags & FL_RETX_SENT)) rto_update(c, t->net->now - c->t_last_tx);
            c->flags &= (sdtcp_u16)~FL_RETX_SENT;
            rto_clear(c);
            c->retries = 0;
            enter_established(t, c, win, seq, ack);
            c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
            conn_output(t, c, 1);
        }
        return;
    }

    size = c->rxmask + 1;
    {
        sdtcp_u32 used = (c->rcv_nxt - ((c->flags & FL_FIN_RCVD) ? 1u : 0u)) - c->rd_seq;
        cap = size - used;
    }
    seglen = dlen + ((fl & TH_SYN) ? 1u : 0u) + ((fl & TH_FIN) ? 1u : 0u);
    if (seglen == 0) ok = (cap == 0) ? (seq == c->rcv_nxt) : (SEQ_GEQ(seq, c->rcv_nxt) && SEQ_LT(seq, c->rcv_nxt + cap));
    else if (cap == 0) ok = 0;
    else ok = (SEQ_GEQ(seq, c->rcv_nxt) && SEQ_LT(seq, c->rcv_nxt + cap)) ||
              (SEQ_GEQ(seq + seglen - 1, c->rcv_nxt) && SEQ_LT(seq + seglen - 1, c->rcv_nxt + cap)) ||
              (SEQ_LT(seq, c->rcv_nxt) && SEQ_GEQ(seq + seglen - 1, c->rcv_nxt));
    if (c->state == SDTCP_SYN_RCVD && (fl & TH_SYN) && seq == c->irs) {          /* peer retransmitted its SYN */
        c->snd_nxt = c->iss;                                                      /* resend SYN|ACK */
        conn_output(t, c, 1);
        return;
    }
    if (!ok) {
        if (!(fl & TH_RST)) c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
        return;
    }
    if (fl & TH_RST) {
        if (seq == c->rcv_nxt) {
            conn_reset_state(c, SDTCP_E_RESET);
            t->st.rst_rx++;
        } else {
            t->st.challenge_acks++;
            c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
        }
        return;
    }
    if (fl & TH_SYN) {                                                            /* RFC 5961: challenge */
        t->st.challenge_acks++;
        c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
        return;
    }
    if (!(fl & TH_ACK)) return;

    if (c->state == SDTCP_SYN_RCVD) {
        if (SEQ_LT(c->snd_una, ack) && SEQ_LEQ(ack, c->snd_nxt)) {
            c->snd_una = ack;
            rto_clear(c);
            enter_established(t, c, win, seq, ack);
            c->flags |= FL_NEWCHILD;
        } else {
            send_rst_raw(t, c->rip, c->lport, c->rport, ack, 0, 0);
            return;
        }
    }
    do_ack_processing(t, c, seq, ack, win, dlen, (fl & (TH_SYN | TH_FIN)) != 0);
    note_ack_of_fin(t, c);
    if (c->state == SDTCP_CLOSED) return;

    /* data */
    if (dlen && (c->state == SDTCP_ESTABLISHED || c->state == SDTCP_FIN_WAIT_1 || c->state == SDTCP_FIN_WAIT_2)) {
        sdtcp_u32 dseq = seq, n = dlen;
        const sdtcp_u8 *d = data;
        int fin_ok = (fl & TH_FIN) != 0;

        if (SEQ_LT(dseq, c->rcv_nxt)) {
            skip = c->rcv_nxt - dseq;
            if (skip >= n) { n = 0; }
            else { d += skip; n -= skip; }
            dseq = c->rcv_nxt;
        }
        if (n && SEQ_GT(dseq + n, c->rcv_nxt + cap)) {                            /* beyond the ring */
            n = (c->rcv_nxt + cap) - dseq;
            fin_ok = 0;
        }
        fin_seq = seq + dlen;
        if (n) {
            if (dseq == c->rcv_nxt) {
                ring_put(c->rxbuf, c->rxmask, dseq, d, n);
                c->rcv_nxt += n;
                t->st.data_rx_bytes += n;
                c->unacked_bytes += n;
                if (c->ooo_lo != c->ooo_hi && SEQ_GEQ(c->rcv_nxt, c->ooo_lo)) {
                    if (SEQ_GT(c->ooo_hi, c->rcv_nxt)) {
                        t->st.data_rx_bytes += c->ooo_hi - c->rcv_nxt;
                        c->rcv_nxt = c->ooo_hi;
                    }
                    c->ooo_lo = c->ooo_hi = 0;
                    c->flags |= FL_ACK_NOW | FL_ACK_URGENT;                 /* a hole was filled: ACK at once */
                }
                if (c->ooo_lo != c->ooo_hi) c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
            } else {                                         /* out of order: keep one interval */
                if (c->ooo_lo == c->ooo_hi) {
                    c->ooo_lo = dseq; c->ooo_hi = dseq + n;
                    ring_put(c->rxbuf, c->rxmask, dseq, d, n);
                    t->st.ooo_stored++;
                } else if (SEQ_LEQ(dseq, c->ooo_hi) && SEQ_GEQ(dseq + n, c->ooo_lo)) {
                    if (SEQ_LT(dseq, c->ooo_lo)) c->ooo_lo = dseq;
                    if (SEQ_GT(dseq + n, c->ooo_hi)) c->ooo_hi = dseq + n;
                    ring_put(c->rxbuf, c->rxmask, dseq, d, n);
                    t->st.ooo_stored++;
                } else {
                    t->st.ooo_dropped++;
                }
                c->flags |= FL_ACK_NOW | FL_ACK_URGENT;                     /* duplicate ACK (+SACK) */
            }
            if (!(c->flags & FL_ACK_NOW)) {
                if (c->unacked_bytes >= 2u * c->mss) c->flags |= FL_ACK_NOW;
                else if (!(c->flags & FL_DELACK)) { c->flags |= FL_DELACK; c->t_ack = t->net->now + SDTCP_DELACK_MS; }
            }
        } else {
            c->flags |= FL_ACK_NOW | FL_ACK_URGENT;                         /* pure duplicate data */
        }
        if (fin_ok && c->rcv_nxt == fin_seq) {
            fl |= TH_FIN;
        } else {
            fl &= (sdtcp_u8)~TH_FIN;
        }
    } else if (dlen) {
        c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
    }
    if ((fl & TH_FIN) && !(c->flags & FL_FIN_RCVD)) {
        sdtcp_u32 fseq = seq + dlen;
        if (c->rcv_nxt == fseq && (c->state == SDTCP_ESTABLISHED || c->state == SDTCP_FIN_WAIT_1 || c->state == SDTCP_FIN_WAIT_2)) {
            c->rcv_nxt++;
            c->flags |= FL_FIN_RCVD | FL_ACK_NOW | FL_ACK_URGENT;
            if (c->state == SDTCP_ESTABLISHED) c->state = SDTCP_CLOSE_WAIT;
            else if (c->state == SDTCP_FIN_WAIT_1) c->state = SDTCP_CLOSING;
            else {
                c->state = SDTCP_TIME_WAIT;
                c->t_tw = t->net->now + SDTCP_TW_MS;
                c->flags |= FL_TW;
                rto_clear(c);
            }
            note_ack_of_fin(t, c);        /* FIN+ACK of our FIN in one segment: CLOSING -> TIME_WAIT */
            if (c->state == SDTCP_CLOSING && (c->flags & FL_FIN_QUEUED) && SEQ_GT(c->snd_una, c->wr_seq)) {
                c->state = SDTCP_TIME_WAIT;
                c->t_tw = t->net->now + SDTCP_TW_MS;
                c->flags |= FL_TW;
            }
        } else if (c->rcv_nxt != fseq) {
            c->flags |= FL_ACK_NOW | FL_ACK_URGENT;
        }
    }
    (void)i;
    conn_output(t, c, 0);
}

void sdtcp_rx(struct sdtcp *t, sdtcp_u32 src, sdtcp_u32 dst, const sdtcp_u8 *seg, sdtcp_u32 len)
{
    struct sdnet *s = t->net;
    sdtcp_u32 off, seq, ack, dlen, sum, win;
    sdtcp_u16 sport, dport;
    sdtcp_u8 ph[12], fl;
    int i;
    struct sdtcp_conn *c = 0;

    if (t->mode == SDTCP_PAUSED) { t->st.paused_drop++; return; }
    if (len < 20) { t->st.bad_hdr++; return; }
    off = (sdtcp_u32)(seg[12] >> 4) * 4u;
    if (off < 20 || off > len) { t->st.bad_hdr++; return; }
    wr32(ph, src); wr32(ph + 4, dst); ph[8] = 0; ph[9] = 6; wr16(ph + 10, (sdtcp_u16)len);
    sum = sdnet_csum_add(0, ph, 12);
    sum = sdnet_csum_add(sum, seg, len);
    if (sdnet_csum_fold(sum) != 0) { t->st.bad_csum++; return; }
    t->st.seg_rx++;
    sport = rd16(seg); dport = rd16(seg + 2);
    seq = rd32(seg + 4); ack = rd32(seg + 8);
    fl = (sdtcp_u8)(seg[13] & 0x3f);
    win = rd16(seg + 14);
    dlen = len - off;

    for (i = 0; i < t->nconn; i++) {
        struct sdtcp_conn *k = &t->conn[i];
        if (k->state != SDTCP_CLOSED && k->state != SDTCP_LISTEN_ST && k->lport == dport && k->rport == sport && k->rip == src) {
            c = k;
            break;
        }
    }
    if (c) {
        if (c->state == SDTCP_TIME_WAIT) {
            if (!(fl & TH_RST)) { c->flags |= FL_ACK_NOW; conn_output(t, c, 1); }
            return;
        }
        conn_input(t, c, seq, ack, fl, win, seg + 20, off - 20, seg + off, dlen);
        return;
    }
#if SDTCP_LISTEN
    if ((fl & TH_SYN) && !(fl & (TH_ACK | TH_RST))) {
        for (i = 0; i < t->nconn; i++) {
            if (t->conn[i].state == SDTCP_LISTEN_ST && t->conn[i].lport == dport) {
                int hh = -1;
                struct sdtcp_conn *k = alloc_conn(t, &hh);
                if (!k) return;                      /* backlog full: drop, the peer retries */
                conn_start(t, k, src, sport, dport);
                k->state = SDTCP_SYN_RCVD;
                k->flags |= FL_TW;
                k->t_tw = t->net->now + SDTCP_CONNECT_MS;
                k->irs = seq;
                k->rcv_nxt = seq + 1;
                k->rd_seq = k->rcv_nxt;
                parse_opts(k, seg + 20, off - 20);
                k->snd_wnd = win;
                if (t->budget <= 0) t->budget = 1;
                conn_output(t, k, 1);
                return;
            }
        }
    }
#endif
    t->st.no_conn++;
    if (!(fl & TH_RST)) {
        if (fl & TH_ACK) send_rst_raw(t, src, dport, sport, ack, 0, 0);
        else send_rst_raw(t, src, dport, sport, 0, seq + dlen + ((fl & TH_SYN) ? 1u : 0u) + ((fl & TH_FIN) ? 1u : 0u), 1);
    }
    (void)s;
}

/* ---- poll: timers, then output ---- */
static void conn_fail(struct sdtcp *t, struct sdtcp_conn *c, int err)
{
    if (c->state != SDTCP_SYN_SENT && c->state != SDTCP_LISTEN_ST) {
        seg_send(t, c, c->snd_nxt, (sdtcp_u8)(TH_RST | TH_ACK), 0, 0);
        t->st.rst_tx++;
    }
    conn_reset_state(c, err);
}

void sdtcp_poll(struct sdtcp *t)
{
    sdtcp_u32 now = t->net->now, flight;
    int i;

    t->budget = SDTCP_TX_BUDGET;
    if (t->mode == SDTCP_PAUSED) return;
    for (i = 0; i < t->nconn; i++) {
        struct sdtcp_conn *c = &t->conn[i];

        if (c->state == SDTCP_CLOSED || c->state == SDTCP_LISTEN_ST) continue;
        if ((c->flags & FL_TW) && due(now, c->t_tw)) {
            conn_reset_state(c, (c->state == SDTCP_SYN_SENT || c->state == SDTCP_SYN_RCVD) ? SDTCP_E_TIMEOUT : 0);
            continue;
        }
        if ((c->flags & FL_RTO) && due(now, c->t_rto)) {
            t->st.rto_fires++;
            c->retries++;
            if (c->state == SDTCP_SYN_SENT || c->state == SDTCP_SYN_RCVD) {
                if (c->retries >= SDTCP_SYN_TRIES) { conn_reset_state(c, SDTCP_E_TIMEOUT); continue; }
                c->rto = umin(c->rto * 2u, SDTCP_RTO_MAX_MS);
                c->snd_nxt = c->iss;
                c->flags |= FL_RETX_SENT;
                c->flags &= (sdtcp_u16)~FL_RTO;
                if (t->budget <= 0) t->budget = 1;
                conn_output(t, c, 1);
                if (!(c->flags & FL_RTO)) rto_arm(t, c);
                continue;
            }
            if (c->retries >= SDTCP_DATA_TRIES) { conn_fail(t, c, SDTCP_E_TIMEOUT); continue; }
            flight = c->snd_max - c->snd_una;
            c->ssthresh = umax(flight / 2, 2u * c->mss);
            c->cwnd = c->mss;
            c->dupacks = 0;
            c->flags &= (sdtcp_u16)~FL_RECOVERY;
            c->recover = c->snd_max;
            c->rto = umin(c->rto * 2u, SDTCP_RTO_MAX_MS);
            c->flags &= (sdtcp_u16)~FL_RTT_ON;
            c->flags |= FL_RETX_SENT;
            c->snd_nxt = c->snd_una;
            rto_arm(t, c);
            if (t->budget <= 0) t->budget = 1;
            conn_output(t, c, 1);
        }
        if ((c->flags & FL_PERSIST) && due(now, c->t_persist)) {
            if (c->snd_wnd == 0 && seg_send(t, c, c->snd_una - 1u, TH_ACK, 0, 0) == 0) { /* probe: peer answers with its window */
                t->budget--;
                c->rto = umin(c->rto * 2u, SDTCP_RTO_MAX_MS);
            }
            c->t_persist = now + c->rto;
        }
        if ((c->flags & FL_KEEP) && due(now, c->t_keep) && in_data_state(c)) {
            if (c->keep_cnt >= SDTCP_KEEP_PROBES) { conn_fail(t, c, SDTCP_E_TIMEOUT); continue; }
            c->keep_cnt++;
            seg_send(t, c, c->snd_nxt - 1u, TH_ACK, 0, 0);
            c->t_keep = now + (c->keep_ms > 10000u ? 5000u : c->keep_ms);
        }
        if ((c->flags & FL_DELACK) && due(now, c->t_ack)) c->flags |= FL_ACK_NOW;
        if (c->state == SDTCP_CLOSING || c->state == SDTCP_LAST_ACK) {
            if (!(c->flags & FL_TW)) { c->flags |= FL_TW; c->t_tw = now + SDTCP_FW2_MS * 3u; }
        }
        conn_output(t, c, 1);
    }
}

#endif /* SDNET_ENABLE_TCP */
