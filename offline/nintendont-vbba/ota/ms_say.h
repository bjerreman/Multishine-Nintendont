/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The scrolling status window of the loader's multi-line screens (Update, Virtual card, physical card; sayf in ms_ota_loader.c): pure, host-tested (tests/say_test.c).
 * The lines are kept in a ring of MSS_RING lines; only the last `cap` lines that fit between the title and the logo are drawn, older ones scroll up and out. */
#ifndef MS_SAY_H
#define MS_SAY_H

#define MSS_RING   24      /* lines kept (line numbers are absolute: line i lives in slot i % MSS_RING) */
#define MSS_LEN    96      /* bytes per line incl. the NUL */
#define MSS_TOP_Y  80      /* y of the first drawn line (title text ends about y 54) */
#define MSS_PITCH  20      /* line pitch of the 16 px text */
#define MSS_TEXT_H 20      /* the text box of a line: top y .. y + 20 */
#define MSS_LIMIT  328     /* nothing is drawn below this y: the logo spans about y 340..410, 12 px margin */

/* how many lines fit: the last line's box must end at or above `limit` */
static inline int mss_cap(int top_y, int pitch, int text_h, int limit)
{
	const int n = (limit - text_h - top_y) / pitch + 1;
	return n < 1 ? 1 : n;
}

/* the first absolute line to draw when `total` lines exist and `cap` fit */
static inline int mss_first(int total, int cap) { return total > cap ? total - cap : 0; }

/* the y of absolute line i (i >= first) */
static inline int mss_y(int i, int first, int top_y, int pitch) { return top_y + (i - first) * pitch; }

/* can line `at` (0-based, absolute) still be changed in place: it exists and has not been overwritten in the ring */
static inline int mss_in_ring(int at, int total) { return at >= 0 && at < total && total - at <= MSS_RING; }

/* is the screen still a plain append (no line has scrolled out yet): then a new line can simply be drawn below the others */
static inline int mss_fits(int total, int cap) { return total <= cap; }

#endif
