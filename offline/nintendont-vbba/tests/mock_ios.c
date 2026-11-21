/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* mock_ios: see mock_ios.h. */
#include "mock_ios.h"
#include <poll.h>
#include <errno.h>
#include "../platform/host/vbba_tp_host.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static vbba_u32 m_now(void *c) { struct mock_ios *m = c; return m->mode == MOCK_UDP ? vbba_host_ms() : m->now; }
static vbba_u32 rnd(struct mock_ios *m) { m->rng = m->rng * 1103515245u + 12345u; return m->rng >> 8; }

static struct mock_sock *by_port(struct mock_ios *m, vbba_u16 port)
{
    int i;
    for (i = 0; i < MOCK_FDS; i++) if (m->s[i].used && m->s[i].port == port && port) return &m->s[i];
    return 0;
}
static int push(struct mock_sock *s, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u32 len)
{
    struct mock_dgram *g;
    if (s->cnt >= MOCK_RXQ || len > sizeof g->d) return 0;
    g = &s->q[(s->head + s->cnt) % MOCK_RXQ];
    g->ip = ip; g->port = port; g->len = (vbba_u16)len; memcpy(g->d, d, len);
    s->cnt++;
    return 1;
}
int mock_ios_inject(struct mock_ios *m, vbba_u16 dport, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u32 len)
{
    struct mock_sock *s = by_port(m, dport);
    return s ? push(s, ip, port, d, len) : 0;
}

static int m_submit(void *c, struct vbba_ios_req *r)
{
    struct mock_ios *m = c;
    if (r->op == VBBA_IOS_SENDTO && m->refuse_send > 0) { m->refuse_send--; m->refused++; return -1; }
    if (m->npend >= MOCK_PEND) { m->refused++; return -1; }
    m->pend[m->npend] = r;
    m->sub[m->npend] = m_now(m);
    m->due[m->npend] = m_now(m) + m->latency_ms + (r->op == VBBA_IOS_SENDTO ? m->send_latency_ms : 0) + (r->op == VBBA_IOS_STARTUP ? m->startup_latency_ms : 0);
    m->npend++;
    m->submitted++;
    if (r->op == VBBA_IOS_RECVFROM) m->recv_submitted++;
    { int i; unsigned c = 0; for (i = 0; i < m->npend; i++) if (m->pend[i]->op == VBBA_IOS_SENDTO || m->pend[i]->op == VBBA_IOS_POLL || m->pend[i]->op == VBBA_IOS_RECVFROM) c++; if (c > m->net_pend_max) m->net_pend_max = c; }
    return 0;
}

/* returns 1 if the request completed now */
static int run(struct mock_ios *m, struct vbba_ios_req *r)
{
    struct mock_sock *s = (r->fd >= 0 && r->fd < MOCK_FDS) ? &m->s[r->fd] : 0;
    int i;
    switch (r->op) {
    case VBBA_IOS_STARTUP: r->result = m->fail_startup ? -1 : 0; return 1;
    case VBBA_IOS_GETIP: r->result = 0; r->ip = m_now(m) >= m->dhcp_ms ? m->ip : 0; return 1;
    case VBBA_IOS_SOCKET:
        if (m->fail_socket) { r->result = -1; return 1; }
        for (i = 0; i < MOCK_FDS; i++) if (!m->s[i].used) break;
        if (i == MOCK_FDS) { r->result = -1; return 1; }
        memset(&m->s[i], 0, sizeof m->s[i]);
        m->s[i].used = 1; m->s[i].osfd = -1;
        if (m->mode == MOCK_UDP) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) { m->s[i].used = 0; r->result = -1; return 1; }
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            m->s[i].osfd = fd;
        }
        r->result = i;
        return 1;
    case VBBA_IOS_BIND:
        if (!s || !s->used) { r->result = -1; return 1; }
        if (r->port && (r->port == m->port_taken || by_port(m, r->port))) { r->result = -1; return 1; }
        if (m->mode == MOCK_UDP) {
            struct sockaddr_in a; socklen_t l = sizeof a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7F000001u); a.sin_port = htons(r->port);
            if (bind(s->osfd, (struct sockaddr *)&a, sizeof a) < 0) { r->result = -1; return 1; }
            getsockname(s->osfd, (struct sockaddr *)&a, &l);
            s->port = ntohs(a.sin_port);
        } else {
            s->port = r->port ? r->port : (vbba_u16)(49152 + (rnd(m) % 1000));
        }
        r->result = 0;
        return 1;
    case VBBA_IOS_SENDTO:
        if (!s || !s->used) { r->result = -1; return 1; }
        memcpy(m->last_tx, r->buf, r->len < sizeof m->last_tx ? r->len : sizeof m->last_tx);
        m->last_tx_len = r->len; m->last_tx_ip = r->ip; m->last_tx_port = r->port;
        if (m->mode == MOCK_UDP) {
            struct sockaddr_in a;
            if ((r->ip >> 24) != 127u) { r->result = -1; m->refused++; return 1; }
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(r->ip); a.sin_port = htons(r->port);
            r->result = (vbba_s32)sendto(s->osfd, r->buf, r->len, 0, (struct sockaddr *)&a, sizeof a);
        } else {
            r->result = (vbba_s32)r->len;
            if (m->tx_hook) m->tx_hook(r->ip, r->port, r->buf, r->len);
            if (m->drop_first > 0 && r->ip == m->echo_ip && r->port == 7) { m->drop_first--; m->lost++; }
            else if (m->loss_pct && (int)(rnd(m) % 100) < m->loss_pct) { m->lost++; }
            else if (m->echo_ip && r->ip == m->echo_ip && r->port == 7) {
                int k;
                for (k = 0; k <= m->echo_copies; k++) {
                    unsigned char tmp[2048]; const vbba_u8 *d = r->buf;
                    if (m->echo_stamp && r->len == 64 && d[2] == 'B' && d[3] == 'R' && d[15] == 1) {      /* what the project's other tooling does */
                        vbba_u32 now = m_now(m), us;
                        memcpy(tmp, d, 64);
                        if (!m->stamp_cnt || now - m->stamp_last_ms > 3000) { m->stamp_cnt = 0; m->stamp_first_ms = now; }
                        m->stamp_last_ms = now; m->stamp_cnt++;
                        us = (now - m->stamp_first_ms) * 1000u;
                        tmp[8] = (unsigned char)(m->stamp_cnt >> 8); tmp[9] = (unsigned char)m->stamp_cnt;
                        tmp[10] = (unsigned char)(us >> 24); tmp[11] = (unsigned char)(us >> 16); tmp[12] = (unsigned char)(us >> 8); tmp[13] = (unsigned char)us;
                        d = tmp;
                    }
                    if (push(s, r->ip, 7, d, r->len)) m->echoed++;
                }
            }
            else if (r->ip == m->ip) { struct mock_sock *d = by_port(m, r->port); if (d) push(d, m->ip, s->port, r->buf, r->len); }
        }
        m->sent++;
        return 1;
    case VBBA_IOS_RECVFROM: {
        struct mock_dgram *g;
        if (!s || !s->used) { r->result = -1; return 1; }
        if (m->mode == MOCK_UDP) {
            unsigned char tmp[2048];
            struct sockaddr_in a; socklen_t l = sizeof a;
            ssize_t n = recvfrom(s->osfd, tmp, sizeof tmp, 0, (struct sockaddr *)&a, &l);
            if (n < 0) { if (s->nb) { r->result = -11; return 1; } return 0; }
            push(s, ntohl(a.sin_addr.s_addr), ntohs(a.sin_port), tmp, (vbba_u32)n);
        }
        if (!s->cnt) { if (s->nb) { r->result = -11; return 1; } return 0; }
        g = &s->q[s->head];
        s->head = (s->head + 1) % MOCK_RXQ; s->cnt--;
        r->ip = g->ip; r->port = g->port;
        memcpy(r->buf, g->d, g->len < r->len ? g->len : r->len);
        r->result = g->len;                         /* > len: truncated, as recvfrom reports it */
        return 1;
    }
    case VBBA_IOS_NONBLOCK: if (s && s->used) s->nb = 1; r->result = 0; return 1;
    case VBBA_IOS_SETTOS: if (s && s->used) s->tos = r->len; r->result = (s && s->used) ? 0 : -1; return 1;   /* */
    case VBBA_IOS_POLL:                             /* poll(POLLIN, timeout 0): 1 when a datagram can be read now */
        if (!s || !s->used) { r->result = -1; return 1; }
        if (m->mode == MOCK_UDP) {
            unsigned char tmp[2048];
            struct sockaddr_in a; socklen_t l = sizeof a;
            ssize_t n = recvfrom(s->osfd, tmp, sizeof tmp, 0, (struct sockaddr *)&a, &l);
            if (n >= 0) push(s, ntohl(a.sin_addr.s_addr), ntohs(a.sin_port), tmp, (vbba_u32)n);
        }
        m->polls++;
        r->result = m->fail_poll ? -1 : s->cnt > 0 ? 1 : 0;
        return 1;
    case VBBA_IOS_CLOSE:
        if (s && s->used) {
            for (i = 0; i < m->npend; i++)          /* fail the socket's outstanding receives */
                if (m->pend[i]->op == VBBA_IOS_RECVFROM && m->pend[i]->fd == r->fd) { m->pend[i]->result = -1; m->pend[i]->op = -VBBA_IOS_RECVFROM; m->due[i] = 0; }
            if (s->osfd >= 0) close(s->osfd);
            s->used = 0;
        }
        r->result = 0;
        return 1;
    case -VBBA_IOS_RECVFROM:
        r->op = VBBA_IOS_RECVFROM;                  /* failed by close */
        return 1;
    /* VBBA_TCP: stream sockets (MOCK_UDP mode only: real host TCP, IOS-style results: -26 in progress, -6 would block, IOS poll bits) */
    case VBBA_IOS_TCPSOCKET:
        for (i = 0; i < MOCK_FDS; i++) if (!m->s[i].used) break;
        if (i == MOCK_FDS || m->mode != MOCK_UDP) { r->result = -1; return 1; }
        memset(&m->s[i], 0, sizeof m->s[i]);
        m->s[i].used = 1; m->s[i].osfd = socket(AF_INET, SOCK_STREAM, 0);
        if (m->s[i].osfd < 0) { m->s[i].used = 0; r->result = -1; return 1; }
        m->tcp_ops++;
        r->result = i;
        return 1;
    case VBBA_IOS_CONNECT: {
        struct sockaddr_in a;
        if (!s || !s->used) { r->result = -1; return 1; }
        if ((r->ip >> 24) != 127u) { r->result = -1; m->refused++; return 1; }
        if (s->nb) fcntl(s->osfd, F_SETFL, fcntl(s->osfd, F_GETFL) | O_NONBLOCK);
        memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(r->ip); a.sin_port = htons(r->port);
        m->tcp_ops++;
        r->result = connect(s->osfd, (struct sockaddr *)&a, sizeof a) == 0 ? 0 : (errno == EINPROGRESS ? -26 : -111);
        return 1;
    }
    case VBBA_IOS_TCPPOLL: {
        struct pollfd p; int ev = 0;
        if (!s || !s->used) { r->result = -1; return 1; }
        p.fd = s->osfd; p.events = (short)(((r->len & 3u) ? POLLIN : 0) | ((r->len & 8u) ? POLLOUT : 0)); p.revents = 0;
        m->tcp_ops++;
        if (poll(&p, 1, 0) < 0) { r->result = -1; return 1; }
        if (p.revents & POLLIN) ev |= 1; if (p.revents & POLLOUT) ev |= 8; if (p.revents & POLLERR) ev |= 0x20; if (p.revents & POLLHUP) ev |= 0x40; if (p.revents & POLLNVAL) ev |= 0x80;
        r->result = ev;
        return 1;
    }
    case VBBA_IOS_SEND: {
        ssize_t n;
        if (!s || !s->used) { r->result = -1; return 1; }
        m->tcp_ops++;
        n = send(s->osfd, r->buf, m->tcp_chunk && r->len > m->tcp_chunk ? m->tcp_chunk : r->len, 0);
        r->result = n >= 0 ? (vbba_s32)n : (errno == EAGAIN ? -6 : -54);
        return 1;
    }
    case VBBA_IOS_RECV: {
        ssize_t n;
        if (!s || !s->used) { r->result = -1; return 1; }
        m->tcp_ops++;
        n = recv(s->osfd, r->buf, m->tcp_chunk && r->len > m->tcp_chunk ? m->tcp_chunk : r->len, 0);
        r->result = n >= 0 ? (vbba_s32)n : (errno == EAGAIN ? -6 : -54);
        return 1;
    }
    case VBBA_IOS_SHUTDOWN:
        if (s && s->used) shutdown(s->osfd, SHUT_WR);
        r->result = 0;
        return 1;
    }
    r->result = -1;
    return 1;
}

static int m_poll(void *c, struct vbba_ios_req **done)
{
    struct mock_ios *m = c;
    vbba_u32 t = m_now(m);
    int i;
    if (m->stash_out) { *done = m->stash_out; m->stash_out->busy = 0; m->stash_out = 0; return 1; }
    for (i = 0; i < m->npend; i++) {
        struct vbba_ios_req *r = m->pend[i];
        if ((vbba_s32)(t - m->due[i]) < 0) continue;
        if (!run(m, r)) continue;
        m->npend--;
        if (r->op == VBBA_IOS_RECVFROM) { r->lat_us = (vbba_u32)(t - m->sub[i]) * 1000u; r->dwell_us = 0; }      /* a receive waits for its datagram: the wait is the latency */
        else { r->lat_us = (vbba_u32)(m->due[i] - m->sub[i]) * 1000u; r->dwell_us = (vbba_u32)(t - m->due[i]) * 1000u; }
        { int k; for (k = i; k < m->npend; k++) { m->pend[k] = m->pend[k + 1]; m->due[k] = m->due[k + 1]; m->sub[k] = m->sub[k + 1]; } }     /* keeps the submit order: IOS serves receives first come first served */
        m->completed++;
        if (m->rx_rev && r->op == VBBA_IOS_RECVFROM && r->result > 0) {            /* the first finished receive is held back; when a second one finishes, that one is reported first and the held one right after */
            if (!m->stash) { m->stash = r; i = -1; continue; }
            m->stash_out = m->stash; m->stash = 0;
        }
        r->busy = 0;
        *done = r;
        return 1;
    }
    if (m->stash) { *done = m->stash; m->stash->busy = 0; m->stash = 0; return 1; }       /* no second one: released */
    return 0;
}

void mock_ios_init(struct mock_ios *m, int mode, vbba_u32 ip)
{
    memset(m, 0, sizeof *m);
    m->mode = mode; m->ip = ip; m->rng = 12345u;
    m->ops.ctx = m; m->ops.submit = m_submit; m->ops.poll = m_poll; m->ops.now_ms = m_now;
}

void mock_ios_close_all(struct mock_ios *m)
{
    int i;
    for (i = 0; i < MOCK_FDS; i++) if (m->s[i].used && m->s[i].osfd >= 0) { close(m->s[i].osfd); m->s[i].used = 0; }
}
