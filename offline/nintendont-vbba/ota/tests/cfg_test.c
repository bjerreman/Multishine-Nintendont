/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_cfg_force.h and ms_rows.h (tests/test_cfg.py runs it; it needs the pinned Nintendont CommonConfig.h on the include path).
 *   cfg_test force IN OUT [MARKER WIIU VCARD]   read a 548 byte big-endian nincfg.bin, run ms_cfg_force, write it back
 *   cfg_test rows FL          print the rank of every row ("L r rank" / "R r rank", -1 = gone); FL = layout flags: 1 dev marker, 2 video mode forced
 *   cfg_test layout FL | order FL | step FL PART ROW DIR   the settings page layout, its screen order and the cursor step (ms_rows.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include "CommonConfig.h"
#include "ms_cfg_force.h"
#include "ms_rows.h"

static unsigned be(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static void put(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int main(int argc, char **argv)
{
	if ((argc == 4 || argc == 7) && !strcmp(argv[1], "force")) {
		unsigned char b[548];
		NIN_CFG c;
		FILE *f = fopen(argv[2], "rb");
		if (!f || fread(b, 1, sizeof b, f) != sizeof b) return 2;
		fclose(f);
		if (sizeof(NIN_CFG) != 548) { printf("sizeof NIN_CFG %zu\n", sizeof(NIN_CFG)); return 3; }
		memcpy(&c, b, sizeof c);
		c.Config = be(b + 8); c.VideoMode = be(b + 12); c.Language = be(b + 16); c.MaxPads = be(b + 532);
		ms_cfg_force(&c, argc == 7 ? atoi(argv[4]) : 0, argc == 7 ? atoi(argv[5]) : 0, argc == 7 ? atoi(argv[6]) : 0);
		memcpy(b + 275, c.CheatPath, 255);
		put(b + 8, c.Config); put(b + 12, c.VideoMode); put(b + 16, c.Language); put(b + 532, c.MaxPads); b[540] = (unsigned char)c.MemCardBlocks;
		f = fopen(argv[3], "wb");
		fwrite(b, 1, sizeof b, f);
		fclose(f);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "rows")) {            /* argv[2] = layout flags (1 dev, 2 video forced) */
		int fl = atoi(argv[2]), r;
		if (NIN_SETTINGS_LAST != MSR_LEFT_ROWS) { printf("NIN_SETTINGS_LAST %d\n", (int)NIN_SETTINGS_LAST); return 3; }
		if (NIN_SETTINGS_VIDEOMODE != 16 || NIN_CFG_BIT_AUTO_BOOT != MSR_SECOND_ROW) { printf("row numbers moved\n"); return 3; }
		for (r = 0; r < MSR_LEFT_ROWS; r++) printf("L %d %d\n", r, msr_rank(0, r, fl));
		for (r = 0; r < MSR_RIGHT_ROWS; r++) printf("R %d %d\n", r, msr_rank(1, r, fl));
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "layout")) {          /* "L|R row y col2 slot" for every visible row, then the description lines 9..18 */
		int fl = atoi(argv[2]), r, part;
		for (part = 0; part < 2; part++)
			for (r = 0; r < (part ? MSR_RIGHT_ROWS : MSR_LEFT_ROWS); r++)
				if (msr_rank(part, r, fl) >= 0) printf("%c %d %d %d %d\n", part ? 'R' : 'L', r, msr_y(part, r, fl), msr_col2(part, r, fl), msr_slot(part, r, fl));
		for (r = 9; r < 19; r++) printf("D %d %d 0 0\n", r, msr_y(2, r, fl));
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "order")) {           /* the screen order: "slot part row" */
		int fl = atoi(argv[2]), s, p, r;
		for (s = 0; msr_at(s, fl, &p, &r); s++) printf("%d %d %d\n", s, p, r);
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "step")) {            /* step FL PART ROW DIR -> "part row" */
		int np = -1, nr = -1;
		msr_step(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[2]), &np, &nr);
		printf("%d %d\n", np, nr);
		return 0;
	}
	return 2;
}
