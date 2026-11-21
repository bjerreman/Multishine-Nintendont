/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_progress.h (tests/test_theme.py runs it): prints "OK" or the first failure. */
#include <stdio.h>
#include "ms_progress.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
	unsigned cur = 0, v, s;
	int err, i;
	int seq[] = { 0, 0, 1, 1, 2, 3, 4, 5, 0, 2, 3, 3, 7, 6, 9, 10, 11, 11, 4, 12, 40 };
	/* steps rise strictly and stay below the stage range */
	for (i = 1; i < MSP_N; i++) CHECK(msp_step(i) > msp_step(i - 1));
	CHECK(msp_step(MSP_HANDOVER) <= msp_stage(0));
	CHECK(msp_stage(MSP_STAGE_MAX) < MSP_DONE);
	for (s = 1; s <= MSP_STAGE_MAX; s++) CHECK(msp_stage(s) > msp_stage(s - 1));
	CHECK(msp_stage(500) == msp_stage(MSP_STAGE_MAX));
	/* any mix of statuses never moves the bar back; a fatal error freezes it; the kernel's final value completes it */
	for (i = 0; i < (int)(sizeof seq / sizeof seq[0]); i++) {
		v = msp_status(cur, seq[i], &err);
		CHECK(!err && v >= cur && v <= MSP_DONE);
		cur = v;
	}
	CHECK(cur == msp_stage(MSP_STAGE_MAX));
	for (i = -2; i > -20; i--) { v = msp_status(cur, i, &err); CHECK(err == 1 && v == cur); }
	v = msp_status(cur, -1, &err); CHECK(err == 0 && v >= cur);               /* -1 is the kernel's 'loading' marker, not an error */
	v = msp_status(cur, -20, &err); CHECK(err == 0 && v >= cur);              /* outside the error range */
	v = msp_status(100, (int)0xdeadbeefu, &err); CHECK(v == MSP_DONE && err == 0);
	CHECK(msp_advance(500, 300) == 500 && msp_advance(300, 500) == 500 && msp_advance(0, 5000) == MSP_DONE);
	CHECK(msp_step(-1) == 0 && msp_step(99) == 0);
	/* the pill: symmetric rows, rounder toward the corners, edge alpha in range, never narrower than tall, never wider than the track */
	for (i = 0; i < 12; i++) {
		int in, a, in2, a2;
		msp_pill_row(12, i, 1.0f, &in, &a); msp_pill_row(12, 11 - i, 1.0f, &in2, &a2);
		CHECK(in == in2 && a == a2 && a >= 0 && a <= 255 && in >= 0 && in < 6);
		if (i > 0 && i < 6) { int p, pa; msp_pill_row(12, i - 1, 1.0f, &p, &pa); CHECK(p >= in); }
	}
	{ int in, a; msp_pill_row(12, 0, 1.0f, &in, &a); CHECK(in >= 2); msp_pill_row(12, 5, 1.0f, &in, &a); CHECK(in == 0); }
	CHECK(msp_fill_w(360, 0, 12) == 12 && msp_fill_w(360, 1, 12) == 12 && msp_fill_w(360, 500, 12) == 180 && msp_fill_w(360, 1000, 12) == 360 && msp_fill_w(360, 5000, 12) == 360 && msp_fill_w(300, 1, 10) == 10);
	for (v = 0; v <= 1000; v += 7) CHECK(msp_fill_w(270, v, 12) >= 12 && msp_fill_w(270, v, 12) <= 270);
		/* squeezed picture: the end is as many pixels deep as the squeeze says, so it still looks round (xs 0.84375: the 6 px radius becomes 5 px) */
	{ int in1, a1, in2, a2; msp_pill_row(12, 0, 1.0f, &in1, &a1); msp_pill_row(12, 0, 0.84375f, &in2, &a2); CHECK(in2 <= in1 && in2 >= 1); }
	/* the compensation from the real mode: the loader's modes are all 640 x 480 framebuffers in a 640 column picture */
	CHECK(msp_comp_permille(640, 640, 480, 1) == 844u);                          /* TVNtsc480Prog / Int, TVEurgb60Hz480Int, TVMpal480IntDf, TVPal576IntDfScale: 27/32 */
	CHECK(msp_comp_permille(720, 640, 480, 1) == 750u);                          /* Video Width at its maximum: the old fixed 3/4 */
	CHECK(msp_comp_permille(680, 640, 480, 1) > 750u && msp_comp_permille(680, 640, 480, 1) < 844u);
	CHECK(msp_comp_permille(640, 640, 480, 0) == 1000u && msp_comp_permille(0, 640, 480, 1) == 1000u);
	for (i = 640; i < 720; i++) CHECK(msp_comp_permille((unsigned)i, 640, 480, 1) >= msp_comp_permille((unsigned)i + 1, 640, 480, 1));   /* wider picture, smaller factor */
	printf("OK\n");
	return 0;
}
