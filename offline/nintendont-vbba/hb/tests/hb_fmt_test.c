/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of the q33-diag heartbeat format (hb/ms_hb_fmt.h): writes a synthetic /hb.log whose contents the project's other tooling checks with the decoder the project's other tooling.
 *   cc -std=c89 -pedantic -Wall -Wextra -Werror -o hb_fmt_test hb_fmt_test.c && ./hb_fmt_test OUT_FILE SCENARIO
 * SCENARIO: healthy | main_sd | main_ios | power | wrap
 *   healthy   live block, 10 records, everything moving, no snapshot
 *   main_sd   the main loop stops at pass 500 inside an SD READ (gate in 1, the op 12 s old at the last record); snapshot 1 = MAIN5, block 2 = RETR6
 *   main_ios  the main loop stops inside IOS_Ioctlv on fd 7 (/dev/net/ip/top), the SD idle; snapshot MAIN5; power button seen, no Shutdown
 *   power     healthy until the power press, then a SHUTDOWN block (the press was handled)
 *   wrap      60 records written to the ring: the block carries the newest 28, oldest first */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ms_hb_fmt.h"

static hb_u32 ring[HB_RECS * HB_REC_WORDS], hdr[HB_HDR_WORDS], total;
static unsigned char blk[HB_BLOCKS][HB_BLOCK];

static void rec(hb_u32 kind, hb_u32 tick, hb_u32 mainp, hb_u32 retrace, hb_u32 mop, hb_u32 mop_t0, hb_u32 sd, hb_u32 sd_t0)
{
	hb_u32 *r = ring + (total % HB_RECS) * HB_REC_WORDS;
	memset(r, 0, HB_REC_WORDS * 4);
	r[HB_R_MAGIC] = HB_MAGIC_REC; r[HB_R_SEQ] = total; r[HB_R_TICK] = tick; r[HB_R_KIND] = kind;
	r[HB_R_MAIN] = mainp; r[HB_R_STAGE] = 2; r[HB_R_STAGE_T] = mop_t0 ? mop_t0 : tick; r[HB_R_CRUMBS] = mainp * 9u;
	r[HB_R_MOP] = mop; r[HB_R_MOP_T0] = mop_t0; r[HB_R_LOP] = mop ? mop : HB_OPID(HB_OP_IOCTLV, 3, 7); r[HB_R_LOP_TID] = 1; r[HB_R_LOP_T] = mop_t0 ? mop_t0 : tick;
	r[HB_R_SD] = sd; r[HB_R_SD_T0] = sd_t0; r[HB_R_SD_SEC] = 123456; r[HB_R_SDOPS] = 1000 + mainp; r[HB_R_SDERR] = 0;
	r[HB_R_EXI0] = 13372 + mainp; r[HB_R_EXI1] = 3; r[HB_R_EXI2] = mainp * 2u; r[HB_R_RETRACE] = retrace;
	r[HB_R_BC] = 0x11; r[HB_R_BC_TBL] = tick * 3u; r[HB_R_SCENE] = 0x00000001; r[HB_R_DISP] = retrace * 4u;
	r[HB_R_VBBA] = 0xC0000000u | mainp; r[HB_R_VQ] = (2u << 16) | 1u; r[HB_R_VTX] = 0; r[HB_R_DI_OPS] = mainp / 2u; r[HB_R_NET_OPS] = 40; r[HB_R_WR] = (total << 16) | total;
	total++;
}

static void header(hb_u32 kind, hb_u32 wseq, hb_u32 tick)
{
	hb_u32 i;
	memset(hdr, 0, sizeof hdr);
	hdr[HB_H_MAGIC] = HB_MAGIC_BLK; hdr[HB_H_VERSION] = HB_VERSION; hdr[HB_H_KIND] = kind; hdr[HB_H_WSEQ] = wseq; hdr[HB_H_TICK] = tick; hdr[HB_H_BOOT] = 1000;
	hdr[HB_H_RECSZ] = HB_REC_WORDS * 4u; hdr[HB_H_RECCAP] = HB_RECS; hdr[HB_H_LBA] = 777216; hdr[HB_H_CSIZE] = 64;
	hdr[HB_H_TID + 0] = 1; hdr[HB_H_TID + 1] = 2; hdr[HB_H_TID + 2] = 5; hdr[HB_H_TID + 6] = 9; hdr[HB_H_TID + 7] = 10;
	hdr[HB_H_WR + 0] = wseq; hdr[HB_H_WR + 1] = wseq - 1u; hdr[HB_H_WR + 3] = 5000;
	hdr[HB_H_PRIO] = (0x7C << 8) | 0x7A; hdr[HB_H_STK] = (600u << 16) | 900u;
	/* thread slots: main (tid 1), DI (tid 2), net (tid 5) */
	hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_TID] = 1; hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_NOPS] = 4321;
	hdr[HB_H_SLOTS + 1 * HB_SLOT_WORDS + HB_S_TID] = 2; hdr[HB_H_SLOTS + 1 * HB_SLOT_WORDS + HB_S_NOPS] = 999;
	hdr[HB_H_SLOTS + 2 * HB_SLOT_WORDS + HB_S_TID] = 5; hdr[HB_H_SLOTS + 2 * HB_SLOT_WORDS + HB_S_NOPS] = 40;
	/* fd table: 3 = /dev/sdio/slot0, 7 = /dev/net/ip/top (stored without '/dev/') */
	{
		static const char *nm[2] = { "sdio/slot0", "net/ip/top" };   /* the kernel drops "/dev/" */
		static const hb_u32 fd[2] = { 3, 7 };
		for (i = 0; i < 2; i++) {
			const char *s = nm[i]; size_t n = strlen(s), j; unsigned char b[12];
			const char *t = n > 12 ? s + n - 12 : s;
			for (j = 0; j < 12; j++) b[j] = (unsigned char)(j < strlen(t) ? t[j] : 0);
			hdr[HB_H_FDS + i * 4] = fd[i];
			for (j = 0; j < 3; j++) hdr[HB_H_FDS + i * 4 + 1 + j] = ((hb_u32)b[4 * j] << 24) | ((hb_u32)b[4 * j + 1] << 16) | ((hb_u32)b[4 * j + 2] << 8) | b[4 * j + 3];
		}
	}
}

static void unused_blocks(void)
{
	hb_u32 b;
	for (b = 0; b < HB_BLOCKS; b++) {
		hb_u32 t = 0;
		memset(hdr, 0, sizeof hdr);
		hdr[HB_H_MAGIC] = HB_MAGIC_BLK; hdr[HB_H_VERSION] = HB_VERSION; hdr[HB_H_KIND] = 0x100u + b; hdr[HB_H_BOOT] = 1000;
		hb_block_build(blk[b], hdr, ring, t);
	}
}

int main(int argc, char **argv)
{
	FILE *f;
	hb_u32 i, T = HB_TICKS_S;
	const char *sc;
	if (argc != 3) { fprintf(stderr, "usage: %s OUT SCENARIO\n", argv[0]); return 2; }
	sc = argv[2];
	unused_blocks();
	if (!strcmp(sc, "healthy")) {
		for (i = 0; i < 10; i++) rec(i ? HB_K_PERIODIC : HB_K_BOOT, 1000 + i * T, 100 * i, 60 * i, 0, 0, 0, 0);
		header(0, 5, 1000 + 10 * T); hb_block_build(blk[0], hdr, ring, total);
	} else if (!strcmp(sc, "main_sd")) {
		hb_u32 t_stuck = 1000 + 5 * T - 100, sdop = HB_OPID(HB_OP_IOCTLV, 3, 7);
		for (i = 0; i < 5; i++) rec(HB_K_PERIODIC, 1000 + i * T, 100 * i, 60 * i, 0, 0, 0, 0);
		for (i = 5; i < 18; i++) {
			hb_u32 k = i == 11 ? HB_K_MAIN5 : i == 12 ? HB_K_RETR6 : HB_K_PERIODIC;
			rec(k, 1000 + i * T, 500, 300, sdop, t_stuck, (1u << 24) | (1u << 16) | 64u, t_stuck);
			if (i == 11) { header(HB_K_MAIN5, 6, 1000 + i * T); hb_block_build(blk[1], hdr, ring, total); }
			if (i == 12) { header(HB_K_RETR6, 7, 1000 + i * T); hb_block_build(blk[2], hdr, ring, total); }
		}
		header(0, 9, 1000 + 17 * T);
		hdr[HB_H_SD + 0] = 1; hdr[HB_H_SD + 1] = 1; hdr[HB_H_SD + 2] = 123456; hdr[HB_H_SD + 3] = 64; hdr[HB_H_SD + 4] = t_stuck; hdr[HB_H_SD + 5] = 1; hdr[HB_H_SD + 6] = 4000;
		hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_IOP] = sdop; hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_IT0] = t_stuck;
		hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_FOP] = HB_OPID(HB_OP_FREAD, 0, 64); hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_FT0] = t_stuck - 50;
		hdr[HB_H_WR + 6] = 3;   /* forced writes past the stuck read */
		hdr[HB_H_SNAPS] = 2;
		hb_block_build(blk[0], hdr, ring, total);
	} else if (!strcmp(sc, "main_ios")) {
		hb_u32 t_stuck = 1000 + 4 * T + 7, op = HB_OPID(HB_OP_IOCTLV, 7, 0x0C);
		for (i = 0; i < 4; i++) rec(HB_K_PERIODIC, 1000 + i * T, 100 * i, 60 * i, 0, 0, 0, 0);
		for (i = 4; i < 12; i++) rec(i == 10 ? HB_K_MAIN5 : HB_K_PERIODIC, 1000 + i * T, 400, 60 * i, op, t_stuck, 0, 0);
		header(HB_K_MAIN5, 6, 1000 + 10 * T); hb_block_build(blk[1], hdr, ring, total);
		rec(HB_K_POWER, 1000 + 12 * T, 400, 60 * 12, op, t_stuck, 0, 0);
		header(HB_K_POWER, 7, 1000 + 12 * T); hdr[HB_H_FLAGS] = 8; hb_block_build(blk[2], hdr, ring, total);
		header(0, 8, 1000 + 12 * T + 100); hdr[HB_H_FLAGS] = 8; hdr[HB_H_SNAPS] = 2;
		hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_IOP] = op; hdr[HB_H_SLOTS + 0 * HB_SLOT_WORDS + HB_S_IT0] = t_stuck;
		hb_block_build(blk[0], hdr, ring, total);
	} else if (!strcmp(sc, "power")) {
		for (i = 0; i < 6; i++) rec(HB_K_PERIODIC, 1000 + i * T, 100 * i, 60 * i, 0, 0, 0, 0);
		rec(HB_K_POWER, 1000 + 6 * T, 600, 360, 0, 0, 0, 0);
		header(HB_K_POWER, 4, 1000 + 6 * T); hdr[HB_H_FLAGS] = 8; hb_block_build(blk[1], hdr, ring, total);
		rec(HB_K_SHUTDOWN, 1000 + 6 * T + 300, 601, 361, 0, 0, 0, 0);
		header(HB_K_SHUTDOWN, 5, 1000 + 6 * T + 300); hdr[HB_H_FLAGS] = 9; hb_block_build(blk[0], hdr, ring, total);
	} else if (!strcmp(sc, "wrap")) {
		for (i = 0; i < 60; i++) rec(HB_K_PERIODIC, 1000 + i * T, 100 * i, 60 * i, 0, 0, 0, 0);
		header(0, 30, 1000 + 60 * T); hb_block_build(blk[0], hdr, ring, total);
	} else { fprintf(stderr, "unknown scenario %s\n", sc); return 2; }
	f = fopen(argv[1], "wb");
	if (!f) { perror(argv[1]); return 1; }
	if (fwrite(blk, 1, sizeof blk, f) != sizeof blk) { perror("write"); return 1; }
	fclose(f);
	return 0;
}
