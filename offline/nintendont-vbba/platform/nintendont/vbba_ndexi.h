/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_ndexi: Nintendont's EXI HLE conventions -> vbba_chip.  Behaviour (not code) of Nintendont bbc0a20 as read in
 * kernel/EXI.c (EXIUpdateRegistersNEW: one command per main-loop pass in EXI_CMD_0/EXI_CMD_1) and kernel/asm/EXI*.S:
 *   EXISelect  -> cmd 0x10 | dev << 8 | chn: answer 1 in EXI_CMD_1 when present.     Starts a transaction.
 *   EXIImm     -> cmd 0x11: write mode: EXI_CMD_1 holds the full word loaded from the game's buffer (first byte in
 *                 bits 31..24, bytes beyond len are garbage); read mode: the kernel puts the result in EXI_CMD_1 and
 *                 the stub stores it with stb / sth / stw for len 1 / 2 / 4, i.e. RIGHT-aligned; len 3 is NOT stored.
 *   EXIDMA     -> cmd 0x12: EXI_CMD_1 = physical buffer; the kernel copies directly (cache maintenance is the glue's).
 *   EXIDeselect, EXISync, EXILock, EXIUnlock -> replaced by "li r3,1; blr": the kernel never sees them.
 * This file has no Nintendont code and no Nintendont headers; the GPL glue (kernel/vbba_glue.c in a future patch)
 * calls these three functions from its EXI route for (chn 0, dev 2).
 */
#ifndef VBBA_NDEXI_H
#define VBBA_NDEXI_H

#include "../../core/vbba_chip.h"

struct vbba_ndexi_stats { vbba_u32 selects, imm_w, imm_r, imm_r3, dma_w, dma_r, dma_bytes, other_dev; };

/* EXISelect(chn, dev): returns 1 if the vBBA answers this select (chn 0, dev 2) and starts a transaction */
int vbba_ndexi_select(struct vbba_chip *c, struct vbba_ndexi_stats *s, vbba_u32 chn, vbba_u32 dev);
/* EXIImm on the selected vBBA: mode 1 write (data = EXI_CMD_1), 0 read; returns the value to put in EXI_CMD_1
   (right-aligned for reads, as the stub expects) */
vbba_u32 vbba_ndexi_imm(struct vbba_chip *c, struct vbba_ndexi_stats *s, vbba_u32 data, vbba_u32 len, vbba_u32 mode);
/* EXIDMA on the selected vBBA: buf = the game's buffer in ARM view (already invalidated for a write / to be flushed
   after a read by the glue) */
void vbba_ndexi_dma(struct vbba_chip *c, struct vbba_ndexi_stats *s, vbba_u8 *buf, vbba_u32 len, vbba_u32 mode);

#endif
