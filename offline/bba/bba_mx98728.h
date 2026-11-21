#ifndef BBA_MX98728_H
#define BBA_MX98728_H

/*
 * Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later (libogc attribution: see bba_mx98728.c)
 * Driver for the GameCube Broadband Adapter (DOL-015, MX98728EC),
 * EXI channel 0 device 2.  Polling only (no interrupt handler), non-blocking
 * receive, no OS, no libc, no libogc.  The init sequence, the SIACTRL/NWAYC values
 * and the authentication response follow libogc/libogc2 gcif.c; see the
 * attribution at the top of bba_mx98728.c.
 *
 * Everything platform specific goes through struct bba_exi_ops: on the console
 * that is a thin wrapper over the SDK/libogc EXI primitives, in host tests it is
 * a fake EXI bus in front of a simulated MX98728.  All ops are synchronous.
 */

#include "../net/sdnet.h"

/* Real-hardware aids (diagnostic builds only): the first run of this driver on a genuine BBA (GameCube, MX98728, FlippyDrive) put
   no frame on the wire while Swiss/libogc2 on the same console did.  With -DMS_HIL_DEV (and not -DBBA_NO_HWFIX) the driver (a) keeps a
   diagnostic record (struct bba_diag) that the dev image shows on the TV, (b) follows libogc2's bring-up more closely (SIACTRL2 = 0x74,
   RXINTT, GCA.ARXERRB, byte-wise 12-bit ring registers, EXI ID read at 1 MHz and retried).  Without MS_HIL_DEV nothing here is compiled:
   the release blobs are unchanged. */
#if defined(MS_HIL_DEV) && !defined(BBA_NO_HWFIX)
#define BBA_HWFIX 1
#endif

/* ---- EXI side ---- */
struct bba_exi_ops {
    int (*select)(void *ctx);                  /* ch0 dev2 @32 MHz; 0 ok */
    void (*deselect)(void *ctx);
    /* immediate transfer, len 1..4, data MSB aligned (first byte on the wire = bits 31..24) */
    void (*imm_write)(void *ctx, sdnet_u32 data, int len);
    sdnet_u32 (*imm_read)(void *ctx, int len); /* result MSB aligned */
    /* DMA, buf 32-byte aligned and len a multiple of 32; may be NULL (imm loops are used) */
    void (*dma_write)(void *ctx, const void *buf, sdnet_u32 len);
    void (*dma_read)(void *ctx, void *buf, sdnet_u32 len);
    /* cache maintenance around DMA; may be NULL (uncached/coherent hosts) */
    void (*flush)(void *ctx, const void *buf, sdnet_u32 len);       /* before dma_write */
    void (*invalidate)(void *ctx, void *buf, sdnet_u32 len);        /* before and after dma_read */
    void (*delay_us)(void *ctx, sdnet_u32 us);                      /* may be NULL: no delay */
#ifdef BBA_HWFIX
    int (*select_slow)(void *ctx);             /* optional: ch0 dev2 @1 MHz for the ID read (the SDK's EXIGetID and libogc's EXI_GetID do that); NULL: select */
#endif
};

#define BBA_EXI_ID 0x04020200u

/* ---- selected registers (MAC register space, 16-bit address, 8 KB) ---- */
#define BBA_NCRA 0x00
#define BBA_NCRA_RESET 0x01
#define BBA_NCRA_ST0 0x02
#define BBA_NCRA_ST1 0x04
#define BBA_NCRA_SR 0x08
#define BBA_NCRB 0x01
#define BBA_NCRB_CA 0x02          /* capture effect */
#define BBA_NCRB_AB 0x10          /* accept broadcast */
#define BBA_IMR 0x08
#define BBA_IR 0x09
#define BBA_BP 0x0a               /* 16-bit little endian pairs: low byte at reg */
#define BBA_TLBP 0x0c
#define BBA_RWP 0x16
#define BBA_RRP 0x18
#define BBA_RHBP 0x1a
#define BBA_PAR0 0x20
#define BBA_NWAYC 0x30
#define BBA_NWAYS 0x31
#define BBA_NWAYS_LS10 0x01
#define BBA_NWAYS_LS100 0x02
#define BBA_NWAYS_ANCLPT 0x08     /* autonegotiation: link partner ability received */
#define BBA_WRTXFIFOD 0x48        /* write port, address does not auto-increment */
#define BBA_MISC2 0x50
#define BBA_MISC2_AUTORCVR 0x80   /* auto RX full recovery */
#define BBA_SIACTRL 0x58          /* 0x58-0x5f */
#define BBA_SIACTRL2 0x60
#define BBA_GCA 0x32              /* GMAC configuration A */
#define BBA_GCA_ARXERRB 0x08      /* accept errored frames (libogc) */

/* ---- EXI-side (command port) registers used by the challenge/response handshake ---- */
#define BBA_CMD_IMR 0x02          /* EXI interrupt mask */
#define BBA_CMD_ISR 0x03          /* EXI interrupt status, write-1-to-clear via 0x43 */
#define BBA_CMD_REVID 0x01        /* read, 1 byte: chip revision, feeds the response */
#define BBA_CMD_DEVID 0x04        /* write, 2 bytes MSB first */
#define BBA_CMD_ACSTART 0x05      /* write, 1 byte */
#define BBA_CMD_CHALLENGE 0x08    /* read, 4 bytes MSB first */
#define BBA_CMD_RESPONSE 0x09     /* write, 4 bytes MSB first */
#define BBA_CMD_AUTHSTAT 0x0b     /* read, 1 byte, diagnostic only (meaning unknown) */
#define BBA_ISR_AUTHDONE 0x08     /* chip finished judging the response */
#define BBA_ISR_CHALLENGE 0x10    /* chip issued a challenge */
#define BBA_ISR_UNKNOWN 0x20
#define BBA_ISR_RESETREQ 0x40     /* chip asks the host to re-run init */
#define BBA_ISR_MAC 0x80          /* MAC-side interrupt (IR & IMR) */
#define BBA_DEVID 0xD107
#define BBA_ACSTART 0x4E

/* ring layout: page p lives at packet-RAM byte offset p*256; page 0 overlaps the
   register file, so RX pages are BP..RHBP inside 1..15 */
#define BBA_INIT_TLBP 0x00
#define BBA_INIT_BP 0x01
#define BBA_INIT_RHBP 0x0f
#define BBA_RX_HDR 4
#define BBA_DMA_BUF 1536

struct bba_stats {
    sdnet_u32 rx_frames, rx_dropped_big, rx_bad_desc, rx_resync;
    sdnet_u32 rx_walk_abort;      /* bba_recv gave up on a descriptor chain that did not reach RWP (self-loop / cycle) */
    sdnet_u32 tx_frames, tx_timeouts;
    sdnet_u32 tx_skipped;         /* sends refused without touching the chip while it is in the post-timeout hold-off */
    sdnet_u32 auth_challenges, auth_done, isr_resetreq;
};

/* handshake state (b->auth) */
#define BBA_AUTH_NONE 0           /* not started or no challenge seen (see bba_init) */
#define BBA_AUTH_SENT 1           /* challenge answered, no verdict (ISR 0x08) yet */
#define BBA_AUTH_OK 2             /* challenge answered and the chip raised ISR 0x08 */

#ifdef BBA_HWFIX
/* what the chip said during bring-up (shown on the dev image's status page; all plain integers, written by the driver only) */
struct bba_diag {
    sdnet_u32 id;                 /* raw EXI ID of the last read (expected 0x04020200) */
    sdnet_u32 id_tries;           /* ID reads done (the first one may be too early after the previous user) */
    sdnet_u8 nwayc, nwayc_rb;     /* NWAYC power-on value read, and the readback after the NWAY kick */
    sdnet_u8 ncra_rb, ncrb_rb;    /* NCRA after SR (expect bit 3), NCRB readback (expect 0x12) */
    sdnet_u8 siactrl2_rb, misc2_rb, gca_rb, imr_rb;   /* readbacks of 0x60 (0x74), 0x50 (0x80), 0x32 (0x08), IMR (0) */
    sdnet_u8 phase;               /* bring-up phase reached (IP_*) */
    sdnet_u8 nways_first, nways_last, nways_or;   /* NWAYS at the end of init, at the last link check, OR of all samples */
    sdnet_u16 rb_bp, rb_rhbp, rb_rwp, rb_rrp, rb_tlbp;   /* ring registers read back after they were written (expect 1, 15, 1, 1, 0) */
    sdnet_u8 par[6];              /* PAR0-5 as read from the chip */
    sdnet_u32 t_init_ms, t_auth_ms, t_link_ms;   /* caller clock at bring-up start, at the verdict (or give-up), at the first link bit */
    sdnet_u32 link_polls;         /* NWAYS samples taken by bba_note_link */
    sdnet_u8 last_rc;             /* last init return code (BBA_E_*, as unsigned) */
};
#endif

struct bba {
    const struct bba_exi_ops *ops;
    void *ctx;
    sdnet_u8 mac[6];
    sdnet_u8 link;                /* NWAYS at last check */
    sdnet_u16 bp, rhbp, rrp;      /* software copies of ring bounds / read pointer */
    struct bba_stats st;
    sdnet_u8 auth;                /* BBA_AUTH_* */
    sdnet_u8 auth_strict;         /* caller sets before bba_init: no challenge in time -> BBA_E_AUTH */
    sdnet_u8 exi_imr_run;         /* EXI IMR to leave after init (0 = polling only; libogc/uIP use 0xF8) */
    sdnet_u8 revid;               /* chip revision read during init */
    sdnet_u8 authstat;            /* last read of cmd 0x0b (diagnostic) */
    sdnet_u8 reset_requested;     /* chip raised ISR 0x40: caller should run bba_init again */
    sdnet_u16 idle_polls;
    /* Wii receive-path savings (the engine sets these at run time on a Wii only; a GameCube leaves them 0 and the driver behaves exactly as before):
       rx_hint = a word the kernel keeps equal to 0x5258<<16 | the chip's RWP (nothing to read while it equals our RRP: no EXI traffic for an idle poll);
       rx_burst = read the descriptor and the first 188 payload bytes with ONE DMA and write RRP once per few frames (rrp_dirty / rrp_flushed) instead of after every frame */
    volatile const sdnet_u32 *rx_hint;
    sdnet_u8 rx_burst, rrp_dirty;
    sdnet_u16 rrp_flushed;
    sdnet_u8 tx_hold;             /* sends still to be refused after a TX timeout (BBA_TX_HOLD_CALLS) */
    /* Wii EXI cut (the engine sets wii_fast on a Wii only, never on a GameCube): every EXI command the PowerPC issues waits one pass of the
       Nintendont kernel's main loop (about 150 us), so the driver issues fewer of them against the virtual chip (offline/nintendont-vbba/core/vbba_chip.c), whose behaviour
       it relies on: a send is ONE DMA of the register-port header + the frame (any length, no 32-byte rounding), NCRA is written from a copy (ncra_sh, read once) and the
       transmit is not polled (the virtual chip hands the frame on and clears ST1 in the same access); a receive poll reads RWP once and takes every frame up to that
       snapshot (rx_snap) without a second RWP read; at an idle poll RRP is written only once 3 or more pages are owed (always as one 2-byte transfer, also in the HWFIX build); the ISR service runs
       every 256th idle poll instead of every 8th (the virtual chip raises nothing after the bring-up). */
    sdnet_u8 wii_fast, rx_snap_ok, ncra_ok, ncra_sh;
    /* PV fast path (set by the engine after the bring-up when the kernel answers the PV device; bba_init_begin clears it): a send is ONE DMA ('P' 'V' len, frame), a
       receive poll ONE DMA that returns the oldest frame or 'none' (vbba_chip_pv_rx); pv_idle = the last frame came with 'nothing more waiting' (the next call returns 0
       without EXI traffic: the end of this poll) */
    sdnet_u8 pv, pv_idle;
    /* the glue's PV transfer (Wii kernels with the PV fast path only): one DMA of n bytes (any length) to / from the paravirtual device (EXI channel 2 device 1); the glue
       selects that device when needed.  0 ok, -1 not available.  Used only while pv is set. */
    int (*pv_xfer)(void *ctx, void *buf, sdnet_u32 n, int is_write);
    sdnet_u32 st_pv_tx, st_pv_rx, st_pv_err;
    sdnet_u8 pv_why, pv_bad0, pv_bad1, pv_dump[8];   /* pv_dump: the first 8 bytes of that bad answer (FC words 123 / 124) */   /* why the PV path was left (1 send transfer refused, 2 receive transfer refused, 3 answer without the 'PV' header: its first two bytes, 4 long frame inconsistent) */
    sdnet_u16 rx_snap;
    /* non-blocking bring-up state (bba_init_begin / bba_init_step) */
    sdnet_u8 ip_phase, ip_have_mac, ip_mac[6];
    sdnet_u32 ip_wake, ip_t0;
#ifdef BBA_HWFIX
    struct bba_diag dg;
    sdnet_u16 id_left;            /* ID read retries left */
#endif
    /* DMA bounce buffer, keep 32-byte alignment of the whole struct (use bba_alloc_align) */
    sdnet_u8 dmabuf[BBA_DMA_BUF + 64] __attribute__((aligned(32)));
};

/* Return codes */
#define BBA_OK 0
#define BBA_E_NODEV (-1)          /* EXI ID is not 0x04020200 */
#define BBA_E_RESET (-2)
#define BBA_E_LINK (-3)
#define BBA_E_TXTIMEOUT (-4)
#define BBA_E_PARAM (-5)
#define BBA_E_SELECT (-6)
#define BBA_E_RING (-8)           /* bba_recv: descriptor chain did not terminate; ring resynchronised (counted in st.rx_walk_abort) */
#define BBA_PENDING 1             /* bba_init_step: not finished, call again later */
#define BBA_E_AUTH (-7)           /* strict mode: no challenge/response within BBA_AUTH_TIMEOUT_MS */

/* how long bba_init waits for the chip's challenge (ISR 0x10) and verdict (0x08) */
#define BBA_AUTH_TIMEOUT_MS 500

/* Reads the EXI ID through the ops (0 if select failed). */
sdnet_u32 bba_read_id(struct bba *b);

/* Reset and bring the chip up (accept unicast to our MAC + broadcast, start RX).
   mac == 0: use the address the chip came up with (read from PAR0-5).
   Does not wait for link; call bba_link_wait. */
int bba_init(struct bba *b, const struct bba_exi_ops *ops, void *ctx,
             const sdnet_u8 *mac);

/* Non-blocking variant of bba_init for callers that must not stall (e.g. a per-frame hook inside a game).
   bba_init_begin does no I/O (BBA_OK or BBA_E_PARAM).  Call bba_init_step(b, now_ms) repeatedly from a
   millisecond clock (wraps ok); it returns BBA_PENDING while it waits (settle delays, challenge window)
   and then BBA_OK / BBA_E_NODEV / BBA_E_AUTH.  The caller holds the EXI channel around each step. */
int bba_init_begin(struct bba *b, const struct bba_exi_ops *ops, void *ctx,
                   const sdnet_u8 *mac);
int bba_init_step(struct bba *b, sdnet_u32 now_ms);

/* Services the EXI-side ISR once: answers a challenge (0x10), notes the verdict (0x08),
   flags a reset request (0x40), acks what it handled. bba_recv calls this every few idle
   polls; returns the ISR value seen. */
int bba_service(struct bba *b);

/* Response function of the challenge (exported for tests): 32-bit challenge, chip revid,
   16-bit device id -> 32-bit response. */
sdnet_u32 bba_auth_response(sdnet_u32 challenge, sdnet_u8 revid, sdnet_u16 devid);

#ifdef BBA_HWFIX
/* Records one NWAYS sample for the status page (the glue's link wait calls it with what it read). */
void bba_note_link(struct bba *b, sdnet_u8 nways, sdnet_u32 now_ms);
#endif

/* Polls NWAYS until a link bit is set or timeout_ms (needs ops->delay_us). */
int bba_link_wait(struct bba *b, sdnet_u32 timeout_ms);

/* sdnet NIC glue (frame without FCS, padded to 60 inside send). */
void bba_make_nic(struct bba *b, struct sdnet_nic *nic);

int bba_send(struct bba *b, const sdnet_u8 *frame, sdnet_u16 len);
int bba_recv(struct bba *b, sdnet_u8 *buf, sdnet_u16 max);

/* raw register access (exported for tests/diagnostics) */
sdnet_u8 bba_reg_in8(struct bba *b, sdnet_u16 reg);
void bba_reg_out8(struct bba *b, sdnet_u16 reg, sdnet_u8 v);

#endif
