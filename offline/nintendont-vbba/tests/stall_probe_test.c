/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* host test of the SD probe stage (kernel/vbba_stall.h additions of patch 0021 on top of the header of patch 0015): a probe request that hangs is an SD
   stall (mode 2 = solid LED, no STALL line, as the log cannot be written); every other stage keeps its classification.  run_tests.sh extracts both headers. */
#include <stdio.h>
#include <stdint.h>
typedef uint32_t u32;
#define MS_STALL_HOST 1
static u32 fake_timer;
#define HW_TIMER 0
static u32 read32(u32 a) { (void)a; return fake_timer; }
static void use(void) { (void)read32(0); }
#include "stall_hdr_new.h"   /* the header of patch 0015 with the lines of patch 0021 in place of its MSS_IS_FAT line (run_tests.sh) */
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
	u32 st;
	use();
	CHECK(MSS_PROBE == 14);
	CHECK(MSS_IS_FAT(MSS_PROBE));
	CHECK(ms_stall_mode(2999, MSS_PROBE) == 0);
	CHECK(ms_stall_mode(3000, MSS_PROBE) == 2);
	CHECK(!ms_stall_log_due(ms_stall_mode(9000, MSS_PROBE), 9000, 0));
	for (st = 1; st < 40; st++) { int fat = st == MSS_DI || st == MSS_SAVE || st == MSS_PROBE || (st >= 20 && st <= 22); CHECK(MSS_IS_FAT(st) == fat); }
	CHECK(ms_stall_mode(3000, MSS_VREOPEN) == 1 && ms_stall_mode(3000, MSS_EXI) == 1 && ms_stall_mode(3000, MSS_SAVE) == 2);
	printf("stall_probe_test: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
struct ms_crumb msCrumb;
