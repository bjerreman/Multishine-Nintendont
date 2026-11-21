/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_wl: the command buffer the Wii's Wi-Fi driver (IOS /dev/wl0, a Broadcom WLC command interface) is given, as pure data.
 *
 * IOS_Ioctl number 1 on /dev/wl0 carries one WLC command, but its input is an "iobuf" (one node), not a flat buffer:
 *   node  (0x20 bytes):  +0x00 next (0 = last), +0x10 pointer to the data (PHYSICAL address), +0x16 u16 length of the data
 *   data  (right after the node):  +0x00 u32 WLC command, +0x08 u32 payload length (inner), +0x18 payload
 * For GET_VAR / SET_VAR the payload is the iovar name (NUL ended); a SET puts the value byte right after the NUL, and a GET gets the value
 * back at +0x18 (over the name).  A flat buffer is walked as a node and its bytes followed as pointers: that hangs IOS.
 * Source of the layout: the layout reverse-engineered for wize-logic/melee-wii (wii/src/wifi.c), which reports it working on a Wii; this
 * file is our own code written from that description (no code copied), and it is only used by the NIN_MS_WIFIFIX build (build_wii.sh).
 * Everything is big-endian by explicit byte stores, so the host test (tests/wl_build_test.c) checks it on any machine.
 */
#ifndef VBBA_WL_H
#define VBBA_WL_H

#include "vbba_types.h"

#define VBBA_WL_IOCTL_WLC   1u        /* IOS_Ioctl number on /dev/wl0 */
#define VBBA_WLC_GET_VAR    0x106u
#define VBBA_WLC_SET_VAR    0x107u
#define VBBA_WL_NODE_SIZE   0x20u
#define VBBA_WL_DATA_OFF    0x20u     /* the data follows the node */
#define VBBA_WL_IOBUF_SIZE  0x60u     /* node 0x20 + data 0x40 */
#define VBBA_WL_VALUE_OFF   0x18u     /* payload / GET result, from the start of the data */
#define VBBA_WL_GET_INNER   0x20u     /* length asked for a GET_VAR of a bool iovar */

static inline void vbba_wl_put32(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }
static inline vbba_u32 vbba_wl_get32(const vbba_u8 *p) { return ((vbba_u32)p[0] << 24) | ((vbba_u32)p[1] << 16) | ((vbba_u32)p[2] << 8) | p[3]; }

/* Builds the 0x60 byte buffer `io` (32-byte aligned by the caller) for WLC command `cmd`.  `data_phys` is the physical address io + 0x20
   will have.  `name`: the iovar for GET_VAR / SET_VAR (NULL for other commands); `setval` >= 0 is the value byte of a SET, -1 for none (GET).
   `inner` is the payload length.  Returns 0, or -1 when the name does not fit (the buffer is then left zeroed). */
static inline int vbba_wl_build(vbba_u8 *io, vbba_u32 data_phys, vbba_u32 cmd, vbba_u32 inner, const char *name, int setval)
{
    vbba_u8 *node = io, *data = io + VBBA_WL_DATA_OFF;
    unsigned i, n = 0;
    for (i = 0; i < VBBA_WL_IOBUF_SIZE; i++) io[i] = 0;
    if (name) {
        while (name[n]) n++;
        if (VBBA_WL_VALUE_OFF + n + 2u > VBBA_WL_IOBUF_SIZE - VBBA_WL_DATA_OFF) return -1;
    }
    vbba_wl_put32(node + 0x00, 0);
    vbba_wl_put32(node + 0x10, data_phys);
    node[0x16] = (vbba_u8)((VBBA_WL_VALUE_OFF + inner) >> 8); node[0x17] = (vbba_u8)(VBBA_WL_VALUE_OFF + inner);
    vbba_wl_put32(data + 0x00, cmd);
    vbba_wl_put32(data + 0x08, inner);
    if (name) {
        for (i = 0; i < n; i++) data[VBBA_WL_VALUE_OFF + i] = (vbba_u8)name[i];
        if (setval >= 0) data[VBBA_WL_VALUE_OFF + n + 1u] = (vbba_u8)setval;
    }
    return 0;
}

/* The answer of a GET_VAR "roam_off": 0 = roaming on, non-zero = off */
static inline vbba_u32 vbba_wl_value(const vbba_u8 *io) { return vbba_wl_get32(io + VBBA_WL_DATA_OFF + VBBA_WL_VALUE_OFF); }

#endif
