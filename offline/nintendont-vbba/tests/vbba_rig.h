/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_rig: one emulated "Wii" for host tests, every layer real except the hardware:
 *
 *   OUR driver offline/bba/bba_mx98728.c (unchanged) + offline/net/sdnet.c (unchanged)
 *     -> game-side bba_exi_ops written like the project's other tooling (SDK EXISelect / EXIImm / EXIDma / EXISync)
 *     -> model of Nintendont's PowerPC stubs (kernel/asm/EXISelect.S, EXIImm.S, EXIDMA.S; deselect/sync/lock = no-ops):
 *        write words loaded from the buffer, reads stored right-aligned with stb/sth/stw, len 3 NOT stored (stock)
 *     -> model of the kernel's EXI dispatcher (one command per main-loop pass) -> platform/nintendont/vbba_ndexi.c
 *     -> core/vbba_chip.c -> core/vbba_router.c -> a vbba_transport (IOS transport over tests/mock_ios.c, or the
 *        host loopback transport)
 * Every EXI call that reaches the kernel is counted as one "trip" (the PowerPC spins until the ARM answers it).
 */
#ifndef VBBA_RIG_H
#define VBBA_RIG_H

#include "../../bba/bba_mx98728.h"
#include "../core/vbba_chip.h"
#include "../core/vbba_router.h"
#include "../platform/nintendont/vbba_ndexi.h"
#include "../platform/nintendont/vbba_tp_ios.h"
#include "../platform/host/vbba_tp_host.h"
#include "mock_ios.h"

#define RIG_STUB_STOCK 0              /* Nintendont bbc0a20 EXIImm.S: a 3-byte read result is dropped */
#define RIG_STUB_PATCHED 1            /* EXIImm.S fixed in our patch: stores 3 bytes */

struct rig {
    /* game side */
    struct bba B;
    struct sdnet S;
    int stub;                         /* RIG_STUB_* */
    int split3;                       /* game glue splits 3-byte reads into 2 + 1 (fix on the image side) */
    int use_dma;
    int interleave;                   /* the ARM runs one vbba_chip_poll between every two EXI trips */
    int sel;                          /* the SDK's view: chip selected */
    int absent;                       /* the game's select reaches another device (no vBBA in this kernel) */
    /* kernel side */
    struct vbba_chip chip;
    struct vbba_ndexi_stats nx;
    struct vbba_router router;
    struct vbba_tp_ios tpi;
    struct vbba_tp_host tph;
    struct mock_ios io;
    const struct vbba_transport *tp;
    const struct vbba_frame_backend *be;
    int real_time;                    /* clock = host monotonic ms (MOCK_UDP / host transport) */
    unsigned long long us;            /* simulated microseconds (MOCK_MEM) */
    /* counters */
    unsigned trips, trip_select, trip_imm, trip_dma, garbage;
    unsigned pv_cmp_bad, pv_cmp_n;    /* check in the rig: delivered PV frames compared with the ring (vbba_chip_pv_cmp) */
    int pv_zero;                      /* the next N PV receive transfers consume a frame and come back without the 'PV' header: models the / field answers */
    unsigned trip_us;                 /* simulated microseconds each trip costs (MOCK_MEM; 0 = none, the default): the PowerPC waits one kernel main-loop pass per command */
    struct bba_exi_ops ops;
};

extern const vbba_u8 RIG_MAC[6];
extern const vbba_u8 RIG_ROUTER_MAC[6];

/* transport: 0 = IOS transport over mock_ios (mode MOCK_MEM / MOCK_UDP), 1 = host loopback transport */
void rig_setup(struct rig *g, int transport, int mock_mode, vbba_u32 ios_ip, const struct vbba_router_cfg *rc,
               vbba_u32 chip_flags, const struct vbba_frame_backend *be_override);
/* the ARM side's work between EXI commands: router/transport poll and RX ring fill */
void rig_arm(struct rig *g);
/* advance the simulated clock (MOCK_MEM) by ms, running the ARM loop and sdnet_poll once per ms */
void rig_run(struct rig *g, int ms);
vbba_u32 rig_ms(struct rig *g);
sdnet_u32 rig_clock(void *ctx);
/* bring up driver + sdnet (DHCP started); returns bba_init's rc */
int rig_bringup(struct rig *g, int strict);
void rig_close(struct rig *g);

/* standard router config for tests: private 10.211.0.2/24 (or MIRROR), allow 127.0.0.0/8 and 192.168.0.0/16 */
void rig_router_cfg(struct vbba_router_cfg *c, vbba_u32 flags);

#endif
