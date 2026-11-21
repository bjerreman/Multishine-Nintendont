/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* When the physical-disc probe (drive_probe in ms_ota_loader.c) stops waiting for the drive: pure decision, host-tested (tests/driveprobe_test.c).
 * The drive reports DI status DVD_INIT while it starts.  With no disc (or an open cover) it stays at exactly that value for ever, which cost the full 4 s at every start (ota.log: 'drive: not ready after
 * 4002 ms ... status 00000002').  So: the status is polled about every 100 ms; the SAME 'initialising only' value for MSDP_NODISC_MS in a row ends the wait ('no disc').  Any change of the status (the drive is
 * spinning up, reading) restarts that count, and the full MSDP_MAX_MS stays as the limit while it changes.  B stops it at once (as before). */
#ifndef MS_DRIVEPROBE_H
#define MS_DRIVEPROBE_H
#define MSDP_MAX_MS    4000u    /* the old bound */
#define MSDP_NODISC_MS 800u     /* the same 'init only' status for this long = no disc */
#define MSDP_POLL_MS   100u
enum { MSDP_CONTINUE = 0, MSDP_DONE = 1 /* no longer initialising: the caller checks READY */, MSDP_STOP_B = 2, MSDP_STOP_TIMEOUT = 3, MSDP_STOP_NODISC = 4 };

struct msdp_state { unsigned last, since; int have; };
static inline void msdp_begin(struct msdp_state *s) { s->last = 0; s->since = 0; s->have = 0; }
/* one poll: status = DI_GetStatus, init_bit = DVD_INIT, elapsed = ms since the wait began, b = B pressed */
static inline int msdp_step(struct msdp_state *s, unsigned status, unsigned init_bit, unsigned elapsed, int b)
{
	if (b) return MSDP_STOP_B;
	if (!(status & init_bit)) return MSDP_DONE;
	if (!s->have || s->last != status) { s->have = 1; s->last = status; s->since = elapsed; }
	if (status == init_bit && elapsed - s->since >= MSDP_NODISC_MS) return MSDP_STOP_NODISC;
	if (elapsed >= MSDP_MAX_MS) return MSDP_STOP_TIMEOUT;
	return MSDP_CONTINUE;
}
#endif
