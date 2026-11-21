/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_rig: see vbba_rig.h. */
#include "vbba_rig.h"
#include <string.h>
#include <time.h>

const vbba_u8 RIG_MAC[6] = { 0x00, 0x09, 0xbf, 0x12, 0x34, 0x56 };
const vbba_u8 RIG_ROUTER_MAC[6] = { 0x02, 0x56, 0x42, 0x42, 0x41, 0x01 };   /* locally administered */

vbba_u32 rig_ms(struct rig *g) { return g->real_time ? vbba_host_ms() : (vbba_u32)(g->us / 1000ull); }
sdnet_u32 rig_clock(void *ctx) { return rig_ms((struct rig *)ctx); }

static void advance(struct rig *g, unsigned us)
{
    if (g->real_time) {
        if (us >= 100) { struct timespec ts = { 0, (long)us * 1000L }; nanosleep(&ts, 0); }
        return;
    }
    g->us += us;
    g->io.now = (vbba_u32)(g->us / 1000ull);
}

void rig_arm(struct rig *g) { (void)vbba_chip_poll(&g->chip, 4); }

static void trip(struct rig *g)
{
    g->trips++;
    if (g->trip_us && !g->real_time) { g->us += g->trip_us; g->io.now = (vbba_u32)(g->us / 1000ull); }
    if (g->interleave) (void)vbba_chip_poll(&g->chip, 1);
}

/* ---- the kernel's EXI dispatcher for one command (EXIUpdateRegistersNEW + our route) ---- */
static vbba_u32 kernel_imm(struct rig *g, vbba_u32 data, vbba_u32 len, vbba_u32 mode)
{
    if (!g->sel) return 0;            /* the SP1 handler is only reached for the selected device 2 */
    return vbba_ndexi_imm(&g->chip, &g->nx, data, len, mode);
}

/* ---- game-side bba_exi_ops through the Nintendont stubs ---- */
static int g_select(void *c)
{
    struct rig *g = c;
    g->trip_select++;
    trip(g);
    g->sel = vbba_ndexi_select(&g->chip, &g->nx, 0, g->absent ? 1u : 2u);   /* EXISelect.S returns EXI_CMD_1 */
    return g->sel ? 0 : -1;
}
static void g_deselect(void *c) { struct rig *g = c; g->sel = 0; }   /* EXIDeselect is "li r3,1; blr": no trip */

static void g_imm_write(void *c, sdnet_u32 d, int len)
{
    struct rig *g = c;
    vbba_u8 p[4] = { 0xA5, 0xA5, 0xA5, 0xA5 };            /* bytes beyond len: whatever was in the game's buffer */
    vbba_u32 word;
    int i;
    for (i = 0; i < len; i++) p[i] = (vbba_u8)(d >> (24 - 8 * i));
    word = ((vbba_u32)p[0] << 24) | ((vbba_u32)p[1] << 16) | ((vbba_u32)p[2] << 8) | p[3];   /* lwz r3,0(r4) */
    g->trip_imm++;
    trip(g);
    (void)kernel_imm(g, word, (vbba_u32)len, 1);
}

static sdnet_u32 one_read(struct rig *g, int len)
{
    vbba_u8 p[4] = { 0, 0, 0, 0 };                        /* u32 tmp = 0 in the glue */
    vbba_u32 v;
    sdnet_u32 out = 0;
    int i;
    g->trip_imm++;
    trip(g);
    v = kernel_imm(g, 0, (vbba_u32)len, 0);
    switch (len) {                                        /* EXIImm.S CopyResult */
    case 1: p[0] = (vbba_u8)v; break;                     /* stb */
    case 2: p[0] = (vbba_u8)(v >> 8); p[1] = (vbba_u8)v; break;            /* sth (big-endian) */
    case 4: p[0] = (vbba_u8)(v >> 24); p[1] = (vbba_u8)(v >> 16); p[2] = (vbba_u8)(v >> 8); p[3] = (vbba_u8)v; break;
    case 3:
        if (g->stub == RIG_STUB_PATCHED) { p[0] = (vbba_u8)(v >> 16); p[1] = (vbba_u8)(v >> 8); p[2] = (vbba_u8)v; }
        else g->garbage++;                                /* stock stub: "b end", nothing stored */
        break;
    }
    for (i = 0; i < len; i++) out |= (sdnet_u32)p[i] << (24 - 8 * i);
    return out;
}

static sdnet_u32 g_imm_read(void *c, int len)
{
    struct rig *g = c;
    if (len == 3 && g->split3) {
        sdnet_u32 a = one_read(g, 2), b = one_read(g, 1);
        return a | (b >> 16);
    }
    return one_read(g, len);
}

static void g_dma_write(void *c, const void *b, sdnet_u32 n)
{
    struct rig *g = c;
    g->trip_dma++;
    trip(g);
    if (g->sel) vbba_ndexi_dma(&g->chip, &g->nx, (vbba_u8 *)(size_t)b, n, 1);
}
static void g_dma_read(void *c, void *b, sdnet_u32 n)
{
    struct rig *g = c;
    g->trip_dma++;
    trip(g);
    if (g->sel) vbba_ndexi_dma(&g->chip, &g->nx, (vbba_u8 *)b, n, 0);
}
static void g_delay(void *c, sdnet_u32 us) { advance((struct rig *)c, us); }
/* the PV device (EXI channel 2 device 1, kernels with NIN_MS_PVBBA): one trip per DMA, no select (the routing stays) */
static int g_pv_xfer(void *c, void *b, sdnet_u32 n, int is_write)   /* is_write: 1 send, 0 receive, 2 receive retried */
{
    struct rig *g = c;
    g->trip_select++;
    trip(g);                                              /* the glue selects the PV device before every transfer (1290) */
    g->trip_dma++;
    trip(g);
    if (is_write == 1) return vbba_chip_pv_tx(&g->chip, (const vbba_u8 *)b, n) == 0 ? 0 : -1;
    if (g->pv_zero > 0) { g->pv_zero--; (void)vbba_chip_pv_rx(&g->chip, (vbba_u8 *)b, n); memset(b, 0, 8); return 0; }   /* as on the chip consumed the frame, the header reads wrong */
    {   vbba_u32 rr0 = vbba_chip_rrp(&g->chip);
        int r = vbba_chip_pv_rx(&g->chip, (vbba_u8 *)b, n), f;
        if (r > 0) { g->pv_cmp_n++; if (vbba_chip_pv_cmp(&g->chip, rr0, (const volatile vbba_u8 *)b, (vbba_u32)r, &f)) g->pv_cmp_bad++; }
        return r >= 0 ? 0 : -1; }
}

void rig_router_cfg(struct vbba_router_cfg *c, vbba_u32 flags)
{
    memset(c, 0, sizeof *c);
    c->flags = flags;
    c->guest_ip = 0x0AD30002u;        /* 10.211.0.2 */
    c->mask = 0xffffff00u;
    c->gw = 0x0AD30001u;
    c->lease_s = 3600;
    memcpy(c->router_mac, RIG_ROUTER_MAC, 6);
    c->nallow = 2;
    c->allow[0].ip = 0x7F000000u; c->allow[0].mask = 0xff000000u; c->allow[0].port_lo = 1; c->allow[0].port_hi = 65535;
    c->allow[1].ip = 0xC0A80000u; c->allow[1].mask = 0xffff0000u; c->allow[1].port_lo = 1; c->allow[1].port_hi = 65535;
}

void rig_setup(struct rig *g, int transport, int mock_mode, vbba_u32 ios_ip, const struct vbba_router_cfg *rc,
               vbba_u32 chip_flags, const struct vbba_frame_backend *be_override)
{
    int stub = g->stub, split3 = g->split3, dma = g->use_dma, inter = g->interleave;
    memset(g, 0, sizeof *g);
    g->stub = stub; g->split3 = split3; g->use_dma = dma; g->interleave = inter;
    if (transport == 1) {
        vbba_tp_host_init(&g->tph);
        g->tp = &g->tph.tp;
        g->real_time = 1;
    } else {
        mock_ios_init(&g->io, mock_mode, ios_ip);
        vbba_tp_ios_init(&g->tpi, &g->io.ops);
        g->tp = vbba_tp_ios_transport(&g->tpi);
        g->real_time = mock_mode == MOCK_UDP;
    }
    vbba_router_init(&g->router, rc, g->tp);
    g->be = be_override ? be_override : vbba_router_backend(&g->router);
    vbba_chip_init(&g->chip, RIG_MAC, 0x04, chip_flags, g->be, 0xC0FFEEu);
    g->ops.select = g_select; g->ops.deselect = g_deselect; g->ops.imm_write = g_imm_write; g->ops.imm_read = g_imm_read;
    g->ops.dma_write = g->use_dma ? g_dma_write : 0; g->ops.dma_read = g->use_dma ? g_dma_read : 0;
    g->ops.flush = 0; g->ops.invalidate = 0; g->ops.delay_us = g_delay;
}

int rig_bringup(struct rig *g, int strict)
{
    struct sdnet_nic nic;
    int rc;
    memset(&g->B, 0, sizeof g->B);
    g->B.auth_strict = (sdnet_u8)strict;
    rc = bba_init(&g->B, &g->ops, g, 0);                  /* MAC from the chip (PAR) */
    g->B.pv_xfer = g_pv_xfer;                             /* used only while the driver's pv flag is set */
    if (rc != BBA_OK) return rc;
    bba_make_nic(&g->B, &nic);
    sdnet_init(&g->S, &nic, rig_clock, g);
    sdnet_dhcp_start(&g->S);
    return rc;
}

void rig_run(struct rig *g, int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        advance(g, 1000);
        rig_arm(g);
        sdnet_poll(&g->S);
    }
}

void rig_close(struct rig *g)
{
    if (g->tp == &g->tph.tp) vbba_tp_host_close_all(&g->tph);
    else mock_ios_close_all(&g->io);
}
