/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_driveprobe.h (tests/test_driveprobe.py runs it): prints OK */
#include <stdio.h>
#include "ms_driveprobe.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define INIT 2u
/* run a status sequence polled every 100 ms: seq[i] is the status at i*100 ms; the last value repeats; -> the result, *at = elapsed ms of the stop */
static int run(const unsigned *seq, int n, int b_at, unsigned *at)
{
	struct msdp_state s; unsigned t; int r;
	msdp_begin(&s);
	for (t = 0;; t += MSDP_POLL_MS) {
		unsigned i = t / MSDP_POLL_MS;
		r = msdp_step(&s, seq[i < (unsigned)n ? i : (unsigned)n - 1], INIT, t, b_at >= 0 && t >= (unsigned)b_at);
		if (r != MSDP_CONTINUE || t > 10000) { *at = t; return r; }
	}
}
int main(void)
{
	unsigned at; int r;
	{ const unsigned q[] = { 2 }; r = run(q, 1, -1, &at); CHECK(r == MSDP_STOP_NODISC); CHECK(at == 800); }                  /* no disc: stuck at 2 -> out after 800 ms, not 4000 */
	{ const unsigned q[] = { 2, 2, 2, 2, 2, 2, 2, 0 }; r = run(q, 8, -1, &at); CHECK(r == MSDP_DONE); CHECK(at == 700); }   /* ready at 700 ms */
	{ const unsigned q[] = { 2, 2, 2, 3, 2, 2, 2 }; r = run(q, 7, -1, &at); CHECK(r == MSDP_STOP_NODISC); CHECK(at == 1200); }   /* a change restarts the count: 2 again from 400 ms (3 at 300) */
	{ unsigned q[50], i; for (i = 0; i < 50; i++) q[i] = 2u | (i & 1 ? 4u : 8u); r = run(q, 50, -1, &at); CHECK(r == MSDP_STOP_TIMEOUT); CHECK(at == 4000); }   /* spinning up / reading (changing, not init-only): the full 4 s */
	{ const unsigned q[] = { 6 }; r = run(q, 1, -1, &at); CHECK(r == MSDP_STOP_TIMEOUT); CHECK(at == 4000); }                    /* init plus another bit, steady: not 'no disc', old bound */
	{ const unsigned q[] = { 2 }; r = run(q, 1, 300, &at); CHECK(r == MSDP_STOP_B); CHECK(at == 300); }                         /* B stops at once */
	{ const unsigned q[] = { 1 }; r = run(q, 1, -1, &at); CHECK(r == MSDP_DONE); CHECK(at == 0); }                              /* a disc already ready: no wait (unchanged behaviour) */
	puts(fails ? "FAILED" : "OK");
	return fails != 0;
}
