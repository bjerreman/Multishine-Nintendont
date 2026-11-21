/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* host test of the stall watchdog's decisions (kernel/vbba_stall.h, taken from patch 0015).
   Build: cc -std=c99 -Wall -fsanitize=address,undefined -o /tmp/stall_watch_test tests/stall_watch_test.c  (the header is extracted from the patch by run_tests.sh) */
#include <stdio.h>
#include <stdint.h>
typedef uint32_t u32;
#define MS_STALL_HOST 1
static u32 fake_timer;
#define HW_TIMER 0
static u32 read32(u32 a) { (void)a; return fake_timer; }
#include "stall_hdr.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
	u32 st;
	/* modes: moving below 3 s, stuck outside FatFs = 1, inside FatFs (ISO read, card save, log open/write/sync) = 2 */
	CHECK(ms_stall_mode(0, MSS_VREOPEN) == 0);
	CHECK(ms_stall_mode(2999, MSS_VREOPEN) == 0);
	CHECK(ms_stall_mode(3000, MSS_VREOPEN) == 1);
	CHECK(ms_stall_mode(3000, MSS_WAIT) == 1);
	CHECK(ms_stall_mode(9000, MSS_EXI) == 1);
	CHECK(ms_stall_mode(3000, MSS_DI) == 2);
	CHECK(ms_stall_mode(3000, MSS_SAVE) == 2);
	CHECK(ms_stall_mode(3000, MSS_LOG_OPEN) == 2);
	CHECK(ms_stall_mode(3000, MSS_LOG_WRITE) == 2);
	CHECK(ms_stall_mode(3000, MSS_LOG_SYNC) == 2);
	for (st = 1; st < 40; st++) { int fat = st == MSS_DI || st == MSS_SAVE || (st >= 20 && st <= 22); CHECK(MSS_IS_FAT(st) == fat); }
	/* the watchdog never logs inside FatFs and never while moving */
	CHECK(!ms_stall_log_due(0, 5000, 0));
	CHECK(!ms_stall_log_due(2, 8000, 0));
	/* first line at the first check of a stall, next only 5 s later, none after 12 lines (60 s) */
	CHECK(ms_stall_log_due(1, 3000, 0));
	CHECK(!ms_stall_log_due(1, 3200, 3000));
	CHECK(!ms_stall_log_due(1, 7999, 3000));
	CHECK(ms_stall_log_due(1, 8000, 3000));
	CHECK(ms_stall_log_due(1, 53000, 48000));
	CHECK(!ms_stall_log_due(1, 66000, 60000));
	{   /* a whole stall with a 200 ms watchdog: lines at 3.0, 8.0, 13.0 ... s, capped */
		u32 logged = 0, age, lines = 0;
		for (age = 0; age <= 120000; age += 200) { u32 m = ms_stall_mode(age, MSS_VREOPEN); if (ms_stall_log_due(m, age, logged)) { logged = age; lines++; } }
		CHECK(lines >= 11 && lines <= 13);
	}
	/* MS_ST: stores stage, time and a count; the watchdog flag freezes the crumbs */
	{
		fake_timer = 1234; MS_ST(MSS_EXI);
		CHECK(msCrumb.stage == MSS_EXI && msCrumb.t == 1234 && msCrumb.n == 1);
		msCrumb.wd = 1; fake_timer = 99; MS_ST(MSS_SAVE);
		CHECK(msCrumb.stage == MSS_EXI && msCrumb.t == 1234 && msCrumb.n == 1);
		msCrumb.wd = 0; MS_ST(MSS_SAVE);
		CHECK(msCrumb.stage == MSS_SAVE && msCrumb.t == 99 && msCrumb.n == 2);
	}
	printf("stall_watch_test: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
struct ms_crumb msCrumb;
