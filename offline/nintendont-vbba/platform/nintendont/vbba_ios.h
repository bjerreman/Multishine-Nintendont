/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_ios: the asynchronous IOS socket requests the vBBA's IOS transport needs, as intent (the IOS wire encoding lives
 * in the GPL glue inside the Nintendont patch).  The IOS facts are the ones Nintendont's BBA hack uses (kernel/sock.c,
 * sock.h at bbc0a20, cited also by the project's other tooling): /dev/net/kd/request ioctl 6 (NWC24 startup),
 * /dev/net/ip/top ioctl 0x1F startinterface, ioctlv 0x1C getinterfaceopt(0x4003) = own IPv4, 0x0F socket,
 * 0x02 bind, 0x0D sendto, 0x0C recvfrom, 0x03 close; every call as IOS_IoctlAsync/IoctlvAsync with completions on a
 * message queue.  Tests: tests/mock_ios.c (in-memory network or real 127.0.0.1 sockets).
 *
 * Contract: submit never blocks; a request is owned by the IOS layer from submit until poll hands it back
 * (busy cleared); buffers are owned by the caller, 32-byte aligned, valid until completion.
 */
#ifndef VBBA_IOS_H
#define VBBA_IOS_H

#include "../../core/vbba_types.h"

enum vbba_ios_op {
    VBBA_IOS_STARTUP = 1,   /* kd/request 6 + ip/top 0x1F: bring the configured network profile up */
    VBBA_IOS_GETIP,         /* getinterfaceopt(0x4003): result >= 0, ip = own address (0 while DHCP runs) */
    VBBA_IOS_SOCKET,        /* socket(AF_INET, SOCK_DGRAM, 0): result = fd */
    VBBA_IOS_BIND,          /* bind(fd, INADDR_ANY:port): result 0 / < 0 */
    VBBA_IOS_SENDTO,        /* sendto(fd, buf, len, ip:port): result bytes / < 0 */
    VBBA_IOS_RECVFROM,      /* recvfrom(fd, buf, len): result bytes (> len: truncated) / < 0; ip/port = source */
    VBBA_IOS_CLOSE,         /* close(fd); fails outstanding recvfrom */
    VBBA_IOS_NONBLOCK,      /* fcntl(fd, F_SETFL, O_NONBLOCK) (ioctl 0x05 {fd, 4, 4}); only used with VBBA_TP_NONBLOCK / VBBA_TP_RXPOLL; a recvfrom without data then returns < 0 at once */
    VBBA_IOS_POLL,          /* poll({fd, POLLIN}, timeout 0) (ioctl 0x0B, libogc net_poll): result 1 = a datagram can be read now, 0 = none, < 0 error; only used with VBBA_TP_RXPOLL */
    VBBA_IOS_SETTOS,        /* setsockopt(fd, IPPROTO_IP 0, IP_TOS 1, value = len) (ioctl 9 {fd, level, optname, optlen, optval[20]}, libogc net_setsockopt); only when the transport's tos is set */
    /* VBBA_TCP: stream sockets for the router's TCP splice; the glue exempts these from the request watchdog (a slow one never takes the network down) */
    VBBA_IOS_TCPSOCKET,     /* socket(AF_INET, SOCK_STREAM 1, 0) (ioctl 0x0F): result = fd */
    VBBA_IOS_CONNECT,       /* connect(fd, ip:port) (ioctl 0x04 {fd, has_addr 1, sockaddr}, 36 bytes, the bind layout): 0 connected, < 0 refused or in progress (TCPPOLL decides) */
    VBBA_IOS_SEND,          /* send(fd, buf, len) = sendto 0x0D without a destination (has_dest 0, libogc net_send): result bytes / < 0 */
    VBBA_IOS_RECV,          /* recv(fd, buf, len) = recvfrom 0x0C without an address vector (libogc net_recv): result bytes, 0 = the peer closed, < 0 nothing / error */
    VBBA_IOS_TCPPOLL,       /* poll({fd, events = len}, 0) (ioctl 0x0B): result = revents (IOS bits: 0x01/0x02 read, 0x08 write, 0x20 error, 0x40 hang-up, 0x80 invalid) / < 0 */
    VBBA_IOS_SHUTDOWN       /* shutdown(fd, SHUT_WR 1) (ioctl 0x0E {fd, how}): result ignored */
};

struct vbba_ios_req {
    int op;
    vbba_s32 fd;
    vbba_u32 ip;
    vbba_u16 port;
    vbba_u8 *buf;
    vbba_u32 len;
    vbba_s32 result;
    int busy;
    vbba_u32 tag;
    /* filled by the IOS layer when poll hands the request back (0 = not measured): lat_us = submit -> IOS completion (as seen by
       the completion thread), dwell_us = completion -> collected by the thread that owns the transport.  Microseconds. */
    vbba_u32 lat_us, dwell_us;
};

struct vbba_ios_ops {
    void *ctx;
    int (*submit)(void *ctx, struct vbba_ios_req *r);        /* 0 queued, < 0 refused (r not busy) */
    int (*poll)(void *ctx, struct vbba_ios_req **done);      /* 1: *done completed, 0: nothing */
    vbba_u32 (*now_ms)(void *ctx);                           /* HW_TIMER based on the console */
};

#endif
