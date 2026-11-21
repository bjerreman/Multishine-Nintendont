/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_say.h (the scrolling status window); tests/test_theme.py builds and runs it. */
#include <stdio.h>
#include "ms_say.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
	const int cap = mss_cap(MSS_TOP_Y, MSS_PITCH, MSS_TEXT_H, MSS_LIMIT);
	int total, first, i, ylast;
	CHECK(cap == 12);
	CHECK(mss_cap(0, 20, 20, 10) == 1);                         /* never below one line */
	for (total = 0; total <= 40; total++) {
		first = mss_first(total, cap);
		CHECK(first >= 0 && total - first <= cap);               /* never more than fit */
		CHECK(first == 0 || total - first == cap);               /* a full window once it scrolls */
		if (total) {
			ylast = mss_y(total - 1, first, MSS_TOP_Y, MSS_PITCH);
			CHECK(ylast + MSS_TEXT_H <= MSS_LIMIT);              /* the newest line ends above the logo margin */
			CHECK(mss_y(first, first, MSS_TOP_Y, MSS_PITCH) == MSS_TOP_Y);
		}
		CHECK(mss_fits(total, cap) == (first == 0));
	}
	for (i = 0; i < 40; i++) CHECK(MSS_TOP_Y + MSS_PITCH * cap - 0 >= 0);
	/* the 14 lines of the virtual card (the progress line is index 6): scrolled by 2, the progress line is still on screen until 6 lines follow it */
	CHECK(mss_first(14, cap) == 2);
	CHECK(mss_in_ring(6, 14) && mss_in_ring(6, 6 + MSS_RING) && !mss_in_ring(6, 7 + MSS_RING));   /* in place while it is in the ring */
	CHECK(!mss_in_ring(-1, 5) && !mss_in_ring(5, 5) && mss_in_ring(4, 5));
	CHECK(MSS_RING >= 14 && MSS_LEN >= 80);
	printf(fails ? "say: %d FAILED\n" : "say: all passed\n", fails);
	return fails != 0;
}
