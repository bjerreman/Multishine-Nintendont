/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_tp_host: see vbba_tp_host.h.  POSIX sockets, non-blocking, bound to 127.0.0.1. */
#include "vbba_tp_host.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

vbba_u32 vbba_host_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (vbba_u32)((unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull);
}

static vbba_u32 h_now(void *c) { (void)c; return vbba_host_ms(); }
static void h_status(void *c, struct vbba_tp_status *o) { struct vbba_tp_host *h = c; o->up = 1; o->ip = h->ip; o->mask = 0xff000000u; o->gw = 0; }

static int h_open(void *c, vbba_u16 port)
{
    struct vbba_tp_host *h = c;
    struct sockaddr_in a;
    int i, fd;
    for (i = 0; i < VBBA_TP_HOST_SOCKS; i++) if (h->fd[i] < 0) break;
    if (i == VBBA_TP_HOST_SOCKS) return -1;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7F000001u); a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        a.sin_port = 0;                                     /* port taken: ephemeral, like the IOS fallback */
        if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    }
    {
        socklen_t l = sizeof a;
        getsockname(fd, (struct sockaddr *)&a, &l);
        h->bound[i] = ntohs(a.sin_port);
    }
    h->fd[i] = fd;
    return i;
}

static int h_state(void *c, int i) { struct vbba_tp_host *h = c; return (i >= 0 && i < VBBA_TP_HOST_SOCKS && h->fd[i] >= 0) ? VBBA_TP_OPEN : VBBA_TP_FREE; }
static vbba_u16 h_bound(void *c, int i) { struct vbba_tp_host *h = c; return (i >= 0 && i < VBBA_TP_HOST_SOCKS) ? h->bound[i] : 0; }

static int h_send(void *c, int i, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u16 len)
{
    struct vbba_tp_host *h = c;
    struct sockaddr_in a;
    if (i < 0 || i >= VBBA_TP_HOST_SOCKS || h->fd[i] < 0) return VBBA_ERROR;
    if ((ip >> 24) != 127u) { h->refused++; return VBBA_ERROR; }      /* tests never leave the host */
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip); a.sin_port = htons(port);
    if (sendto(h->fd[i], d, len, 0, (struct sockaddr *)&a, sizeof a) < 0) return errno == EAGAIN || errno == ENOBUFS ? VBBA_WOULD_BLOCK : VBBA_ERROR;
    h->tx++;
    return VBBA_OK;
}

static int h_recv(void *c, int i, vbba_u8 *buf, vbba_u16 cap, vbba_u32 *ip, vbba_u16 *port)
{
    struct vbba_tp_host *h = c;
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    unsigned char tmp[2048];
    ssize_t n;
    if (i < 0 || i >= VBBA_TP_HOST_SOCKS || h->fd[i] < 0) return -1;
    n = recvfrom(h->fd[i], tmp, sizeof tmp, 0, (struct sockaddr *)&a, &l);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    *ip = ntohl(a.sin_addr.s_addr); *port = ntohs(a.sin_port);
    if (n > cap) return VBBA_TP_TRUNC;
    memcpy(buf, tmp, (size_t)n);
    h->rx++;
    return (int)n;
}

static void h_close(void *c, int i) { struct vbba_tp_host *h = c; if (i >= 0 && i < VBBA_TP_HOST_SOCKS && h->fd[i] >= 0) { close(h->fd[i]); h->fd[i] = -1; } }

/* ---- stream sockets ---- */
#include <sys/select.h>
static int hs_open(void *c, vbba_u32 ip, vbba_u16 port)
{
    struct vbba_tp_host *h = c;
    struct sockaddr_in a;
    int i, fd;
    if ((ip >> 24) != 127u) { h->refused++; return -1; }
    for (i = 0; i < VBBA_TP_HOST_SOCKS; i++) if (h->sfd[i] < 0) break;
    if (i == VBBA_TP_HOST_SOCKS) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip); a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    h->sfd[i] = fd; h->sst[i] = VBBA_TP_OPENING; h->s_opens++;
    return i;
}
static int hs_state(void *c, int i)
{
    struct vbba_tp_host *h = c;
    if (i < 0 || i >= VBBA_TP_HOST_SOCKS || h->sfd[i] < 0) return VBBA_TP_FREE;
    if (h->sst[i] == VBBA_TP_OPENING) {
        fd_set w; struct timeval tv = { 0, 0 }; int err = 0; socklen_t l = sizeof err;
        FD_ZERO(&w); FD_SET(h->sfd[i], &w);
        if (select(h->sfd[i] + 1, 0, &w, 0, &tv) > 0) {
            getsockopt(h->sfd[i], SOL_SOCKET, SO_ERROR, &err, &l);
            h->sst[i] = err ? VBBA_TP_FAILED : VBBA_TP_OPEN;
        }
    }
    return h->sst[i];
}
static int hs_send(void *c, int i, const vbba_u8 *d, vbba_u16 len)
{
    struct vbba_tp_host *h = c;
    ssize_t n;
    if (i < 0 || i >= VBBA_TP_HOST_SOCKS || h->sfd[i] < 0 || h->sst[i] != VBBA_TP_OPEN) return -1;
    if (h->s_chunk && len > h->s_chunk) len = (vbba_u16)h->s_chunk;
    n = send(h->sfd[i], d, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    h->s_bytes_tx += (unsigned)n;
    return (int)n;
}
static int hs_recv(void *c, int i, vbba_u8 *buf, vbba_u16 cap)
{
    struct vbba_tp_host *h = c;
    ssize_t n;
    if (i < 0 || i >= VBBA_TP_HOST_SOCKS || h->sfd[i] < 0 || h->sst[i] != VBBA_TP_OPEN) return -1;
    if (h->s_chunk && cap > h->s_chunk) cap = (vbba_u16)h->s_chunk;
    n = recv(h->sfd[i], buf, cap, 0);
    if (n == 0) return VBBA_TP_EOF;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    h->s_bytes_rx += (unsigned)n;
    return (int)n;
}
static void hs_close(void *c, int i) { struct vbba_tp_host *h = c; if (i >= 0 && i < VBBA_TP_HOST_SOCKS && h->sfd[i] >= 0) { close(h->sfd[i]); h->sfd[i] = -1; h->sst[i] = VBBA_TP_FREE; } }

static void hs_shutdown(void *c, int i) { struct vbba_tp_host *h = c; if (i >= 0 && i < VBBA_TP_HOST_SOCKS && h->sfd[i] >= 0) shutdown(h->sfd[i], SHUT_WR); }

void vbba_tp_host_init(struct vbba_tp_host *h)
{
    int i;
    memset(h, 0, sizeof *h);
    for (i = 0; i < VBBA_TP_HOST_SOCKS; i++) h->fd[i] = h->sfd[i] = -1;
    h->ip = 0x7F000001u;
    h->tp.ctx = h;
    h->tp.now_ms = h_now; h->tp.status = h_status; h->tp.poll = 0; h->tp.open = h_open; h->tp.state = h_state;
    h->tp.bound_port = h_bound; h->tp.send = h_send; h->tp.recv = h_recv; h->tp.close = h_close; h->tp.allow = 0; h->tp.queues_opening = 0;
    h->tp.s_open = hs_open; h->tp.s_state = hs_state; h->tp.s_send = hs_send; h->tp.s_recv = hs_recv; h->tp.s_close = hs_close; h->tp.s_shutdown = hs_shutdown;
}

void vbba_tp_host_close_all(struct vbba_tp_host *h) { int i; for (i = 0; i < VBBA_TP_HOST_SOCKS; i++) { h_close(h, i); hs_close(h, i); } }
