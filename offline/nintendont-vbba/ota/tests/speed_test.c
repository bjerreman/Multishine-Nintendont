/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_speed.h (tests/test_speed.py runs it): prints OK */
#include <stdio.h>
#include <string.h>
#include "ms_speed.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
	char b[32];
	CHECK(msspd_kibs(0, 100) == 0);
	CHECK(msspd_kibs(8u << 20, 1000) == 8192);                       /* 8 MiB in 1 s = 8192 KiB/s */
	CHECK(msspd_kibs(1u << 20, 0) == 1024000);                       /* 0 ms counts as 1 ms */
	CHECK(msspd_kibs(3ul << 20, 1000) == 3072);
	CHECK(msspd_iops(0, 100) == 0 && msspd_iops(500, 1000) == 500 && msspd_iops(10, 0) == 10000);
	CHECK(msspd_lat_us(0, 5) == 0 && msspd_lat_us(500, 1000) == 2000);
	{ unsigned s = 1, i; for (i = 0; i < 100000; i++) { unsigned long o = msspd_rnd_off(&s, 1400ul << 20, MSSPD_LARGE, 2048); CHECK(o % 2048 == 0 && o <= (1400ul << 20) - MSSPD_LARGE); } }
	{ unsigned s = 7; CHECK(msspd_rnd_off(&s, 100, 4096, 2048) == 0); }
	{ unsigned s = 3, i, hi = 0; for (i = 0; i < 1000; i++) if (msspd_rnd_off(&s, 1000ul << 20, 4096, 2048) > (500ul << 20)) hi++; CHECK(hi > 300 && hi < 700); }   /* spread over the whole file */
	msspd_fmt(b, sizeof b, 0);     CHECK(!strcmp(b, "-"));
	msspd_fmt(b, sizeof b, 3072);  CHECK(!strcmp(b, "3.0 MB/s"));
	msspd_fmt(b, sizeof b, 12595); CHECK(!strcmp(b, "12.2 MB/s"));
	msspd_fmt(b, sizeof b, 100);   CHECK(!strcmp(b, "0.0 MB/s"));
	CHECK(msspd_fmt(b, 5, 3072) == 8 && !strcmp(b, "3.0 "));          /* truncated, still terminated, length is the full one */
	puts(fails ? "FAILED" : "OK");
	return fails != 0;
}
