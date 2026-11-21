/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Which rows of the loader's settings screen the Multishine shell shows: pure, host-tested (tests/cfg_test.c).
 * The loader's own row numbers stay exactly as they are (descriptions, cursor code and the value toggles all use them); a row that is "gone" is never drawn (it has no y), the rows
 * below it move up (rank), and the cursor skips it.  Left column = part 0, rows 0..19 (NIN_SETTINGS_LAST = 20); right column = part 1, rows 0..8. */
#ifndef MS_ROWS_H
#define MS_ROWS_H

#define MSR_LEFT_ROWS  20
#define MSR_RIGHT_ROWS 9

/* `fl` = layout flags: bit 0 = the dev marker (SD:/multishine/dev), bit 1 = the video mode is forced (ncfg->VideoMode & NIN_VID_FORCE: only then does the loader draw the 'Video mode' row 16).
   removed by our (never shown, dev marker or not): Cheats 0, Cheat Path 4, Max Pads 13, Language 14 / right: Patch PAL50 2, TRI Arcade 3, Wiimote CC Rumble 4, Skip IPL 5, BBA Emulation 6
   (the on/off row; Network Profile 7 is gone too since the Network screen chooses the connection) and, left, Memcard Blocks 17 / Memcard Multi 18 (the shell makes the card file itself; the config values stay as they are).
   Debug rows (shown only with SD:/multishine/dev): Debugger 1, Debugger Wait 2, OSReport 9.  Log 12 and Native Control 19 are gone for everyone (Log forced ON, Native Control set only by the SD:/multishine/native_si marker, both in ms_cfg_force.h).  Drive Access LED 11 is always shown.  Left row 16 (video mode) exists only when forced.
   Every other row the loader draws unconditionally (the Wii U rows are only greyed), so no other row can leave a blank slot. */
#define MSR_FL_DEV   1
#define MSR_FL_VFORCE 2
#define MSR_FL_VCARD  4      /* SD:/multishine/saves/GALE.raw exists */
#define MSR_FL_WIIU   8      /* the console is a Wii U / vWii */
#define MSR_WIDE_ROW  10     /* left: WiiU Widescreen, the LAST row of the screen order */
static inline int msr_gone(int part, int row, int fl)
{
	if (part == 1) return row == 2 || row == 3 || row == 4 || row == 5 || row == 6 || row == 7 || row == 8;       /* 8 = WiiU Gamepad Slot (hidden, the stored value stays); 7 = Network Profile: the Network screen (X on Settings) chooses the connection, and on a Wii U the row is greyed out anyway */
	if (row == 0 || row == 4 || row == 8 || row == 13 || row == 14 || row == 17 || row == 18) return 1;       /* 8 = Unlock Read Speed (forced on, ms_cfg_force.h) */
	if (row == 3) return (fl & MSR_FL_VCARD) != 0;       /* Memcard Emulation: forced on while the virtual card file exists, so the row is gone then (without the card file it stays: a real card needs it off) */
	if (row == 16) return !(fl & MSR_FL_VFORCE);
	if (row == 10) return !(fl & MSR_FL_WIIU);       /* WiiU Widescreen: "no effect on original Wii systems" */
	if (row == 11) return (fl & MSR_FL_WIIU) != 0;   /* Drive Access LED: "no effect on Wii U" */
	if (row == 12 || row == 19) return 1;       /* Log (forced on) and Native Control (a hidden marker decides, ms_cfg_force.h) */
	if (row == 1 || row == 2 || row == 9) return !(fl & MSR_FL_DEV);
	return 0;
}

/* how many visible rows come before `row` in its own column; -1 when the row itself is gone */
static inline int msr_rank(int part, int row, int fl)
{
	int i, n = 0;
	if (msr_gone(part, row, fl)) return -1;
	for (i = 0; i < row; i++) if (!msr_gone(part, i, fl)) n++;
	return n;
}

/* Settings page layout: ONE column of rows, 16 px apart from y MSR_BASE_Y, the last row ends above the logo (top about y 335).  Screen order: Network Profile (right row 7)
   first, Auto Boot (left row 7) second, then the other left rows in their own order, then the other right rows.  With the dev marker there are up to 19 rows: the first MSR_CAP stay in the
   column, the rest (always right-column rows) wrap to a second column at the top right and the description lines start below them.  The cursor follows this order (msr_step). */
#define MSR_BASE_Y 76
#define MSR_CAP    16
#define MSR_DESC_FIRST_LINE 9      /* the loader numbers description lines from 9 */
#define MSR_TOP_ROW 7              /* right column: Network Profile, drawn first */
#define MSR_SECOND_ROW 7           /* left column: Auto Boot (NIN_CFG_BIT_AUTO_BOOT), drawn second */

/* the k-th candidate of the screen order (gone or not): 0 = right 7, 1 = left 7, then the left rows without 7 and 10, then the right rows without 7, then left row 10 (WiiU Widescreen) last; 0 past the end */
static inline int msr_cand(int k, int *part, int *row)
{
	int r;
	if (k == 0) { *part = 1; *row = MSR_TOP_ROW; return 1; }
	if (k == 1) { *part = 0; *row = MSR_SECOND_ROW; return 1; }
	k -= 2;
	for (r = 0; r < MSR_LEFT_ROWS; r++) if (r != MSR_SECOND_ROW && r != MSR_WIDE_ROW) { if (k == 0) { *part = 0; *row = r; return 1; } k--; }
	for (r = 0; r < MSR_RIGHT_ROWS; r++) if (r != MSR_TOP_ROW) { if (k == 0) { *part = 1; *row = r; return 1; } k--; }
	if (k == 0) { *part = 0; *row = MSR_WIDE_ROW; return 1; }
	return 0;
}
static inline int msr_count(int fl)
{
	int k, p, r, n = 0;
	for (k = 0; msr_cand(k, &p, &r); k++) if (!msr_gone(p, r, fl)) n++;
	return n;
}
static inline int msr_slot(int part, int row, int fl)
{
	int k, p, r, n = 0;
	if (msr_gone(part, row, fl)) return -1;
	for (k = 0; msr_cand(k, &p, &r); k++) {
		if (p == part && r == row) return n;
		if (!msr_gone(p, r, fl)) n++;
	}
	return -1;
}
/* the row at screen slot s; 1 = found */
static inline int msr_at(int s, int fl, int *part, int *row)
{
	int k, p, r, n = 0;
	for (k = 0; msr_cand(k, &p, &r); k++) {
		if (msr_gone(p, r, fl)) continue;
		if (n == s) { *part = p; *row = r; return 1; }
		n++;
	}
	return 0;
}
/* the cursor: dir +1 / -1 = the next / previous visible row in screen order with wrap (down from the last row = the first, up from the first = the last); dir 0 = keep the row if it is visible,
   else the first row.  The row it stands on does not have to be visible (a row that went away under it). */
static inline void msr_step(int part, int row, int dir, int fl, int *np, int *nr)
{
	int n = msr_count(fl), s = msr_slot(part, row, fl);
	if (dir == 0) {
		if (s >= 0) { *np = part; *nr = row; } else msr_at(0, fl, np, nr);
		return;
	}
	if (s < 0) s = dir > 0 ? -1 : 0;                                       /* a vanished row: down goes to the first row, up to the last */
	msr_at((s + dir + n) % n, fl, np, nr);
}
static inline int msr_y(int part, int row, int fl)               /* part 2 = description line number; a gone row is drawn below the screen */
{
	int s;
	if (part > 1) return MSR_BASE_Y + ((fl & MSR_FL_DEV) ? 16 * 5 : 0) + 16 * (row - MSR_DESC_FIRST_LINE);
	s = msr_slot(part, row, fl);
	return s < 0 ? 1000 : MSR_BASE_Y + 16 * (s >= MSR_CAP ? s - MSR_CAP : s);
}
static inline int msr_col2(int part, int row, int fl) { return msr_slot(part, row, fl) >= MSR_CAP; }
#endif
