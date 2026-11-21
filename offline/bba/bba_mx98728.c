/*
 * Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Parts of this driver follow the libogc / libogc2 BBA driver (gcif.c): the
 * authentication response mix, the pre-reset sequence and the SIACTRL/SIACONN
 * power-on values. libogc is licensed under the zlib licence (see LICENSES/Zlib.txt
 * and NOTICE). This version is altered; it is not the original libogc.
 * Upstream: https://github.com/devkitPro/libogc
 *           https://github.com/extremscorner/libogc2
 */
/*
 * GameCube BBA (MX98728EC) driver, see bba_mx98728.h.  Polling, synchronous.
 *
 * Wire format (research doc section 1.1):
 *   command port (first byte bit 7 clear): 0x00 ID, 0x02/0x42 read/write EXI IMR,
 *   0x03/0x43 read/write EXI ISR; data follows after one dummy byte.
 *   register/buffer port: byte0 = 0x80 (read) or 0xC0 (write), bytes 1..2 = 16-bit
 *   address (high, low), byte 3 dummy, data from byte 4 on; address auto-increments
 *   except at WRTXFIFOD.  Multi-byte registers and the RX descriptor are little endian.
 */
#include "bba_mx98728.h"
#ifdef MM_FC
#define PV_WHY(k) do { if (!b->pv_why) b->pv_why = (k); } while (0)   /* why PV was left: the FC readout's word 121 only */
#else
#define PV_WHY(k) ((void)0)
#endif

static void delay(struct bba *b, sdnet_u32 us)
{
    if (b->ops->delay_us)
        b->ops->delay_us(b->ctx, us);
}

/* ---- command port ---- */
static sdnet_u8 cmd_in8(struct bba *b, sdnet_u8 reg)
{
    sdnet_u32 v;
    b->ops->select(b->ctx);
    b->ops->imm_write(b->ctx, (sdnet_u32)reg << 24, 2);      /* cmd + dummy */
    v = b->ops->imm_read(b->ctx, 1);
    b->ops->deselect(b->ctx);
    return (sdnet_u8)(v >> 24);
}

/* command-port read that keeps the chip selected for a while before release (libogc does
   this for reg 0x0f during reset); delay is skipped when ops->delay_us is NULL */
static sdnet_u8 cmd_in8_slow(struct bba *b, sdnet_u8 reg)
{
    sdnet_u32 v;
    b->ops->select(b->ctx);
    b->ops->imm_write(b->ctx, (sdnet_u32)reg << 24, 2);
    v = b->ops->imm_read(b->ctx, 1);
    delay(b, 200);
    b->ops->deselect(b->ctx);
    return (sdnet_u8)(v >> 24);
}

/* command-port write of 1..4 data bytes, MSB first */
static void cmd_out(struct bba *b, sdnet_u8 reg, sdnet_u32 val, int len)
{
    b->ops->select(b->ctx);
    b->ops->imm_write(b->ctx, (sdnet_u32)(0x40 | reg) << 24, 2);
    b->ops->imm_write(b->ctx, val << (32 - 8 * len), len);
    b->ops->deselect(b->ctx);
}

static void cmd_out8(struct bba *b, sdnet_u8 reg, sdnet_u8 val)
{
    cmd_out(b, reg, val, 1);
}

static sdnet_u32 cmd_in32(struct bba *b, sdnet_u8 reg)
{
    sdnet_u32 v;
    b->ops->select(b->ctx);
    b->ops->imm_write(b->ctx, (sdnet_u32)reg << 24, 2);
    v = b->ops->imm_read(b->ctx, 4);
    b->ops->deselect(b->ctx);
    return v;                                   /* first wire byte = bits 31..24 */
}

/* ---- challenge/response (EXI-side authentication) ----
   After a reset the host tells the chip its device id (0xD107) and an "acstart" byte (0x4E)
   and reads the chip revision byte.  The chip then raises ISR bit 0x10 with a 32-bit challenge
   readable at cmd reg 0x08; the host must write a 32-bit answer to cmd reg 0x09 that is a fixed
   byte-wise mix of the challenge, the revision byte and the device id.  The chip confirms with
   ISR bit 0x08.  All arithmetic is modulo 256 per byte. */
/* Provenance: same mix as libogc/libogc2 gcif.c (see file header); keep the upstream notice. */
sdnet_u32 bba_auth_response(sdnet_u32 ch, sdnet_u8 revid, sdnet_u16 devid)
{
    sdnet_u32 a = ch >> 24, b = (ch >> 16) & 0xff, c = (ch >> 8) & 0xff, d = ch & 0xff;
    sdnet_u32 rev = revid, dh = (devid >> 8) & 0xff, dl = devid & 0xff;
    sdnet_u32 r0, r1, r2, r3;

    r0 = (a + b * 0xc1 + 0x18 + rev) ^ (c * d + 0x90);
    r0 &= 0xff;
    r1 = ((b + c + 0x90) ^ (r0 + a - 0xc1)) & 0xff;
    r2 = ((c + 0xc8) ^ (r0 + ((dh + rev * 0x23) ^ 0x19))) & 0xff;
    r3 = ((a + 0xc1) ^ (d + ((dl + 0xc8) ^ 0x90))) & 0xff;
    return (r0 << 24) | (r1 << 16) | (r2 << 8) | r3;
}

/* one service pass over the EXI ISR; returns the ISR value read */
int bba_service(struct bba *b)
{
    sdnet_u8 isr = cmd_in8(b, BBA_CMD_ISR);

    if (isr & BBA_ISR_AUTHDONE) {
        b->authstat = cmd_in8(b, BBA_CMD_AUTHSTAT);
        cmd_out8(b, BBA_CMD_ISR, BBA_ISR_AUTHDONE);
        if (b->auth == BBA_AUTH_SENT) {
            b->auth = BBA_AUTH_OK;
            b->st.auth_done++;
        }
    }
    if (isr & BBA_ISR_CHALLENGE) {
        sdnet_u32 ch, resp;
        cmd_out8(b, BBA_CMD_ACSTART, BBA_ACSTART);   /* libogc re-sends acstart each time */
        ch = cmd_in32(b, BBA_CMD_CHALLENGE);
        resp = bba_auth_response(ch, b->revid, BBA_DEVID);
        cmd_out(b, BBA_CMD_RESPONSE, resp, 4);
        cmd_out8(b, BBA_CMD_ISR, BBA_ISR_CHALLENGE);
        b->auth = BBA_AUTH_SENT;
        b->st.auth_challenges++;
    }
    if (isr & BBA_ISR_RESETREQ) {
        b->reset_requested = 1;
        b->st.isr_resetreq++;
        cmd_out8(b, BBA_CMD_ISR, BBA_ISR_RESETREQ);
    }
    if (isr & BBA_ISR_UNKNOWN)
        cmd_out8(b, BBA_CMD_ISR, BBA_ISR_UNKNOWN);
    return isr;
}

sdnet_u32 bba_read_id(struct bba *b)
{
    sdnet_u32 v;
#ifdef BBA_HWFIX
    /* the SDK's EXIGetID and libogc's EXI_GetID read the ID at 1 MHz; a real EXI device may not answer the ID command at 32 MHz */
    if ((b->ops->select_slow ? b->ops->select_slow(b->ctx) : b->ops->select(b->ctx)) != 0) {
        b->dg.id = 0;
        return 0;
    }
#else
    if (b->ops->select(b->ctx) != 0)
        return 0;
#endif
    b->ops->imm_write(b->ctx, 0, 2);
    v = b->ops->imm_read(b->ctx, 4);
    b->ops->deselect(b->ctx);
#ifdef BBA_HWFIX
    b->dg.id = v;
    b->dg.id_tries++;
#endif
    return v;
}

/* ---- register port ---- */
static void reg_begin(struct bba *b, sdnet_u16 addr, int write)
{
    b->ops->select(b->ctx);
    b->ops->imm_write(b->ctx,
                      (write ? 0xC0000000u : 0x80000000u) | ((sdnet_u32)addr << 8), 4);
}

sdnet_u8 bba_reg_in8(struct bba *b, sdnet_u16 reg)
{
    sdnet_u32 v;
    reg_begin(b, reg, 0);
    v = b->ops->imm_read(b->ctx, 1);
    b->ops->deselect(b->ctx);
    return (sdnet_u8)(v >> 24);
}

void bba_reg_out8(struct bba *b, sdnet_u16 reg, sdnet_u8 v)
{
    reg_begin(b, reg, 1);
    b->ops->imm_write(b->ctx, (sdnet_u32)v << 24, 1);
    b->ops->deselect(b->ctx);
}

static sdnet_u16 reg_in16(struct bba *b, sdnet_u16 reg)
{
    sdnet_u32 v;
    reg_begin(b, reg, 0);
    v = b->ops->imm_read(b->ctx, 2);
    b->ops->deselect(b->ctx);
    return (sdnet_u16)(((v >> 24) & 0xff) | ((v >> 8) & 0xff00));  /* byte0 = low */
}

static void reg_out16(struct bba *b, sdnet_u16 reg, sdnet_u16 val)
{
#ifdef BBA_HWFIX
    /* libogc / libogc2 / libdb write the 12-bit ring registers one byte per EXI transaction (bba_out12); no source writes them as one 2-byte burst */
    bba_reg_out8(b, reg, (sdnet_u8)(val & 0xff));
    bba_reg_out8(b, (sdnet_u16)(reg + 1), (sdnet_u8)(val >> 8));
    return;
#endif
    reg_begin(b, reg, 1);
    b->ops->imm_write(b->ctx,
                      ((sdnet_u32)(val & 0xff) << 24) | ((sdnet_u32)(val >> 8) << 16), 2);
    b->ops->deselect(b->ctx);
}

/* Read n bytes of packet RAM / registers starting at addr into dst. */
static void mem_read(struct bba *b, sdnet_u16 addr, sdnet_u8 *dst, sdnet_u32 n)
{
    const struct bba_exi_ops *o = b->ops;
    sdnet_u32 bulk = 0, i, c, v;

    reg_begin(b, addr, 0);
    if (o->dma_read && n >= 32) {
        bulk = n & ~31u;
        if (o->invalidate) o->invalidate(b->ctx, b->dmabuf, bulk);
        o->dma_read(b->ctx, b->dmabuf, bulk);
        if (o->invalidate) o->invalidate(b->ctx, b->dmabuf, bulk);
        for (i = 0; i < bulk; i++)
            dst[i] = b->dmabuf[i];
    }
    for (i = bulk; i < n; i += c) {
        sdnet_u32 j;
        c = n - i > 4 ? 4 : n - i;
        v = o->imm_read(b->ctx, (int)c);
        for (j = 0; j < c; j++)
            dst[i + j] = (sdnet_u8)(v >> (24 - 8 * j));
    }
    o->deselect(b->ctx);
}

/* Append n bytes to the TX FIFO port (address does not advance). */
static void fifo_write(struct bba *b, const sdnet_u8 *src, sdnet_u32 n)
{
    const struct bba_exi_ops *o = b->ops;
    sdnet_u32 bulk = 0, i, c, v;

    reg_begin(b, BBA_WRTXFIFOD, 1);
    if (o->dma_write && n >= 32) {
        bulk = n & ~31u;
        for (i = 0; i < bulk; i++)
            b->dmabuf[i] = src[i];
        if (o->flush) o->flush(b->ctx, b->dmabuf, bulk);
        o->dma_write(b->ctx, b->dmabuf, bulk);
    }
    for (i = bulk; i < n; i += c) {
        sdnet_u32 j;
        c = n - i > 4 ? 4 : n - i;
        v = 0;
        for (j = 0; j < c; j++)
            v |= (sdnet_u32)src[i + j] << (24 - 8 * j);
        o->imm_write(b->ctx, v, (int)c);
    }
    o->deselect(b->ctx);
}

/* ---- bring up ---- */
static const sdnet_u8 siactrl_init[8] = { 0x80, 0x00, 0x03, 0x83, 0x32, 0xfe, 0x1f, 0x1f };

/* Bring-up is a small phase machine so a caller that must never block (a game frame hook) can run
   one phase per call: bba_init_step returns BBA_PENDING until the settle times (10 ms twice) and the
   challenge window (BBA_AUTH_TIMEOUT_MS) have passed on the caller's millisecond clock.  Only
   delays of at most a few hundred microseconds are still spun inside a phase.  bba_init is the
   blocking wrapper (same register sequence and timing as before). */
enum { IP_ID = 0, IP_SLOW0F, IP_MAIN, IP_AUTH, IP_DONE };

int bba_init_begin(struct bba *b, const struct bba_exi_ops *ops, void *ctx,
                   const sdnet_u8 *mac)
{
    int i;

    b->ops = ops;
    b->ctx = ctx;
    b->link = 0;
    b->auth = BBA_AUTH_NONE;
    b->reset_requested = 0;
    b->idle_polls = 0;
    b->revid = 0;
    b->authstat = 0;
    b->st.auth_challenges = b->st.auth_done = b->st.isr_resetreq = 0;
    b->st.rx_frames = b->st.rx_dropped_big = b->st.rx_bad_desc = b->st.rx_resync = 0;
    b->st.tx_frames = b->st.tx_timeouts = 0;
    b->ip_phase = IP_ID;
#ifdef BBA_HWFIX
    {
        sdnet_u8 *z = (sdnet_u8 *)&b->dg;
        sdnet_u32 k;
        for (k = 0; k < sizeof b->dg; k++)
            z[k] = 0;
        b->id_left = 120;
    }
#endif
    b->rx_snap_ok = 0;
    b->ncra_ok = 0;
    b->pv = 0;
    b->pv_idle = 0;
    b->pv_why = b->pv_bad0 = b->pv_bad1 = 0;
    b->ip_wake = 0;
    b->ip_t0 = 0;
    b->ip_have_mac = mac ? 1 : 0;
    for (i = 0; i < 6; i++)
        b->ip_mac[i] = mac ? mac[i] : 0;

    if (!ops || !ops->select || !ops->deselect || !ops->imm_read || !ops->imm_write)
        return BBA_E_PARAM;
    return BBA_OK;
}

int bba_init_step(struct bba *b, sdnet_u32 now_ms)
{
    int i;
    sdnet_u8 v, nwayc;

    if (b->ip_phase != IP_ID && b->ip_phase != IP_DONE &&
        (sdnet_s32)(now_ms - b->ip_wake) < 0)
        return BBA_PENDING;

    switch (b->ip_phase) {
    case IP_ID:
#ifdef BBA_HWFIX
        if (!b->dg.t_init_ms)
            b->dg.t_init_ms = now_ms ? now_ms : 1u;
        if (bba_read_id(b) != BBA_EXI_ID) {
            /* a wrong or zero ID right after the previous user (Swiss) let go is retried for about 2 s of calls before it is called "no BBA" */
            if (b->id_left) {
                b->id_left--;
                return BBA_PENDING;
            }
            b->dg.last_rc = (sdnet_u8)BBA_E_NODEV;
            return BBA_E_NODEV;
        }
        b->dg.phase = 1;
#else
        if (bba_read_id(b) != BBA_EXI_ID)
            return BBA_E_NODEV;
#endif
        /* mask all interrupts on both levels; we poll the ring pointers */
        cmd_out8(b, BBA_CMD_IMR, 0x00);            /* EXI-side IMR; ISR is not acked blindly: a
                                                      pending 0x10/0x08 belongs to the handshake */
        /* pre-reset sequence common to libogc, libogc2, libdb uIP and YAGCD: clear 0x60,
           read command reg 0x0f, 10 ms settle each, then pulse NCRA.RESET */
        bba_reg_out8(b, BBA_SIACTRL2, 0x00);
        b->ip_wake = now_ms + 10;
        b->ip_phase = IP_SLOW0F;
        return BBA_PENDING;

    case IP_SLOW0F:
#ifdef BBA_HWFIX
        b->dg.phase = 2;
#endif
        (void)cmd_in8_slow(b, 0x0f);
        b->ip_wake = now_ms + 10;
        b->ip_phase = IP_MAIN;
        return BBA_PENDING;

    case IP_MAIN:
        bba_reg_out8(b, BBA_NCRA, BBA_NCRA_RESET);
        delay(b, 100);
        bba_reg_out8(b, BBA_NCRA, 0);
        delay(b, 100);

        bba_reg_out8(b, BBA_IMR, 0x00);
        bba_reg_out8(b, BBA_IR, 0xff);                 /* write-1-to-clear */

        /* handshake setup, same order as libogc: revision read, device id (2 bytes MSB first),
           acstart; the chip answers later with ISR 0x10 (serviced in IP_AUTH) */
        b->revid = cmd_in8(b, BBA_CMD_REVID);
        cmd_out(b, BBA_CMD_DEVID, BBA_DEVID, 2);
        cmd_out8(b, BBA_CMD_ACSTART, BBA_ACSTART);

        /* SIACTRL/SIACONN back to power-on values (libogc, libogc2). Zeros here would
           reconfigure the PHY/NWAY block. */
        for (i = 0; i < 8; i++)
            bba_reg_out8(b, (sdnet_u16)(BBA_SIACTRL + i), siactrl_init[i]);
        delay(b, 100);

        bba_reg_out8(b, BBA_MISC2, BBA_MISC2_AUTORCVR);  /* auto RX-full recovery (4 sources) */
        bba_reg_out8(b, BBA_NCRB, BBA_NCRB_CA | BBA_NCRB_AB);

        /* ring: TX low boundary, RX pages BP..RHBP, both pointers at BP */
        b->bp = BBA_INIT_BP;
        b->rhbp = BBA_INIT_RHBP;
        b->rrp = BBA_INIT_BP;
        reg_out16(b, BBA_TLBP, BBA_INIT_TLBP);
        reg_out16(b, BBA_BP, b->bp);
        reg_out16(b, BBA_RHBP, b->rhbp);
        reg_out16(b, BBA_RWP, b->bp);
        reg_out16(b, BBA_RRP, b->rrp);
        b->rrp_flushed = b->rrp; b->rrp_dirty = 0;
#ifdef BBA_HWFIX
        /* libogc2 __bba_recv_init: SIACTRL2 = 0x74, RXINTT = 0x0600, GCA.ARXERRB, all before the receiver starts */
        bba_reg_out8(b, BBA_SIACTRL2, 0x74);
        bba_reg_out8(b, 0x14, 0x00);
        bba_reg_out8(b, 0x15, 0x06);
        bba_reg_out8(b, BBA_GCA, BBA_GCA_ARXERRB);
        b->dg.rb_tlbp = reg_in16(b, BBA_TLBP);
        b->dg.rb_bp = reg_in16(b, BBA_BP);
        b->dg.rb_rhbp = reg_in16(b, BBA_RHBP);
        b->dg.rb_rwp = reg_in16(b, BBA_RWP);
        b->dg.rb_rrp = reg_in16(b, BBA_RRP);
        b->dg.phase = 3;
#endif

        /* autonegotiation: keep the LTE/reserved top bits, then ANE, then restart (libogc) */
#ifdef BBA_HWFIX
        b->dg.nwayc = bba_reg_in8(b, BBA_NWAYC);
#endif
        nwayc = (sdnet_u8)(bba_reg_in8(b, BBA_NWAYC) & 0xc0);
        bba_reg_out8(b, BBA_NWAYC, nwayc);
        delay(b, 100);
        nwayc |= 0x04;
        bba_reg_out8(b, BBA_NWAYC, nwayc);
        delay(b, 100);
        nwayc |= 0x08;
        bba_reg_out8(b, BBA_NWAYC, nwayc);
        delay(b, 100);

        /* station address */
        if (b->ip_have_mac) {
            for (i = 0; i < 6; i++) {
                b->mac[i] = b->ip_mac[i];
                bba_reg_out8(b, (sdnet_u16)(BBA_PAR0 + i), b->ip_mac[i]);
            }
        } else {
            for (i = 0; i < 6; i++)
                b->mac[i] = bba_reg_in8(b, (sdnet_u16)(BBA_PAR0 + i));
        }

        /* start receiver */
        v = bba_reg_in8(b, BBA_NCRA);
        bba_reg_out8(b, BBA_NCRA, (sdnet_u8)(v | BBA_NCRA_SR));
#ifdef BBA_HWFIX
        {
            int q;
            b->dg.ncra_rb = bba_reg_in8(b, BBA_NCRA);
            b->dg.ncrb_rb = bba_reg_in8(b, BBA_NCRB);
            b->dg.nwayc_rb = bba_reg_in8(b, BBA_NWAYC);
            b->dg.siactrl2_rb = bba_reg_in8(b, BBA_SIACTRL2);
            b->dg.misc2_rb = bba_reg_in8(b, BBA_MISC2);
            b->dg.gca_rb = bba_reg_in8(b, BBA_GCA);
            b->dg.imr_rb = bba_reg_in8(b, BBA_IMR);
            b->dg.nways_first = bba_reg_in8(b, BBA_NWAYS);
            b->dg.nways_or |= b->dg.nways_first;
            for (q = 0; q < 6; q++)
                b->dg.par[q] = bba_reg_in8(b, (sdnet_u16)(BBA_PAR0 + q));
            b->dg.phase = 4;
        }
#endif

        /* challenge/response: unmask the EXI-side bits while we poll the ISR (uIP does the same),
           then fall back to the caller's run-time mask.  A chip that never challenges (Dolphin's
           model) leaves auth == NONE; that is tolerated unless auth_strict. */
        cmd_out8(b, BBA_CMD_IMR, 0xF8);
        b->ip_t0 = now_ms;
        b->ip_wake = now_ms;
        b->ip_phase = IP_AUTH;
        return BBA_PENDING;

    case IP_AUTH:
        bba_service(b);
        if (b->auth != BBA_AUTH_OK && (sdnet_s32)(now_ms - b->ip_t0) < (sdnet_s32)BBA_AUTH_TIMEOUT_MS) {
            b->ip_wake = now_ms + 1;
            return BBA_PENDING;
        }
        cmd_out8(b, BBA_CMD_IMR, b->exi_imr_run);
        b->ip_phase = IP_DONE;
#ifdef BBA_HWFIX
        b->dg.t_auth_ms = now_ms ? now_ms : 1u;
        b->dg.phase = 5;
        b->dg.last_rc = (sdnet_u8)((b->auth_strict && b->auth != BBA_AUTH_OK) ? BBA_E_AUTH : BBA_OK);
#endif
        if (b->auth_strict && b->auth != BBA_AUTH_OK)
            return BBA_E_AUTH;
        return BBA_OK;

    default:
        return BBA_OK;
    }
}

int bba_init(struct bba *b, const struct bba_exi_ops *ops, void *ctx,
             const sdnet_u8 *mac)
{
    sdnet_u32 now = 0;
    int rc = bba_init_begin(b, ops, ctx, mac);

    if (rc != BBA_OK)
        return rc;
    while ((rc = bba_init_step(b, now)) == BBA_PENDING) {
        delay(b, 1000);
        now++;
    }
    return rc;
}

#ifdef BBA_HWFIX
void bba_note_link(struct bba *b, sdnet_u8 nways, sdnet_u32 now_ms)
{
    b->dg.link_polls++;
    b->dg.nways_last = nways;
    b->dg.nways_or |= nways;
    if ((nways & (BBA_NWAYS_LS10 | BBA_NWAYS_LS100)) && !b->dg.t_link_ms)
        b->dg.t_link_ms = now_ms ? now_ms : 1u;
}
#endif

int bba_link_wait(struct bba *b, sdnet_u32 timeout_ms)
{
    sdnet_u32 t;
    for (t = 0; ; t++) {
        b->link = bba_reg_in8(b, BBA_NWAYS);
        if (b->link & (BBA_NWAYS_LS10 | BBA_NWAYS_LS100))
            return BBA_OK;
        if (t >= timeout_ms)
            return BBA_E_LINK;
        delay(b, 1000);
    }
}

/* ---- data path ---- */
/* TX completion wait: tries x (NCRA read + 10 us delay) is about 2-4 ms of interrupt-off time at the console's EXI speed,
   longer than a 1514 B frame needs at 10 Mb/s (1.2 ms).  After a timeout the next BBA_TX_HOLD_CALLS sends are refused
   at once so a stuck transmitter (cable pulled, link flap) cannot cost tens of ms per datagram (security review N7). */
#ifndef BBA_TX_TRIES
#define BBA_TX_TRIES 200
#endif
#ifndef BBA_TX_HOLD_CALLS
#define BBA_TX_HOLD_CALLS 16
#endif

int bba_send(struct bba *b, const sdnet_u8 *frame, sdnet_u16 len)
{
    sdnet_u8 pad[60];
    sdnet_u8 ncra;
    int tries;

    if (len < 14 || len > 1514)
        return BBA_E_PARAM;
    if (b->tx_hold) {
        b->tx_hold--;
        b->st.tx_skipped++;
        return BBA_E_TXTIMEOUT;
    }

    if (len < 60) {                  /* the chip does not pad: do it here */
        sdnet_u32 i;
        for (i = 0; i < len; i++) pad[i] = frame[i];
        for (; i < 60; i++) pad[i] = 0;
        frame = pad;
        len = 60;
    }
    if (b->pv && b->pv_xfer && (sdnet_u32)len + 8u <= BBA_DMA_BUF) {
        /* Wii kernel with the PV fast path: the whole frame in ONE EXI command */
        sdnet_u32 i;
        b->dmabuf[0] = 'P'; b->dmabuf[1] = 'V'; b->dmabuf[2] = (sdnet_u8)(len >> 8); b->dmabuf[3] = (sdnet_u8)len;
        b->dmabuf[4] = b->dmabuf[5] = b->dmabuf[6] = b->dmabuf[7] = 0;
        for (i = 0; i < len; i++) b->dmabuf[8 + i] = frame[i];
        if (b->ops->flush) b->ops->flush(b->ctx, b->dmabuf, (sdnet_u32)len + 8u);
        if (b->pv_xfer(b->ctx, b->dmabuf, (sdnet_u32)len + 8u, 1) == 0) { b->st.tx_frames++; b->st_pv_tx++; return 0; }
        b->st_pv_err++;
        PV_WHY(1);
        b->pv = 0;                                    /* the device went away: the register path from now on */
    }
    if (b->wii_fast && b->ops->dma_write && (sdnet_u32)len + 4u <= BBA_DMA_BUF) {
        /* Wii (virtual chip): header + frame in ONE DMA, NCRA from the copy, no transmit poll: 5 EXI commands instead of 12 + one per 4-byte tail word */
        const struct bba_exi_ops *o = b->ops;
        sdnet_u32 i;
        b->dmabuf[0] = 0xC0; b->dmabuf[1] = (sdnet_u8)(BBA_WRTXFIFOD >> 8); b->dmabuf[2] = (sdnet_u8)BBA_WRTXFIFOD; b->dmabuf[3] = 0;
        for (i = 0; i < len; i++) b->dmabuf[4 + i] = frame[i];
        if (o->flush) o->flush(b->ctx, b->dmabuf, (sdnet_u32)len + 4u);
        o->select(b->ctx);
        o->dma_write(b->ctx, b->dmabuf, (sdnet_u32)len + 4u);
        o->deselect(b->ctx);
        if (!b->ncra_ok) { b->ncra_sh = (sdnet_u8)(bba_reg_in8(b, BBA_NCRA) & ~(BBA_NCRA_ST0 | BBA_NCRA_ST1)); b->ncra_ok = 1; }
        bba_reg_out8(b, BBA_NCRA, (sdnet_u8)(b->ncra_sh | BBA_NCRA_ST1));
        b->st.tx_frames++;
        return 0;
    }
    fifo_write(b, frame, len);

    ncra = bba_reg_in8(b, BBA_NCRA);
    bba_reg_out8(b, BBA_NCRA,
                 (sdnet_u8)((ncra & ~BBA_NCRA_ST0) | BBA_NCRA_ST1));
    for (tries = 0; tries < BBA_TX_TRIES; tries++) {
        if (!(bba_reg_in8(b, BBA_NCRA) & BBA_NCRA_ST1)) {
            b->st.tx_frames++;
            return 0;
        }
        delay(b, 10);
    }
    b->st.tx_timeouts++;
    b->tx_hold = BBA_TX_HOLD_CALLS;
    return BBA_E_TXTIMEOUT;
}

#define BBA_PV_RX 1536u             /* PV receive: the whole buffer every time (a frame plus the 8-byte header always fits; the chip copies only the frame) */
#define BBA_RX_BURST 192u            /* one DMA: 4 descriptor bytes + 188 payload bytes (a game datagram with its Ethernet / IP / UDP headers fits) */
static void rrp_flush(struct bba *b)
{
    if (b->wii_fast) {                                /* the virtual chip takes both bytes in one transfer (3 EXI commands, not 6) */
        reg_begin(b, BBA_RRP, 1);
        b->ops->imm_write(b->ctx, ((sdnet_u32)(b->rrp & 0xff) << 24) | ((sdnet_u32)(b->rrp >> 8) << 16), 2);
        b->ops->deselect(b->ctx);
    } else
        reg_out16(b, BBA_RRP, b->rrp);
    b->rrp_flushed = b->rrp;
    b->rrp_dirty = 0;
}

int bba_recv(struct bba *b, sdnet_u8 *buf, sdnet_u16 max)
{
    sdnet_u16 rwp, next;
    sdnet_u8 hdr[4];
    sdnet_u32 desc, plen, dlen, start, ring_end, seg1, guard;

    /* A well-formed chain reaches RWP within one lap of the ring, and only empty / oversized frames loop back here, so
       a walk longer than the ring (a self-loop or a cycle of empty frames after an overrun or an EXI glitch) is
       broken off: resynchronise to RWP and report it instead of spinning with interrupts off (security review N8). */
    if (b->pv && b->pv_xfer) {
        /* Wii kernel with the PV fast path: ONE EXI command returns the oldest frame (descriptor checks done by the chip) or 'none'.
           Every read asks for the whole buffer (no second read for a long frame).  An answer without the 'PV' header leaves PV for good at once (1261; the
           bytes are kept for FC words 121 / 123 / 124); the frame it carried is lost once and the register path takes over (RRP read back from the chip). */
        const struct bba_exi_ops *o = b->ops;
        sdnet_u32 n = BBA_PV_RX, j;
        if (b->pv_idle) { b->pv_idle = 0; return 0; }    /* the previous frame said nothing more was waiting: the end of this poll */
        for (guard = 0; guard < 32u; guard++) {
            b->dmabuf[0] = 0; b->dmabuf[1] = 0;           /* a transfer the kernel answers without filling the buffer must not hand back the previous frame */
            if (o->flush) o->flush(b->ctx, b->dmabuf, 32u);
            if (o->invalidate) o->invalidate(b->ctx, b->dmabuf, n);
            if (b->pv_xfer(b->ctx, b->dmabuf, n, 0) != 0) { PV_WHY(2); break; }
            if (o->invalidate) o->invalidate(b->ctx, b->dmabuf, n);
            if (b->dmabuf[0] != 'P' || b->dmabuf[1] != 'V') {   /* 1261: a bad answer leaves PV AT ONCE, for good (1260's retry was answered 'empty' by the kernel, which had already
                                                           consumed the frame: PV stayed on and every received frame was lost, no DHCP lease on ) */
#ifdef MM_FC
                if (!b->pv_why) { b->pv_why = 3; b->pv_bad0 = b->dmabuf[0]; b->pv_bad1 = b->dmabuf[1]; for (j = 0; j < 8u; j++) b->pv_dump[j] = b->dmabuf[j]; }   /* FC words 121 / 123 / 124 */
#endif
                break;
            }
            dlen = ((sdnet_u32)b->dmabuf[2] << 8) | b->dmabuf[3];
            if (dlen == 0) return 0;
            if (dlen + 8u > n) { PV_WHY(4); break; }     /* the chip never answers a frame longer than the buffer it was given */
            b->st.rx_frames++;
            b->st_pv_rx++;
            if (dlen > max) { b->st.rx_dropped_big++; continue; }
            for (j = 0; j < dlen; j++) buf[j] = b->dmabuf[8 + j];
            if (!b->dmabuf[4]) b->pv_idle = 1;
            return (int)dlen;
        }
        b->st_pv_err++;
        b->pv = 0;                                        /* an answer we do not understand: the register path from now on (RRP from the chip) */
        b->rrp = (sdnet_u16)(reg_in16(b, BBA_RRP) & 0x0fff);
        b->rrp_flushed = b->rrp; b->rrp_dirty = 0; b->rx_snap_ok = 0;
        return 0;
    }
    for (guard = 0; ; guard++) {
        if (guard > (sdnet_u32)(b->rhbp - b->bp) + 2u) {
            b->st.rx_walk_abort++;
            b->rx_snap_ok = 0;
            rwp = reg_in16(b, BBA_RWP) & 0x0fff;
            if (rwp >= b->bp && rwp <= b->rhbp) {
                b->rrp = rwp;
                rrp_flush(b);
            }
            return BBA_E_RING;
        }
        if (b->rx_snap_ok)                           /* Wii: this poll already read RWP; the frames up to it are taken without reading it again */
            rwp = b->rx_snap;
        else {
            if (b->rx_hint && guard == 0) {          /* Wii: the kernel publishes the chip's RWP; equal to our RRP = empty, no EXI traffic (every 64th poll still looks) */
                sdnet_u32 hv = *b->rx_hint;
                if ((hv >> 16) == 0x5258u && (hv & 0xfffu) == b->rrp && ((++b->idle_polls) & 63) != 0) {
                    if (b->rrp_dirty) rrp_flush(b);
                    return 0;
                }
            }
            rwp = reg_in16(b, BBA_RWP) & 0x0fff;
            if (b->wii_fast) { b->rx_snap = rwp; b->rx_snap_ok = 1; }
        }
        if (rwp == b->rrp) {
            b->rx_snap_ok = 0;                       /* the next call (the next poll) reads RWP again */
            if (b->rrp_dirty) {
                sdnet_u32 n = (sdnet_u32)(b->rhbp - b->bp + 1);
                if (!b->wii_fast || ((sdnet_u32)b->rrp + n - b->rrp_flushed) % n >= 3u) rrp_flush(b);
            }
            if ((++b->idle_polls & (b->wii_fast ? 255 : 7)) == 0)      /* chips may re-challenge while running (the virtual chip only at the bring-up) */
                bba_service(b);
            return 0;
        }
        if (rwp < b->bp || rwp > b->rhbp) {      /* chip state we do not understand */
            b->rx_snap_ok = 0;
            b->st.rx_resync++;
            return 0;
        }

        ring_end = ((sdnet_u32)b->rhbp + 1u) * 256u;
        if (b->rx_burst && b->ops->dma_read && (sdnet_u32)b->rrp * 256u + BBA_RX_BURST <= ring_end) {
            sdnet_u8 blk[BBA_RX_BURST];
            sdnet_u32 j;
            mem_read(b, (sdnet_u16)(b->rrp * 256u), blk, BBA_RX_BURST);
            desc = (sdnet_u32)blk[0] | ((sdnet_u32)blk[1] << 8) | ((sdnet_u32)blk[2] << 16) | ((sdnet_u32)blk[3] << 24);
            next = (sdnet_u16)(desc & 0xfff);
            plen = (desc >> 12) & 0xfff;
            if (plen >= BBA_RX_HDR && next >= b->bp && next <= b->rhbp && next != b->rrp &&
                plen - BBA_RX_HDR <= BBA_DMA_BUF && plen - BBA_RX_HDR <= max && plen - BBA_RX_HDR <= BBA_RX_BURST - 4u) {
                dlen = plen - BBA_RX_HDR;
                for (j = 0; j < dlen; j++) buf[j] = blk[4 + j];
                b->rrp = next;
                b->rrp_dirty = 1;
                b->st.rx_frames++;
                {   sdnet_u32 lag = ((sdnet_u32)b->rrp + (sdnet_u32)(b->rhbp - b->bp + 1) - b->rrp_flushed) % (sdnet_u32)(b->rhbp - b->bp + 1);
                    if (lag >= 6u) rrp_flush(b);     /* never let the chip see much less free ring than there is */
                }
                if (dlen == 0)
                    continue;
                return (int)dlen;
            }
            /* anything unusual (long frame, bad descriptor): the unchanged path below deals with it */
        }

        mem_read(b, (sdnet_u16)(b->rrp * 256u), hdr, 4);
        desc = (sdnet_u32)hdr[0] | ((sdnet_u32)hdr[1] << 8) |
               ((sdnet_u32)hdr[2] << 16) | ((sdnet_u32)hdr[3] << 24);
        next = (sdnet_u16)(desc & 0xfff);
        plen = (desc >> 12) & 0xfff;             /* includes the 4-byte descriptor */
        if (plen < BBA_RX_HDR || next < b->bp || next > b->rhbp || next == b->rrp ||
            plen - BBA_RX_HDR > BBA_DMA_BUF) {
            /* bad descriptor: drop everything queued so far */
            b->st.rx_bad_desc++;
            b->rx_snap_ok = 0;
            b->rrp = rwp;
            rrp_flush(b);
            return 0;
        }
        dlen = plen - BBA_RX_HDR;
        if (dlen > max) {
            b->st.rx_dropped_big++;
            b->rrp = next;
            rrp_flush(b);
            continue;
        }

        start = (sdnet_u32)b->rrp * 256u + BBA_RX_HDR;
        seg1 = ring_end - start;
        if (seg1 > dlen) seg1 = dlen;
        if (seg1)
            mem_read(b, (sdnet_u16)start, buf, seg1);
        if (dlen > seg1)
            mem_read(b, (sdnet_u16)((sdnet_u32)b->bp * 256u), buf + seg1, dlen - seg1);

        b->rrp = next;
        if (b->rx_burst) {
            b->rrp_dirty = 1;
            {   sdnet_u32 lag = ((sdnet_u32)b->rrp + (sdnet_u32)(b->rhbp - b->bp + 1) - b->rrp_flushed) % (sdnet_u32)(b->rhbp - b->bp + 1);
                if (lag >= 6u) rrp_flush(b);
            }
        } else
            rrp_flush(b);
        b->st.rx_frames++;
        if (dlen == 0)
            continue;
        return (int)dlen;
    }
}

static int nic_send(void *ctx, const sdnet_u8 *f, sdnet_u16 len)
{
    return bba_send((struct bba *)ctx, f, len);
}

static int nic_recv(void *ctx, sdnet_u8 *buf, sdnet_u16 max)
{
    return bba_recv((struct bba *)ctx, buf, max);
}

void bba_make_nic(struct bba *b, struct sdnet_nic *nic)
{
    int i;
    nic->send = nic_send;
    nic->recv = nic_recv;
    nic->ctx = b;
    for (i = 0; i < 6; i++)
        nic->mac[i] = b->mac[i];
}
