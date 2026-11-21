/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba: a virtual GameCube Broadband Adapter (MX98728 on EXI channel 0 device 2), platform-neutral core.
 * Common types and the frame seam.  C99 subset, no libc, no heap, endian-neutral (byte arrays only), so the same core
 * builds for the Starlet (ARM926, big-endian, Nintendont kernel), for a resident region on the Gekko (Cubeboot) and
 * for host tests.
 */
#ifndef VBBA_TYPES_H
#define VBBA_TYPES_H

typedef unsigned char vbba_u8;
typedef unsigned short vbba_u16;
typedef unsigned int vbba_u32;
typedef int vbba_s32;

/* Result codes.  Values and order are identical to enum cubeboot_bba_result
 so the frame seam maps 1:1. */
enum vbba_result {
    VBBA_OK = 0,
    VBBA_EMPTY,
    VBBA_WOULD_BLOCK,
    VBBA_LINK_DOWN,
    VBBA_UNSUPPORTED,
    VBBA_ERROR
};

#define VBBA_FRAME_MAX 1536u        /* = CUBEBOOT_BBA_FRAME_MAX; Ethernet frames without FCS are at most 1514 B */

/* Frame seam: what sits below the chip model.  Same shape and member order as struct cubeboot_bba_backend, so a
   Cubeboot frame backend (W5500 MACRAW, real BBA passthrough, a future FlippyDrive frame service) plugs in through
   platform/cubeboot/vbba_cubeboot_frames.c, and vbba_router (frames <-> UDP datagrams) is one such backend. */
struct vbba_frame_caps {
    vbba_u8 mac[6];
    vbba_u16 max_tx_frame;
    vbba_u16 max_rx_frame;
    vbba_u8 tx_queue_depth;
    vbba_u8 rx_queue_depth;
    vbba_u8 link_up;
};

struct vbba_frame_backend {
    void *context;
    int (*get_capabilities)(void *context, struct vbba_frame_caps *out);
    /* the guest transmitted a complete frame (no FCS); never blocks */
    int (*submit_tx)(void *context, const vbba_u8 *frame, vbba_u16 length);
    /* next frame for the guest: VBBA_OK + *length, or VBBA_EMPTY; must not write more than capacity */
    int (*receive_rx)(void *context, vbba_u8 *frame, vbba_u16 capacity, vbba_u16 *length);
    int (*poll)(void *context);
    void (*reset)(void *context);
};

#endif
