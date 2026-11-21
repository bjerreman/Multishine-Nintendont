/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_chip: software model of the MX98728 as our driver (offline/bba/bba_mx98728.c) and libogc's gcif.c use it.
 * Written from the adapter's register and command behaviour and the driver's own wire format; no Swiss/libogc/Dolphin
 * source text.  The model is byte-stream oriented: a platform adapter turns its bus convention (Nintendont's EXI
 * HLE stubs, Cubeboot's EXI register shim, a host fake) into select / xfer / deselect calls.
 *
 *   command port   first byte bit 7 clear: 0x00 ID (04 02 02 00), 0x01 revid, 0x02/0x42 EXI IMR, 0x03/0x43 EXI ISR
 *                  (write-1-to-clear), 0x04/0x44 devid, 0x05/0x45 acstart, 0x08 challenge, 0x09/0x49 response,
 *                  0x0b authstat, 0x0f (read, 0); data from byte 2 on.
 *   register port  0x80 read / 0xC0 write, 16-bit address, one dummy byte, data from byte 4 on; the address
 *                  auto-increments except at WRTXFIFOD (0x48).  0x000-0x0FF registers, 0x100-0x10FF packet RAM
 *                  (16 pages of 256 B, page p at p * 256; page 0 overlaps the registers).
 *   RX ring        pages BP..RHBP, 4-byte little-endian descriptor (next page 12 bit, length incl. descriptor 12 bit,
 *                  status 8 bit), producer pointer RWP, consumer RRP (the guest).  One page always stays free.
 *   TX             bytes appended to the FIFO at 0x48; NCRA.ST1 sends it (handed to the frame backend at once),
 *                  ST1 clears and IR.TI is set in the same access.
 *   auth           devid 0xD107 + acstart 0x4E after a reset raise EXI ISR 0x10 with a 32-bit challenge; any 4-byte
 *                  response is accepted and answered with ISR 0x08 (VBBA_CHIP_AUTH; without it the chip never
 *                  challenges, like Dolphin's and Swiss's models).
 * No interrupts are generated: the guest polls (EXI IMR is stored and readable, nothing else).
 */
#ifndef VBBA_CHIP_H
#define VBBA_CHIP_H

#include "vbba_types.h"

#define VBBA_EXI_ID 0x04020200u
#define VBBA_PRAM 4096u
#define VBBA_FIFO_MAX 1536u            /* TX FIFO bytes accepted per frame; more = frame dropped (tx_overflow) */

#define VBBA_CHIP_AUTH 0x01u           /* run the EXI-side challenge/response (accept any answer) */
#define VBBA_CHIP_NO_PAD_CHECK 0x02u   /* forward frames shorter than 60 B (default: dropped as runts) */

struct vbba_chip_stats {
    vbba_u32 selects, bytes_in, bytes_out;
    vbba_u32 tx_frames, tx_runt, tx_overflow, tx_backend_err;
    vbba_u32 rx_frames, rx_full_waits, rx_filtered, rx_off_drop, rx_bad_ring, rx_too_big;
    vbba_u32 resets, challenges, responses, bad_cmds;
    vbba_u32 pv_tx, pv_rx, pv_rx_polls, pv_rx_short, pv_bad;   /* PV fast path (kernels with NIN_MS_PVBBA) */
};

struct vbba_chip {
    vbba_u8 regs[256];
    vbba_u8 pram[VBBA_PRAM];
    vbba_u8 fifo[VBBA_FIFO_MAX];
    vbba_u16 fifo_len;
    vbba_u8 fifo_ovf;
    /* EXI transaction */
    vbba_u8 selected, cmd;
    vbba_u16 addr;
    vbba_u32 pos;
    /* EXI side */
    vbba_u8 exi_imr, exi_isr, revid, devid_hi, devid_lo, auth_state, resp_n, flags;
    vbba_u32 challenge, rng;
    vbba_u8 mac[6];
    vbba_u8 guest_reset;               /* the guest has reset the chip at least once (it has read PAR; vbba_chip_set_mac then leaves the PAR registers alone) */
    /* RX: one frame pulled from the backend that did not fit yet */
    vbba_u16 held_len;
    vbba_u8 held[VBBA_FRAME_MAX];
    const struct vbba_frame_backend *be;
    struct vbba_chip_stats st;
};

void vbba_chip_init(struct vbba_chip *c, const vbba_u8 mac[6], vbba_u8 revid, vbba_u32 flags,
                    const struct vbba_frame_backend *be, vbba_u32 seed);
/* Change the MAC the chip presents.  Before the guest has reset the chip the PAR registers follow at once; after that they keep the value the guest
   already read and uses as its source address (the frame filter and the router's guest MAC depend on it), and the new MAC is what the next reset (the
   guest's next adapter init, e.g. Settings > Network > Reconnect) hands out. */
void vbba_chip_set_mac(struct vbba_chip *c, const vbba_u8 mac[6]);
/* EXI transaction boundary.  Select also ends a previous transaction (Nintendont never shows a deselect). */
void vbba_chip_select(struct vbba_chip *c);
void vbba_chip_deselect(struct vbba_chip *c);
/* Clock len bytes through the selected chip: is_write 1 = guest -> chip (buf read), 0 = chip -> guest (buf filled).
   Bytes outside a selection are ignored (reads return 0). */
void vbba_chip_xfer(struct vbba_chip *c, vbba_u8 *buf, vbba_u32 len, int is_write);
/* Move received frames from the backend into the ring (at most budget frames); call from the platform loop.
   Returns frames stored. */
int vbba_chip_poll(struct vbba_chip *c, int budget);

/* PV fast path (kernels built with NIN_MS_PVBBA: EXI channel 2 device 1, each DMA one whole operation, no select per operation).
   pv_tx: buf = 'P' 'V' len_hi len_lo 0 0 0 0, then the Ethernet frame (len bytes); sent exactly as an NCRA.ST1 transmit of the same FIFO bytes.  -1 = bad header.
   pv_rx: the oldest frame of the RX ring into buf (len >= 8 bytes): 'P' 'V' len_hi len_lo more 0 0 0, then the frame; len 0 = nothing waiting; more = 1 when another frame
   waits.  A frame that does not fit (len + 8 > buffer) is NOT consumed: only the header comes back (more = 1) and the guest reads again with room for it.  RRP advances in
   the chip.  Returns the frame length delivered (0 none, -1 bad buffer). */
int vbba_chip_pv_tx(struct vbba_chip *c, const vbba_u8 *buf, vbba_u32 len);
int vbba_chip_pv_rx(struct vbba_chip *c, vbba_u8 *buf, vbba_u32 len);
/* diagnostic: bytes of the last delivered frame (ring position rrp0 before the call) that differ in buf + 8; *first = first differing offset, -1 none */
vbba_u32 vbba_chip_pv_cmp(const struct vbba_chip *c, vbba_u32 rrp0, const volatile vbba_u8 *buf, vbba_u32 dlen, int *first);

/* introspection for tests / status pages */
vbba_u16 vbba_chip_rwp(const struct vbba_chip *c);
vbba_u16 vbba_chip_rrp(const struct vbba_chip *c);

#endif
