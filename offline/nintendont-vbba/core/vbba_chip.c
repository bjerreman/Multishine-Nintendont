/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_chip: MX98728 model, see vbba_chip.h.  Project-owned C99, no libc, no heap. */
#include "vbba_chip.h"

/* registers the model gives meaning to (names as in offline/bba/bba_mx98728.h) */
#define R_NCRA 0x00
#define NCRA_RESET 0x01
#define NCRA_ST1 0x04
#define NCRA_SR 0x08
#define R_NCRB 0x01
#define NCRB_PR 0x01                   /* promiscuous (assumed bit; our driver never sets it) */
#define NCRB_AB 0x10
#define R_IR 0x09
#define IR_TI 0x04
#define R_BP 0x0a
#define R_RWP 0x16
#define R_RRP 0x18
#define R_RHBP 0x1a
#define R_PAR 0x20
#define R_NWAYC 0x30
#define R_NWAYS 0x31
#define R_TXFIFO 0x48
#define NWAYS_UP 0x1e                  /* what Dolphin's BuiltIn model returns (offline/bba/); bit 0x02 = 100 Mb/s */

#define ISR_AUTHDONE 0x08
#define ISR_CHALLENGE 0x10

static void zero(vbba_u8 *p, vbba_u32 n) { while (n--) *p++ = 0; }

static vbba_u16 r16(const struct vbba_chip *c, vbba_u32 r) { return (vbba_u16)((c->regs[r] | (c->regs[r + 1] << 8)) & 0x0fffu); }
static void w16(struct vbba_chip *c, vbba_u32 r, vbba_u16 v) { c->regs[r] = (vbba_u8)v; c->regs[r + 1] = (vbba_u8)(v >> 8); }
vbba_u16 vbba_chip_rwp(const struct vbba_chip *c) { return r16(c, R_RWP); }
vbba_u16 vbba_chip_rrp(const struct vbba_chip *c) { return r16(c, R_RRP); }

static vbba_u32 xs32(vbba_u32 x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x ? x : 0x2545F491u; }

static int link_up(struct vbba_chip *c)
{
    struct vbba_frame_caps caps;
    if (!c->be || !c->be->get_capabilities) return 1;
    caps.link_up = 0;
    if (c->be->get_capabilities(c->be->context, &caps) != VBBA_OK) return 0;
    return caps.link_up != 0;
}

static void mac_reset(struct vbba_chip *c)
{
    int i;
    zero(c->regs, sizeof c->regs);
    c->regs[R_NWAYC] = 0x84;           /* power-on value the driver's mask/keep sequence expects */
    for (i = 0; i < 6; i++) c->regs[R_PAR + i] = c->mac[i];
    c->fifo_len = 0;
    c->fifo_ovf = 0;
    c->auth_state = 0;                 /* a re-init must be able to authenticate again */
    c->resp_n = 0;
    c->held_len = 0;
    c->st.resets++;
}

void vbba_chip_set_mac(struct vbba_chip *c, const vbba_u8 mac[6])
{
    int i;
    for (i = 0; i < 6; i++) c->mac[i] = mac[i];
    if (!c->guest_reset) for (i = 0; i < 6; i++) c->regs[R_PAR + i] = mac[i];
}

void vbba_chip_init(struct vbba_chip *c, const vbba_u8 mac[6], vbba_u8 revid, vbba_u32 flags,
                    const struct vbba_frame_backend *be, vbba_u32 seed)
{
    int i;
    zero((vbba_u8 *)c, sizeof *c);
    for (i = 0; i < 6; i++) c->mac[i] = mac[i];
    c->revid = revid;
    c->flags = (vbba_u8)flags;
    c->be = be;
    c->rng = xs32(seed ^ 0x9E3779B9u);
    mac_reset(c);
    c->st.resets = 0;
}

void vbba_chip_select(struct vbba_chip *c) { c->selected = 1; c->pos = 0; c->st.selects++; }
void vbba_chip_deselect(struct vbba_chip *c) { c->selected = 0; c->pos = 0; }

/* ---- TX ---- */
static void transmit(struct vbba_chip *c)
{
    if (c->fifo_ovf) c->st.tx_overflow++;
    else if (c->fifo_len < 60 && !(c->flags & VBBA_CHIP_NO_PAD_CHECK)) c->st.tx_runt++;
    else if (c->fifo_len < 14) c->st.tx_runt++;
    else if (!c->be || !c->be->submit_tx || c->be->submit_tx(c->be->context, c->fifo, c->fifo_len) != VBBA_OK) c->st.tx_backend_err++;
    else c->st.tx_frames++;
    c->fifo_len = 0;
    c->fifo_ovf = 0;
    c->regs[R_NCRA] &= (vbba_u8)~NCRA_ST1;
    c->regs[R_IR] |= IR_TI;
}

/* ---- register / packet RAM space ---- */
static vbba_u8 *cell(struct vbba_chip *c, vbba_u32 a)
{
    a &= 0x1fffu;
    if (a < 0x100u) return &c->regs[a];
    return &c->pram[(a - 0x100u) & (VBBA_PRAM - 1u)];
}

static vbba_u8 reg_read(struct vbba_chip *c)
{
    vbba_u32 a = c->addr & 0x1fffu;
    vbba_u8 v;
    if (a == R_NWAYS) c->regs[R_NWAYS] = link_up(c) ? NWAYS_UP : 0;
    v = a == R_TXFIFO ? 0 : *cell(c, a);
    if (a != R_TXFIFO) c->addr++;
    return v;
}

static void reg_write(struct vbba_chip *c, vbba_u8 v)
{
    vbba_u32 a = c->addr & 0x1fffu;
    if (a == R_TXFIFO) {
        if (c->fifo_len < VBBA_FIFO_MAX) c->fifo[c->fifo_len++] = v;
        else c->fifo_ovf = 1;
        return;                        /* no auto-increment at the FIFO port */
    }
    c->addr++;
    if (a == R_NCRA) {
        c->regs[R_NCRA] = v;
        if (v & NCRA_RESET) { c->guest_reset = 1; mac_reset(c); c->regs[R_NCRA] = v; return; }
        if (v & NCRA_ST1) transmit(c);
    } else if (a == R_IR) {
        c->regs[R_IR] &= (vbba_u8)~v;  /* write 1 to clear */
    } else if (a == R_NWAYS) {
        /* read-only */
    } else {
        *cell(c, a) = v;
    }
}

/* ---- command port ---- */
static vbba_u8 cmd_byte(struct vbba_chip *c, vbba_u8 d)
{
    vbba_u32 k = c->pos - 2u;          /* data index, valid for pos >= 2 */
    if (c->pos < 2) return 0;
    switch (c->cmd) {
    case 0x00: { static const vbba_u8 id[4] = { 0x04, 0x02, 0x02, 0x00 }; return k < 4 ? id[k] : 0; }
    case 0x01: return c->revid;
    case 0x02: return c->exi_imr;
    case 0x03: return c->exi_isr;
    case 0x08: return k < 4 ? (vbba_u8)(c->challenge >> (24 - 8 * k)) : 0;
    case 0x0b: return c->auth_state == 3 ? 0x01 : 0x00;    /* diagnostic only; meaning on hardware unknown */
    case 0x0f: return 0;
    case 0x42: if (k == 0) c->exi_imr = d; return 0;
    case 0x43: if (k == 0) c->exi_isr &= (vbba_u8)~d; return 0;
    case 0x44: if (k == 0) c->devid_hi = d; else if (k == 1) c->devid_lo = d; return 0;
    case 0x45:
        if (k == 0 && (c->flags & VBBA_CHIP_AUTH) && c->auth_state == 0 && c->devid_hi == 0xD1 && c->devid_lo == 0x07 && d == 0x4E) {
            c->rng = xs32(c->rng);
            c->challenge = c->rng;
            c->auth_state = 2;
            c->exi_isr |= ISR_CHALLENGE;
            c->st.challenges++;
        }
        return 0;
    case 0x49:
        if (k < 4 && ++c->resp_n == 4 && c->auth_state == 2) {   /* any answer is accepted */
            c->auth_state = 3;
            c->exi_isr |= ISR_AUTHDONE;
            c->st.responses++;
        }
        return 0;
    default:
        return 0;
    }
}

static vbba_u8 byte_xfer(struct vbba_chip *c, vbba_u8 d)
{
    vbba_u8 r = 0;
    if (c->pos == 0) {
        c->cmd = d;
        c->resp_n = 0;
        if (!(d & 0x80)) {
            switch (d) {
            case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x0b: case 0x0f:
            case 0x42: case 0x43: case 0x44: case 0x45: case 0x49: break;
            default: c->st.bad_cmds++; break;
            }
        }
    } else if (c->cmd & 0x80) {
        if (c->pos == 1) c->addr = (vbba_u16)((c->addr & 0x00ffu) | ((vbba_u16)d << 8));
        else if (c->pos == 2) c->addr = (vbba_u16)((c->addr & 0xff00u) | d);
        else if (c->pos >= 4) {
            if (c->cmd & 0x40) reg_write(c, d);
            else r = reg_read(c);
        }
    } else {
        r = cmd_byte(c, d);
    }
    if (c->pos < 0xffffffffu) c->pos++;
    return r;
}

void vbba_chip_xfer(struct vbba_chip *c, vbba_u8 *buf, vbba_u32 len, int is_write)
{
    vbba_u32 i;
    if (!c->selected) {
        if (!is_write) zero(buf, len);
        return;
    }
    if (is_write) {
        c->st.bytes_in += len;
        for (i = 0; i < len; i++) (void)byte_xfer(c, buf[i]);
    } else {
        c->st.bytes_out += len;
        for (i = 0; i < len; i++) buf[i] = byte_xfer(c, 0);
    }
}

/* ---- RX ring producer ---- */
/* returns 1 stored (or consumed by a filter), 0 no room (keep the frame) */
static int ring_store(struct vbba_chip *c, const vbba_u8 *f, vbba_u32 len)
{
    vbba_u32 bp = r16(c, R_BP), rhbp = r16(c, R_RHBP), rwp = r16(c, R_RWP), rrp = r16(c, R_RRP);
    vbba_u32 n, used, need, p, off, i, plen, desc;
    static const vbba_u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int is_bc = 1, is_me = 1;

    if (!(c->regs[R_NCRA] & NCRA_SR)) { c->st.rx_off_drop++; return 1; }
    for (i = 0; i < 6; i++) { if (f[i] != bc[i]) is_bc = 0; if (f[i] != c->regs[R_PAR + i]) is_me = 0; }
    if (!(c->regs[R_NCRB] & NCRB_PR) && !(is_me || (is_bc && (c->regs[R_NCRB] & NCRB_AB)))) { c->st.rx_filtered++; return 1; }
    if (bp < 1 || bp > rhbp || rhbp > 16 || rwp < bp || rwp > rhbp || rrp < bp || rrp > rhbp) { c->st.rx_bad_ring++; return 0; }
    n = rhbp - bp + 1;
    used = (rwp + n - rrp) % n;
    plen = len + 4u;
    need = (plen + 255u) / 256u;
    if (need + used >= n) { c->st.rx_full_waits++; return 0; }   /* one page stays free: RWP == RRP means empty */

    /* data after the descriptor, page by page, wrapping RHBP -> BP */
    p = rwp;
    off = 4;
    for (i = 0; i < len; i++) {
        if (off == 256u) { off = 0; p = p == rhbp ? bp : p + 1; }
        *cell(c, p * 256u + off) = f[i];
        off++;
    }
    p = p == rhbp ? bp : p + 1;        /* next free page */
    desc = p | (plen << 12);           /* status byte 0 */
    *cell(c, rwp * 256u + 0) = (vbba_u8)desc;
    *cell(c, rwp * 256u + 1) = (vbba_u8)(desc >> 8);
    *cell(c, rwp * 256u + 2) = (vbba_u8)(desc >> 16);
    *cell(c, rwp * 256u + 3) = (vbba_u8)(desc >> 24);
    w16(c, R_RWP, (vbba_u16)p);
    c->st.rx_frames++;
    return 1;
}

/* ---- PV fast path (vbba_chip.h) ---- */
int vbba_chip_pv_tx(struct vbba_chip *c, const vbba_u8 *buf, vbba_u32 len)
{
    vbba_u32 n, i;
    if (len < 8u || buf[0] != 'P' || buf[1] != 'V') { c->st.pv_bad++; return -1; }
    n = ((vbba_u32)buf[2] << 8) | buf[3];
    if (n > VBBA_FIFO_MAX || n + 8u > len) { c->st.pv_bad++; return -1; }
    for (i = 0; i < n; i++) c->fifo[i] = buf[8 + i];
    c->fifo_len = (vbba_u16)n;
    c->fifo_ovf = 0;
    c->st.pv_tx++;
    transmit(c);                       /* the same checks, backend call and counters as an NCRA.ST1 write */
    return 0;
}

/* (the frame too, as aligned 32-bit stores): the 8-byte answer header is written LAST, as two aligned 32-bit stores (the bytes are put together in a local word, so the byte order in memory is the
   same on the big-endian ARM and on a little-endian host).  ..22 wrote it with byte stores into the PowerPC's memory, and the delivered answers read wrong on
   the console (00 13 00 56 / 00 00 00 56, 01 7D 15 56 50 00 00 00 where 50 56 01 56 00 00 00 00 was stored); the empty answer, and the frame bytes
   (stored in one ascending run, as the register path stores every read), came out right.  Nintendont writes the PowerPC's memory with 32-bit stores (write32). */
static void pv_put(vbba_u8 *buf, vbba_u32 dlen, vbba_u32 more)
{
    vbba_u8 h[8];
    vbba_u32 w[2], i;
    h[0] = 'P'; h[1] = 'V'; h[2] = (vbba_u8)(dlen >> 8); h[3] = (vbba_u8)dlen; h[4] = (vbba_u8)more; h[5] = 0; h[6] = 0; h[7] = 0;
    if (((unsigned long)buf & 3u) != 0) { for (i = 0; i < 8u; i++) buf[i] = h[i]; return; }
    for (i = 0; i < 8u; i++) ((vbba_u8 *)w)[i] = h[i];                  /* native order: the stores put the bytes back in this order */
    ((volatile vbba_u32 *)buf)[0] = w[0];
    ((volatile vbba_u32 *)buf)[1] = w[1];
}
int vbba_chip_pv_rx(struct vbba_chip *c, vbba_u8 *buf, vbba_u32 len)
{
    vbba_u32 bp = r16(c, R_BP), rhbp = r16(c, R_RHBP), rwp = r16(c, R_RWP), rrp = r16(c, R_RRP);
    vbba_u32 desc, next, plen, dlen, a, i, more;
    if (len < 8u) { c->st.pv_bad++; return -1; }
    c->st.pv_rx_polls++;
    if (bp < 1 || bp > rhbp || rhbp > 16 || rwp < bp || rwp > rhbp || rrp < bp || rrp > rhbp || rwp == rrp) { pv_put(buf, 0, 0); return 0; }   /* empty (or a ring the guest never set up) */
    a = rrp * 256u;
    desc = (vbba_u32)*cell(c, a) | ((vbba_u32)*cell(c, a + 1) << 8) | ((vbba_u32)*cell(c, a + 2) << 16) | ((vbba_u32)*cell(c, a + 3) << 24);
    next = desc & 0xfffu; plen = (desc >> 12) & 0xfffu;
    if (plen < 4u || next < bp || next > rhbp || next == rrp) { w16(c, R_RRP, (vbba_u16)rwp); c->st.pv_bad++; pv_put(buf, 0, 0); return 0; }   /* cannot happen with ring_store; drop the queue */
    dlen = plen - 4u;
    if (dlen + 8u > len) { pv_put(buf, dlen, 1); c->st.pv_rx_short++; return 0; }   /* does not fit: header only, not consumed; the guest reads again with room for it */
    more = (next != rwp || c->held_len) ? 1u : 0u;
    if (((unsigned long)buf & 3u) == 0 && dlen + 11u <= len) {             /* the frame as aligned 32-bit stores too (the tail word padded with zeros) */
        vbba_u32 w;
        vbba_u8 *wb = (vbba_u8 *)&w;
        for (i = 0, a += 4u; i < dlen; i += 4u) {
            vbba_u32 k;
            for (k = 0; k < 4u; k++) {
                if (i + k < dlen) {
                    if ((a & 255u) == 0 && a / 256u > rhbp) a = bp * 256u;        /* wrap RHBP -> BP */
                    wb[k] = *cell(c, a);
                    a++;
                } else wb[k] = 0;
            }
            ((volatile vbba_u32 *)(buf + 8))[i / 4u] = w;
        }
    } else {
        for (i = 0, a += 4u; i < dlen; i++) {                              /* an unaligned buffer: byte stores (never the case on the console) */
            if ((a & 255u) == 0 && a / 256u > rhbp) a = bp * 256u;
            buf[8 + i] = *cell(c, a);
            a++;
            if ((a & 255u) == 0 && a / 256u > rhbp) a = bp * 256u;
        }
    }
    pv_put(buf, dlen, more);                                               /* then the header, two word stores */
    w16(c, R_RRP, (vbba_u16)next);
    c->st.pv_rx++;
    return (int)dlen;
}

/* check: the frame of the answer just given (ring position rrp0, dlen bytes) against buf + 8 read back through the given pointer; returns the bytes that
   differ and the first differing offset (in *first, -1 none) */
vbba_u32 vbba_chip_pv_cmp(const struct vbba_chip *c, vbba_u32 rrp0, const volatile vbba_u8 *buf, vbba_u32 dlen, int *first)
{
    vbba_u32 bp = r16(c, R_BP), rhbp = r16(c, R_RHBP), a = rrp0 * 256u + 4u, i, n = 0;
    *first = -1;
    for (i = 0; i < dlen; i++) {
        if ((a & 255u) == 0 && a / 256u > rhbp) a = bp * 256u;
        if (buf[8 + i] != *cell((struct vbba_chip *)c, a)) { if (*first < 0) *first = (int)i; n++; }
        a++;
        if ((a & 255u) == 0 && a / 256u > rhbp) a = bp * 256u;
    }
    return n;
}

int vbba_chip_poll(struct vbba_chip *c, int budget)
{
    int stored = 0;
    if (c->be && c->be->poll) (void)c->be->poll(c->be->context);
    while (budget-- > 0) {
        if (!c->held_len) {
            vbba_u16 n = 0;
            if (!c->be || !c->be->receive_rx) break;
            if (c->be->receive_rx(c->be->context, c->held, VBBA_FRAME_MAX, &n) != VBBA_OK || n == 0) break;
            if (n > 1518u) { c->st.rx_too_big++; continue; }
            while (n < 60u) c->held[n++] = 0;          /* the wire minimum (the driver strips nothing) */
            c->held_len = n;
        }
        if (!ring_store(c, c->held, c->held_len)) break;
        c->held_len = 0;
        stored++;
    }
    return stored;
}
