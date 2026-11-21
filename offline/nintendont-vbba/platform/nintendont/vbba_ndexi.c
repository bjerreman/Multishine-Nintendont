/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* vbba_ndexi: see vbba_ndexi.h. */
#include "vbba_ndexi.h"

int vbba_ndexi_select(struct vbba_chip *c, struct vbba_ndexi_stats *s, vbba_u32 chn, vbba_u32 dev)
{
    if (!((chn == 0 && dev == 2) || (chn == 2 && dev == 0))) { s->other_dev++; return 0; }      /* channel 0 device 2 as on a GameCube, or channel 2 device 0 (the engine's choice on a Wii whose kernel carries the BCH2 marker, ALPHA_OPEN_FINDINGS 51) */
    s->selects++;
    vbba_chip_select(c);              /* also ends the previous transaction: deselects never reach the kernel */
    return 1;
}

vbba_u32 vbba_ndexi_imm(struct vbba_chip *c, struct vbba_ndexi_stats *s, vbba_u32 data, vbba_u32 len, vbba_u32 mode)
{
    vbba_u8 b[4];
    vbba_u32 i, v = 0;
    if (len < 1 || len > 4) return 0;
    if (mode == 1) {
        for (i = 0; i < len; i++) b[i] = (vbba_u8)(data >> (24 - 8 * i));
        vbba_chip_xfer(c, b, len, 1);
        s->imm_w++;
        return 0;
    }
    vbba_chip_xfer(c, b, len, 0);
    for (i = 0; i < len; i++) v = (v << 8) | b[i];   /* right-aligned: first wire byte is the most significant of len */
    s->imm_r++;
    if (len == 3) s->imm_r3++;        /* the stock stub drops this result (see the header) */
    return v;
}

void vbba_ndexi_dma(struct vbba_chip *c, struct vbba_ndexi_stats *s, vbba_u8 *buf, vbba_u32 len, vbba_u32 mode)
{
    vbba_chip_xfer(c, buf, len, mode == 1);
    if (mode == 1) s->dma_w++; else s->dma_r++;
    s->dma_bytes += len;
}
