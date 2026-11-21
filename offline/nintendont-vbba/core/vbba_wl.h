/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_wl: the command buffer for the Wii's Wi-Fi driver (/dev/wl0, IOS's Broadcom WL module), used to switch its roaming scan off.
 * Pure data, no IOS call (the kernel glue does IOS_Open/IOS_Ioctl); host-tested in tests/wl_test.c.
 *
 * Why: the driver scans for a better access point about every 10 s while connected and the link stops for about 150 ms each time
 * (5-7 frames of rollback).  The WLC iovar "roam_off" = 1 stops the scans; the driver sets it back to 0 on every (re)association, so
 * the caller checks again (GET) after the network comes up and after a match.
 *
 * Layout of one request (reverse-engineered from IOS's WD by the wize-logic/melee-wii port, wii/src/wifi.c; only the layout is used here):
 *   IOS_Ioctl number 1 on /dev/wl0, input buffer = one node followed by its data, both 32-byte aligned, physical addresses in the node:
 *     node  (0x20 bytes) : +0x00 next (0), +0x10 data pointer (PHYSICAL), +0x16 u16 data length
 *     data  (0x18 + n)   : +0x00 WLC command, +0x08 payload length n, +0x18 payload
 *     payload of GET_VAR : name\0 padded, the value comes back at the start of the payload (u32)
 *     payload of SET_VAR : name\0 then the value (u32, big-endian here, as the PowerPC stores it)
 * UNVERIFIED ON A CONSOLE: the glue gates the call (compile flag and /vbba.cfg roam=off) and treats every failure as "leave roaming as it is".
 */
#ifndef VBBA_WL_H
#define VBBA_WL_H

#include "vbba_types.h"

#define VBBA_WL_PATH       "/dev/wl0"
#define VBBA_WL_IOCTL_WLC  1            /* IOS_Ioctl number that carries a WLC command */
#define VBBA_WLC_GET_VAR   0x106u
#define VBBA_WLC_SET_VAR   0x107u
#define VBBA_WL_IO_SIZE    0x60u        /* node 0x20 + data 0x40 */
#define VBBA_WL_NODE       0x00u
#define VBBA_WL_DATA       0x20u
#define VBBA_WL_PAYLOAD    0x18u        /* payload offset inside the data */

static inline void vbba_wl_put32(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; }
static inline void vbba_wl_put16(vbba_u8 *p, vbba_u32 v) { p[0] = (vbba_u8)(v >> 8); p[1] = (vbba_u8)v; }
static inline vbba_u32 vbba_wl_get32(const vbba_u8 *p) { return ((vbba_u32)p[0] << 24) | ((vbba_u32)p[1] << 16) | ((vbba_u32)p[2] << 8) | p[3]; }

/* Fill io (VBBA_WL_IO_SIZE bytes, 32-byte aligned) with a GET_VAR / SET_VAR of the iovar `name` (at most 15 characters).  io_phys = the
   PHYSICAL address of io (the console's virtual address & 0x3FFFFFFF).  `inner` = payload bytes the module may use: 0x20 for a GET
   (name + room for the value), name length + 1 + 4 for a SET.  Returns the number of bytes to pass as the ioctl's input length, 0 on a bad name. */
static inline vbba_u32 vbba_wl_build(vbba_u8 *io, vbba_u32 io_phys, vbba_u32 cmd, const char *name, vbba_u32 setval)
{
    vbba_u32 i, n = 0, inner;
    while (name[n]) n++;
    if (n == 0 || n > 15) return 0;
    for (i = 0; i < VBBA_WL_IO_SIZE; i++) io[i] = 0;
    inner = cmd == VBBA_WLC_SET_VAR ? n + 1 + 4 : 0x20u;
    vbba_wl_put32(io + VBBA_WL_NODE + 0x00, 0);
    vbba_wl_put32(io + VBBA_WL_NODE + 0x10, io_phys + VBBA_WL_DATA);
    vbba_wl_put16(io + VBBA_WL_NODE + 0x16, VBBA_WL_PAYLOAD + inner);
    vbba_wl_put32(io + VBBA_WL_DATA + 0x00, cmd);
    vbba_wl_put32(io + VBBA_WL_DATA + 0x08, inner);
    for (i = 0; i < n; i++) io[VBBA_WL_DATA + VBBA_WL_PAYLOAD + i] = (vbba_u8)name[i];
    if (cmd == VBBA_WLC_SET_VAR) vbba_wl_put32(io + VBBA_WL_DATA + VBBA_WL_PAYLOAD + n + 1, setval);
    return VBBA_WL_IO_SIZE;
}

/* (NIN_MS_WLPM): a plain WLC command with a 4-byte payload, for the power-save mode: WLC_GET_PM 85 / WLC_SET_PM 86 (Broadcom wlioctl.h numbering, the
   same numbering the iovar path uses: WLC_GET_VAR 262 = 0x106, WLC_SET_VAR 263 = 0x107).  Value 0 = PM_OFF (CAM, the radio always awake), 1 = PM_MAX, 2 = PM_FAST.
   UNVERIFIED: that IOS's WL module passes these two commands through like the iovars (melee-wii's wifi.c only shows the iovar path).  Payload for a GET: 4 bytes the
   module overwrites with the mode; for a SET: the mode.  Returns the ioctl input length. */
#define VBBA_WLC_GET_PM    85u
#define VBBA_WLC_SET_PM    86u
static inline vbba_u32 vbba_wl_build_cmd(vbba_u8 *io, vbba_u32 io_phys, vbba_u32 cmd, vbba_u32 val)
{
    vbba_u32 i;
    for (i = 0; i < VBBA_WL_IO_SIZE; i++) io[i] = 0;
    vbba_wl_put32(io + VBBA_WL_NODE + 0x10, io_phys + VBBA_WL_DATA);
    vbba_wl_put16(io + VBBA_WL_NODE + 0x16, VBBA_WL_PAYLOAD + 4u);
    vbba_wl_put32(io + VBBA_WL_DATA + 0x00, cmd);
    vbba_wl_put32(io + VBBA_WL_DATA + 0x08, 4u);
    vbba_wl_put32(io + VBBA_WL_DATA + VBBA_WL_PAYLOAD, val);
    return VBBA_WL_IO_SIZE;
}

/* The value a GET_VAR wrote back (the module overwrites the start of the payload with it).  Non-zero = roaming is OFF. */
static inline vbba_u32 vbba_wl_value(const vbba_u8 *io) { return vbba_wl_get32(io + VBBA_WL_DATA + VBBA_WL_PAYLOAD); }

#endif
