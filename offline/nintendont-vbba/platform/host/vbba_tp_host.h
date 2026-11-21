/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_tp_host: vbba_transport over real host UDP sockets, 127.0.0.1 only (tests).  Refuses any other destination. */
#ifndef VBBA_TP_HOST_H
#define VBBA_TP_HOST_H

#include "../../core/vbba_transport.h"

#define VBBA_TP_HOST_SOCKS 4

struct vbba_tp_host {
    struct vbba_transport tp;
    int fd[VBBA_TP_HOST_SOCKS];
    vbba_u16 bound[VBBA_TP_HOST_SOCKS];
    vbba_u32 ip;                       /* what status reports as our address */
    unsigned tx, rx, refused;
    /* stream sockets (TCP, 127.0.0.1 only) for the router's TCP splice */
    int sfd[VBBA_TP_HOST_SOCKS];
    vbba_u8 sst[VBBA_TP_HOST_SOCKS];   /* VBBA_TP_* */
    unsigned s_chunk;                  /* test knob: at most this many bytes per s_send / s_recv call (0 = no limit) */
    unsigned s_opens, s_bytes_tx, s_bytes_rx;
};

void vbba_tp_host_init(struct vbba_tp_host *h);
void vbba_tp_host_close_all(struct vbba_tp_host *h);
vbba_u32 vbba_host_ms(void);

#endif
