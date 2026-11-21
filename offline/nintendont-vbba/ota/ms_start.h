/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The decisions of the Multishine start screen and the quick set-up: pure, host-tested (tests/start_test.c).
 * The loader (ms_ota_loader.c) gathers the facts (files on the SD, stock discs found) and draws; everything that decides lives here so the order of the steps, the resume after a
 * failure, the choice of the main key and the texts are tested without a console. */
#ifndef MS_START_H
#define MS_START_H
#include <stdio.h>
#include <string.h>

/* ---- the set-up steps, in the order they run ---------------------------------------------------------------------------------------------------------------------------- */
#define MSST_NET  1u      /* 1 network check */
#define MSST_ISO  2u      /* 2 find the stock Melee v1.02 image */
#define MSST_DL   4u      /* 3 download the card files (crd) and the system-files data (pack) */
#define MSST_CARD 8u      /* 4 create the virtual memory card and build the system files from the player's disc */
#define MSST_ALL  15u

/* what is on the SD right now (each 0/1; crd / pack = a container that is there AND verifies; sysfiles = the card file holds the whole MultishinePack; phys = the set-up installed onto the memory card
   in slot A (marker SD:/multishine/physical_card), which counts only while there is no virtual card) */
struct msst_facts { int card; int crd; int pack; int sysfiles; int phys; };

/* "no Multishine files": the card is missing, or the pack data is missing, or the system files are not built (the card exists but lacks them).  A console set up on the memory card in slot A has
   nothing missing on the SD: the game builds the system files on that card at its first online start. */
static inline int msst_setup_needed(const struct msst_facts *f) { if (!f->card && f->phys) return 0; return !f->card || !f->pack || !f->sysfiles; }

/* the steps still to run for these facts.  The download is needed when the card files or the pack data are not there (the network check only for it); the card step when there is no card or its
   system files are not built (the disc is only needed for that).  A card that is whole whose pack data is missing only needs the download. */
static inline unsigned msst_plan(const struct msst_facts *f)
{
	unsigned p = 0;
	if (!f->crd || !f->pack) p |= MSST_NET | MSST_DL;
	if (!f->card || !f->sysfiles) p |= MSST_ISO | MSST_CARD;
	return p;
}

/* the steps for the memory card in slot A: the card files (download when missing) and the install; no disc image (the game builds the system files on the card) and no pack data */
static inline unsigned msst_plan_phys(const struct msst_facts *f)
{
	unsigned p = MSST_CARD;
	if (f->phys && !f->card) return 0;
	if (!f->crd) p |= MSST_NET | MSST_DL;
	return p;
}

/* the next step to run: the first of `plan` that this run has not finished (`done`); 0 = nothing left.  After a failure the plan is made again from the files and `done` is kept, so the run resumes
   at the missing step (a finished download is not repeated, a failed card step is the next one). */
static inline unsigned msst_next(unsigned plan, unsigned done)
{
	unsigned left = plan & ~done, b;
	for (b = MSST_NET; b <= MSST_CARD; b <<= 1) if (left & b) return b;
	return 0;
}

#define MSST_S_TODO 0     /* not yet */
#define MSST_S_RUN  1     /* running now */
#define MSST_S_DONE 2     /* done, or already in place */
#define MSST_S_FAIL 3     /* failed (the reason is shown below the list) */
static inline int msst_status(unsigned step, unsigned plan, unsigned done, unsigned running, unsigned failed)
{
	if (failed == step) return MSST_S_FAIL;
	if (running == step) return MSST_S_RUN;
	if ((done & step) || !(plan & step)) return MSST_S_DONE;
	return MSST_S_TODO;
}

static inline const char *msst_step_text(unsigned step)
{
	switch (step) {
	case MSST_NET:  return "Check the network";
	case MSST_ISO:  return "Find your Melee disc";
	case MSST_DL:   return "Download the Multishine files";
	case MSST_CARD: return "Create the memory card";
	}
	return "";
}
/* step 4 when the set-up installs onto the card in slot A */
static inline const char *msst_step_text_route(unsigned step, int phys) { return (phys && step == MSST_CARD) ? "Install on the memory card in slot A" : msst_step_text(step); }

/* ---- the start screen -------------------------------------------------------------------------------------------------------------------------------------------------- */
#define MSST_A_NONE   0   /* nothing to start: no stock disc and nothing to set up */
#define MSST_A_LAUNCH 1
#define MSST_A_SETUP  2

/* the main key: a console without Multishine files offers the set-up (it checks for the disc itself, step 2); otherwise Launch when a stock image was found */
static inline int msst_main_action(int setup_needed, int nstock) { return setup_needed ? MSST_A_SETUP : (nstock > 0 ? MSST_A_LAUNCH : MSST_A_NONE); }

/* autoboot (the player's Auto Boot setting): never without the Multishine files; the set-up is shown instead */
static inline int msst_autoboot_allowed(int setup_needed) { return !setup_needed; }

/* the saved GamePath at an autoboot: 0 keep it (the disc lock accepts it), 1 replace it with the first stock image, -1 nothing to start (no stock image: today's flow, the disc lock explains) */
static inline int msst_autoboot_path(int saved_ok, int nstock) { return saved_ok ? 0 : (nstock > 0 ? 1 : -1); }

/* the device the start screen opens on: the saved one when it holds a stock image, else the other one when that does, else the saved one */
static inline int msst_pick_dev(int saved_usb, const int nstock[2])
{
	if (nstock[saved_usb ? 1 : 0] > 0) return saved_usb ? 1 : 0;
	if (nstock[saved_usb ? 0 : 1] > 0) return saved_usb ? 0 : 1;
	return saved_usb ? 1 : 0;
}
/* the SD / USB selector (L / R) shows only when both devices have a games folder */
static inline int msst_show_pill(const int has_games[2]) { return has_games[0] && has_games[1]; }

/* the footer of the start screen: key / action pairs in the order drawn (B is the exit by holding it; a short B does nothing here) */
struct msst_keys { int n; const char *key[6]; const char *act[6]; unsigned disabled; };
static inline void msst_footer(int action, struct msst_keys *k)
{
	int i = 0;
	k->disabled = 0;
	k->key[i] = "A"; k->act[i] = action == MSST_A_SETUP ? "Set up Multishine" : "Launch"; if (action == MSST_A_NONE) k->disabled |= 1u << i; i++;
	k->key[i] = "X"; k->act[i] = "Browse"; i++;
	k->key[i] = "Y"; k->act[i] = "Settings"; i++;
	k->key[i] = "Start"; k->act[i] = "About"; i++;
	k->key[i] = "Hold B"; k->act[i] = "Exit"; i++;
	k->n = i;
}

/* ---- no suitable image: the two ways forward  ------------------------------------------------------------------------------------------------------- */
/* 'no image' = the search found no stock image FILE and no stock disc is in the drive.  The start screen then offers A: Use the disc drive (a Wii that reads GC discs only: hidden on a Wii U / Wii VC,
   where msst_drive_probe_allowed is 0) and X: Browse for a game (the file browser, ms_browse.h).  A disc from the drive cannot build the system files (the builder reads an image FILE), so with a drive
   disc and an incomplete set-up X opens the browser as well: it is the way to point at a copy on the card. */
static inline int msst_noiso(int drive_stock, int nstock) { return !drive_stock && nstock <= 0; }
static inline int msst_x_opens_browser(int drive_stock, int nstock, int setup_needed) { return nstock <= 0 && (!drive_stock || setup_needed); }

#define MSST_KEY_A     1
#define MSST_KEY_X     2
#define MSST_KEY_Y     3
#define MSST_KEY_START 4
#define MSST_D_NONE     0
#define MSST_D_LAUNCH   1   /* A: start the game */
#define MSST_D_SETUP    2   /* A: the set-up */
#define MSST_D_DRIVE    3   /* A with no image: ask for the disc in the drive */
#define MSST_D_LIST     4   /* X: the list of accepted games (SelectGame) */
#define MSST_D_BROWSER  5   /* X: the file browser */
#define MSST_D_SETTINGS 6
#define MSST_D_ABOUT    7
/* what a key does on the start screen (action = msst_main_action; B is the hold-to-exit, handled apart) */
static inline int msst_key_decision(int key, int action, int drive_stock, int nstock, int setup_needed, int drive_ok)
{
	switch (key) {
	case MSST_KEY_A:
		if (msst_noiso(drive_stock, nstock)) return drive_ok ? MSST_D_DRIVE : MSST_D_NONE;
		return action == MSST_A_SETUP ? MSST_D_SETUP : action == MSST_A_LAUNCH ? MSST_D_LAUNCH : MSST_D_NONE;
	case MSST_KEY_X: return msst_x_opens_browser(drive_stock, nstock, setup_needed) ? MSST_D_BROWSER : MSST_D_LIST;
	case MSST_KEY_Y: return MSST_D_SETTINGS;
	case MSST_KEY_START: return MSST_D_ABOUT;
	}
	return MSST_D_NONE;
}
/* the footer for all the start states: with no image A is the drive (omitted where there is no drive), X says 'Browse for a game' whenever it opens the browser */
static inline void msst_footer_ex(int action, int noiso, int xbrowser, int drive_ok, struct msst_keys *k)
{
	int i = 0;
	k->disabled = 0;
	if (noiso) { if (drive_ok) { k->key[i] = "A"; k->act[i] = "Use the disc drive"; i++; } }
	else { k->key[i] = "A"; k->act[i] = action == MSST_A_SETUP ? "Set up Multishine" : "Launch"; if (action == MSST_A_NONE) k->disabled |= 1u << i; i++; }
	k->key[i] = "X"; k->act[i] = (noiso || xbrowser) ? "Browse for a game" : "Browse"; i++;
	k->key[i] = "Y"; k->act[i] = "Settings"; i++;
	k->key[i] = "Start"; k->act[i] = "About"; i++;
	k->key[i] = "Hold B"; k->act[i] = "Exit"; i++;
	k->n = i;
}
/* the second line of the start screen while the set-up is open but there is no image to build the system files from */
static inline const char *msst_noiso_setup_hint(int drive_ok)
{
	return drive_ok ? "First pick your Melee game: use the disc drive or browse for it." : "First pick your Melee game: browse for it.";
}
/* the disc drive flow: the prompt, then the probe, then the result */
static inline const char *msst_drive_prompt(void) { return "Put your Melee disc in the drive"; }
#define MSST_DRV_NONE    0   /* nothing readable in the drive (no disc, no drive, or B) */
#define MSST_DRV_NOTGC   1   /* a disc, not a GameCube disc */
#define MSST_DRV_NOTSTOCK 2  /* a GameCube disc, not stock Melee v1.02 */
static inline const char *msst_drive_miss_text(int why) { return why == MSST_DRV_NOTGC ? "The disc in the drive is not a GameCube disc" : why == MSST_DRV_NOTSTOCK ? "The disc in the drive is not Melee v1.02 (NTSC-U)" : "No disc found in the drive"; }

/* the footer of the set-up screen: running = no key; a failure offers Retry / Back; a finished set-up Launch (A) / Back */
static inline void msst_setup_footer(int state, struct msst_keys *k)   /* state: 0 running, 1 failed, 2 done */
{
	k->n = 0; k->disabled = 0;
	if (state == 1) { k->key[0] = "A"; k->act[0] = "Retry"; k->key[1] = "B"; k->act[1] = "Back"; k->n = 2; }
	else if (state == 2) { k->key[0] = "A"; k->act[0] = "Launch"; k->key[1] = "B"; k->act[1] = "Back"; k->n = 2; }
}

/* ---- texts ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* no stock image on this device: two short centred lines.  seen = how many full Melee images the folder holds (none of them stock) */
static inline void msst_none_text(char *l1, unsigned n1, char *l2, unsigned n2, const char *dev, unsigned seen, unsigned places, int stopped)
{
	snprintf(l1, n1, "No stock Melee v1.02 found in %s:/games", dev);
	if (stopped) snprintf(l2, n2, "Search stopped (%u places). X: Browse to pick a disc.", places);
	else if (seen == 0) snprintf(l2, n2, "No disc image found (%u places searched). Put one in %s:/games.", places, dev);
	else if (seen == 1) snprintf(l2, n2, "Found 1 image, it is not stock (%u places searched).", places);
	else snprintf(l2, n2, "Found %u images, none is stock (%u places searched).", seen, places);
}

/* no stock image: ONE short line that says what was found and the next step (the two lines and 'N places searched' left the player screen; the counts stay in ota.log).
   dev "SD" / "USB"; seen = stock-size images the lock refused; near = the best image-like file of another size or format (path, "" = none) and its size; keys: 1 = the start screen (X browses,
   A the drive where drive_ok), 0 = the set-up screen (its keys are Retry / Back: the line says to go back first) */
static inline void msst_none_line(char *o, unsigned n, const char *dev, unsigned seen, const char *near, unsigned long near_size, int stopped, int drive_ok, int keys)
{
	const char *next = keys ? (drive_ok ? "A: Use the disc drive, X: Browse." : "X: Browse for it.") : "Go back, then X: Browse for it.";
	const char *base = near ? strrchr(near, '/') : NULL;
	char nm[44];
	base = base ? base + 1 : near;
	if (stopped) { snprintf(o, n, "Search stopped. %s", keys ? "X: Browse for your Melee game." : "Go back, then X: Browse for it."); return; }
	if (seen > 0) { snprintf(o, n, "Your Melee image on %s is not stock v1.02. %s", dev, keys ? "X: Browse for another." : "Go back, then X: Browse."); return; }
	if (base && base[0]) {
		size_t l = strlen(base);
		if (l > 40) { memcpy(nm, base, 37); memcpy(nm + 37, "...", 4); } else memcpy(nm, base, l + 1);
		snprintf(o, n, "Found %s, not a stock v1.02 image (%lu MB). %s", nm, (near_size + 524288UL) / 1048576UL, keys ? "X: Browse for another." : "Go back, then X: Browse.");
		return;
	}
	snprintf(o, n, "No Melee game found on %s. %s", dev, next);
}

/* the line under the game name: which memory card the start uses */
static inline void msst_card_line(char *o, unsigned n, int card_emu, int card_exists)
{
	if (!card_emu) snprintf(o, n, "Memory card: the real card in the console");
	else if (card_exists) snprintf(o, n, "Memory card: virtual card on the SD card");
	else snprintf(o, n, "Memory card: none yet");
}

/* ---- status marks at the right edge of a row (ms_ui.c ms_mark) -------------------------------------------------------------------------------------------------------------- */
#define MSST_MARK_NONE 0   /* nothing (a step that has not started) */
#define MSST_MARK_GOOD 1   /* green check */
#define MSST_MARK_BAD  2   /* red X */
#define MSST_MARK_WORK 3   /* three dots: running */
static inline int msst_step_mark(int status)
{
	return status == MSST_S_DONE ? MSST_MARK_GOOD : status == MSST_S_FAIL ? MSST_MARK_BAD : status == MSST_S_RUN ? MSST_MARK_WORK : MSST_MARK_NONE;
}

/* the three rows under the game name on the start screen: Melee disc (a stock image found and accepted by the disc lock), Multishine card (GALE.raw exists), Multishine files (pack data present AND
   the system files built).  A is Launch only when all three are good, which is exactly `!msst_setup_needed && nstock > 0` (msst_main_action). */
struct msst_row { const char *label; int good; };
static inline void msst_rows(int nstock, const struct msst_facts *f, struct msst_row r[3])
{
	r[0].label = "Melee disc v1.02";  r[0].good = nstock > 0;
	r[1].label = "Multishine card";   r[1].good = f->card != 0 || f->phys;
	r[2].label = "Multishine files";  r[2].good = (f->pack && f->sysfiles) || (!f->card && f->phys);
}
static inline int msst_rows_all_good(const struct msst_row r[3]) { return r[0].good && r[1].good && r[2].good; }

/* ---- the disc in the drive, autoboot once, the memory card ------------------------------------------------------------------------------------------------------------------ */
#define MSST_SRC_NONE  0
#define MSST_SRC_DRIVE 1      /* a stock Melee disc in the drive (a Wii that reads GC discs) */
#define MSST_SRC_FILE  2      /* an image file the search locked in */

/* where the game comes from: the drive first (before any file search); then the file the search found */
static inline int msst_source(int drive_stock, int nstock) { return drive_stock ? MSST_SRC_DRIVE : (nstock > 0 ? MSST_SRC_FILE : MSST_SRC_NONE); }
static inline const char *msst_game_name(int source) { return source == MSST_SRC_DRIVE ? "Melee disc in the drive" : "Super Smash Bros. Melee"; }

/* the drive probe runs only on a Wii that can read a GC disc: never on a Wii U / vWii, never on a Wii VC, and not when the player switched it off (SD:/multishine/no_drive_probe) */
static inline int msst_drive_probe_allowed(int wiiu, int wiivc, int switched_off) { return !wiiu && !wiivc && !switched_off; }

/* the set-up builds the system files from an image FILE through FatFs (the builder cannot read the DVD drive): with only a disc in the drive step 2 says so plainly */
static inline const char *msst_setup_iso_drive_text(void) { return "The system files need a copy of the game on the card."; }

/* Auto Boot is turned on ONCE, at a launch of the stock game found as an image FILE (the drive disc may be gone the next time), when the set-up is complete, the marker SD:/multishine/autoboot_set
   is not there yet and the player did not touch the settings in this session.  The player's own Auto Boot choice is never forced again (the marker stays). */
static inline int msst_autoboot_set_now(int setup_needed, int source, int marker_exists, int settings_touched) { return !setup_needed && source == MSST_SRC_FILE && !marker_exists && !settings_touched; }

/* the plain reason of a failed step when the code did not leave a better one */
static inline const char *msst_fail_text(unsigned step)
{
	switch (step) {
	case MSST_NET:  return "No network. Check the cable or the Wi-Fi of the console.";
	case MSST_ISO:  return "No stock Melee v1.02 disc image found.";
	case MSST_DL:   return "The Multishine files could not be downloaded.";
	case MSST_CARD: return "The memory card could not be created.";
	}
	return "";
}
#endif
