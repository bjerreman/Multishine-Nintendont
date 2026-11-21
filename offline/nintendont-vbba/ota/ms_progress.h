/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The launch progress bar of the Multishine shell: the pure part, host-tested (tests/progress_test.c).
 * After A on the game list the loader goes through fixed steps and then watches the kernel's own stage counter (STATUS_LOADING); each gets a weight in permille.  The bar never goes backwards. */
#ifndef MS_PROGRESS_H
#define MS_PROGRESS_H
#include <math.h>

enum { MSP_BEGIN = 0, MSP_DISC, MSP_READ, MSP_CONFIG, MSP_HANDOVER, MSP_N };
#define MSP_DONE      1000
#define MSP_STAGE_MAX 11                  /* the kernel reports stages 0..11 (ES_Init ... Init CARD) before 0xdeadbeef */

/* permille reached when a step starts: nothing, disc lock passed, disc header read (multi-game / region check done), config and patches done, status handed to the kernel */
static inline unsigned msp_step(int step)
{
	static const unsigned w[MSP_N] = { 0, 80, 180, 300, 400 };
	return (step < 0 || step >= MSP_N) ? 0u : w[step];
}

/* the kernel's stage counter (abs value 0..11) lands between 400 and 950 */
static inline unsigned msp_stage(unsigned stage)
{
	if (stage > MSP_STAGE_MAX) stage = MSP_STAGE_MAX;
	return 400u + 50u * stage;
}

/* STATUS_LOADING: 0xdeadbeef = the kernel is running (done); -2..-19 = a fatal error (the bar stops); anything else is a stage.  -> new permille, never below cur; *error = 1 on a fatal error */
static inline unsigned msp_status(unsigned cur, int status, int *error)
{
	unsigned v;
	*error = 0;
	if ((unsigned)status == 0xdeadbeefu) return MSP_DONE;
	if (status < -1 && status > -20) { *error = 1; return cur; }
	v = msp_stage((unsigned)(status < 0 ? -status : status));
	return v > cur ? v : cur;
}

static inline unsigned msp_advance(unsigned cur, unsigned to)
{
	if (to > MSP_DONE) to = MSP_DONE;
	return to > cur ? to : cur;
}

/* the pill shape of every bar: fully rounded ends of radius h/2.  For pixel row `row` (0..h-1) of a bar h high: the inset of the first fully covered pixel from the bar's end and the
   alpha (0..255) of the edge pixel before it (anti-aliased end).  The fill is never narrower than the bar is high and never wider than the track. */
static inline float msp_pill_inset(int h, int row)
{
	const float rc = h * 0.5f, dy = row + 0.5f - rc;
	return rc - sqrtf(rc * rc - dy * dy);
}
/* the same for a bar whose horizontal pixels are xs times as wide as they are tall (xs < 1 = squeezed picture): the end stays round on the screen */
static inline void msp_pill_row(int h, int row, float xs, int *inset_px, int *edge_alpha)
{
	const float inset = msp_pill_inset(h, row) * xs;
	const int in = (int)inset;
	*inset_px = in;
	*edge_alpha = (int)((1.0f - (inset - in)) * 255.0f);
}
static inline int msp_fill_w(int track_w, unsigned permille, int min_w)
{
	int w = (int)((unsigned long)track_w * (permille > MSP_DONE ? MSP_DONE : permille) / 1000u);
	if (w < min_w) w = min_w;
	return w > track_w ? track_w : w;
}

/* The picture's horizontal compensation for a 16:9 screen, in permille, from the video mode the loader really set (libogc's GXRModeObj: viWidth = columns of the 720 column signal the
   picture occupies, fbWidth / efbHeight = the framebuffer).  A pixel is par = (viWidth / 720) * (16 / 9) * (efbHeight / fbWidth) times as wide as it is tall, so a drawing is pre-scaled by 1 / par.
   640 / 640 / 480 (TVNtsc480Prog, TVNtsc480Int, TVEurgb60Hz480Int, TVMpal480IntDf, TVPal576IntDfScale: all of them) gives 843 (27/32); the loader's Video Width setting widens viWidth to
   as much as 720, which gives the stock 750.  Not widescreen: 1000 (stock behaviour). */
static inline unsigned msp_comp_permille(unsigned vi_width, unsigned fb_width, unsigned efb_height, int wide)
{
	unsigned long long num, den;
	if (!wide || !vi_width || !fb_width || !efb_height) return 1000u;
	num = 1000ull * 720ull * 9ull * fb_width;
	den = 16ull * vi_width * efb_height;
	return (unsigned)((num + den / 2u) / den);
}
#endif
