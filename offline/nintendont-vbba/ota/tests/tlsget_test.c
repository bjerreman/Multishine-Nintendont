/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_tlsget.c: one pinned-TLS GET over a real non-blocking BSD socket (tests/test_tls.py runs it against the update server's TLS listener (not in this repository)).
 *   tlsget_test HOST PORT PINHEX PATH MAXBODY DEADLINE_MS [OUTFILE]    prints "RESULT <code> <name> status N clen N tlserr N alert N hs_ms N" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include "ms_tlsget.h"

static int fd = -1;
static int n_recv(void *c, sdc_u8 *b, sdc_u32 max) { ssize_t n; (void)c; n = recv(fd, b, max, 0); if (n > 0) return (int)n; if (n == 0) return -1; return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1; }
static int n_send(void *c, const sdc_u8 *b, sdc_u32 len) { ssize_t r; (void)c; r = send(fd, b, len, 0); if (r >= 0) return (int)r; return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1; }
static sdc_u32 n_now(void *c) { struct timespec ts; (void)c; clock_gettime(CLOCK_MONOTONIC, &ts); return (sdc_u32)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull); }
static void n_wait(void *c, sdc_u32 ms) { (void)c; usleep(ms * 1000u); }
static void *n_alloc(void *c, sdc_u32 n) { (void)c; return malloc(n ? n : 1); }
static struct sdc_rng rng;
static int rnd(void *c, sdc_u8 *o, sdc_u32 n) { (void)c; if (!sdc_rng_ready(&rng)) return 1; sdc_rng_bytes(&rng, o, n); return 0; }

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    struct mstls_net net;
    struct mstls_result res;
    sdc_u8 pin[32], seed[32];
    unsigned i, v;
    int rc, fl;
    FILE *u;
    if (argc < 7) return 2;
    for (i = 0; i < 32; i++) { if (sscanf(argv[3] + 2 * i, "%2x", &v) != 1) return 2; pin[i] = (sdc_u8)v; }
    u = fopen("/dev/urandom", "rb");
    if (!u || fread(seed, 1, 32, u) != 32) return 2;
    fclose(u);
    sdc_rng_init(&rng);
    sdc_rng_mix(&rng, seed, 32, 128);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)atoi(argv[2])); inet_pton(AF_INET, argv[1], &sa.sin_addr);
    fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) { printf("RESULT %d connect status 0 clen 0 tlserr 0 alert 0 hs_ms 0\n", MSTLS_E_IO); return 0; }
    {   /* wait for the connect to finish (the loader polls net_connect the same way) */
        struct pollfd pf;
        int err = 0;
        socklen_t el = sizeof err;
        pf.fd = fd; pf.events = POLLOUT;
        if (poll(&pf, 1, 3000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) { printf("RESULT %d connect status 0 clen 0 tlserr 0 alert 0 hs_ms 0\n", MSTLS_E_IO); return 0; }
    }
    net.ctx = 0; net.recv = n_recv; net.send = n_send; net.now = n_now; net.wait = n_wait; net.alloc = n_alloc;
    if (getenv("TLSGET_POSTFILE")) {            /* POST that file (text/plain) instead of a GET: the shell's diagnostics upload */
        static sdc_u8 pb[MSTLS_POST_MAX + 64];
        FILE *pf = fopen(getenv("TLSGET_POSTFILE"), "rb");
        size_t pn = pf ? fread(pb, 1, sizeof pb, pf) : 0;
        if (pf) fclose(pf);
        rc = mstls_post(&net, "multishine.link", argv[4], pb, (sdc_u32)pn, pin, 1, rnd, 0, (sdc_u32)strtoul(argv[6], 0, 10), &res);
    } else
    rc = mstls_get(&net, "multishine.link", argv[4], pin, 1, rnd, 0, (sdc_u32)strtoul(argv[5], 0, 10), (sdc_u32)strtoul(argv[6], 0, 10), &res);
    printf("RESULT %d %s status %d clen %lu tlserr %d alert %d hs_ms %lu\n", rc, mstls_err(rc), res.status, (unsigned long)res.clen, res.tls_err, res.alert, (unsigned long)res.hs_ms);
    if (rc == MSTLS_OK && argc > 7) { FILE *o = fopen(argv[7], "wb"); fwrite(res.body, 1, res.clen, o); fclose(o); }
    if (res.body) free(res.body);
    return 0;
}
