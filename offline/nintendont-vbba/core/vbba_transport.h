/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_transport: the datagram seam below vbba_router.  A platform that cannot move raw Ethernet frames but can send
 * and receive UDP datagrams (Wii IOS sockets under Nintendont, a socket-style ESP32 side channel behind a FlippyDrive,
 * host sockets in tests) implements this; the router turns the guest's Ethernet frames into datagrams and back.
 *
 * Rules: nothing blocks; every call returns at once.  Handles are small integers chosen by the transport.  An open may
 * complete later (state reports OPENING until then).  All addresses are IPv4 host order, ports host order.
 * Implementations: platform/host/vbba_tp_host.c (real loopback sockets), platform/nintendont/vbba_tp_ios.c (IOS
 * /dev/net/ip/top through vbba_ios.h hooks), platform/cubeboot/vbba_tp_cubeboot_stub.c (documented stub).
 */
#ifndef VBBA_TRANSPORT_H
#define VBBA_TRANSPORT_H

#include "vbba_types.h"

enum { VBBA_TP_FREE = 0, VBBA_TP_OPENING, VBBA_TP_OPEN, VBBA_TP_FAILED };

struct vbba_tp_status {
    vbba_u8 up;               /* the platform network is usable */
    vbba_u32 ip, mask, gw;    /* the platform's own address (0 while unknown; mask/gw 0 = unknown) */
};

struct vbba_transport {
    void *ctx;
    vbba_u32 (*now_ms)(void *ctx);                                       /* wrapping millisecond clock */
    void (*status)(void *ctx, struct vbba_tp_status *out);
    void (*poll)(void *ctx);                                             /* drive completions; may be NULL */
    int (*open)(void *ctx, vbba_u16 local_port);                         /* handle >= 0, or < 0 (no slot) */
    int (*state)(void *ctx, int h);                                      /* VBBA_TP_*; NULL = OPEN at once */
    vbba_u16 (*bound_port)(void *ctx, int h);                            /* port peers see (0 unknown); may be NULL */
    int (*send)(void *ctx, int h, vbba_u32 ip, vbba_u16 port, const vbba_u8 *d, vbba_u16 len);   /* VBBA_OK/WOULD_BLOCK/ERROR */
    /* > 0 bytes, 0 nothing, VBBA_TP_TRUNC if the datagram did not fit cap (dropped), < 0 other error */
    int (*recv)(void *ctx, int h, vbba_u8 *buf, vbba_u16 cap, vbba_u32 *ip, vbba_u16 *port);
    void (*close)(void *ctx, int h);
    /* optional platform veto on top of the router's allow-list (outbound 1 / inbound 0); NULL = allow */
    int (*allow)(void *ctx, vbba_u32 ip, vbba_u16 port, int outbound);
    /* 1: send accepts datagrams for a socket that is still OPENING and keeps them (a FIFO) until it is open; the router then does not hold them itself */
    int queues_opening;
    /* optional STREAM (TCP client) sockets for the router's TCP splice (core/vbba_tcp.c, VBBA_TCP); all NULL = no TCP (the guest's SYNs are dropped and counted).
       s_open starts a connect to ip:port (handle >= 0, < 0 no slot); s_state: VBBA_TP_OPENING while connecting, OPEN, FAILED (connect refused / timed out / broken);
       s_send: bytes taken (0 = nothing now, try later), < 0 the socket is dead; s_recv: > 0 bytes, 0 nothing yet, VBBA_TP_EOF the peer closed, < 0 dead; s_close frees the handle. */
    int (*s_open)(void *ctx, vbba_u32 ip, vbba_u16 port);
    int (*s_state)(void *ctx, int h);
    int (*s_send)(void *ctx, int h, const vbba_u8 *d, vbba_u16 len);
    int (*s_recv)(void *ctx, int h, vbba_u8 *buf, vbba_u16 cap);
    void (*s_close)(void *ctx, int h);
    void (*s_shutdown)(void *ctx, int h);                                /* half-close (no more sends; receiving goes on); may be NULL: the router then closes after the peer's EOF or the linger time */
};
#define VBBA_TP_TRUNC (-2)
#define VBBA_TP_EOF (-3)

#endif
