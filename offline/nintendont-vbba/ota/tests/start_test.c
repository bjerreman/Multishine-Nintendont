/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_start.h (the start screen and set-up decisions).  Prints OK or the first failure. */
#include <stdio.h>
#include <string.h>
#include "ms_start.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* a whole run on a model SD: the steps in the order the loader would run them; the download puts the containers there, the card step the card and its system files */
static int run(struct msst_facts f, unsigned done, int *steps)
{
	unsigned s;
	int n = 0;
	while ((s = msst_next(msst_plan(&f), done))) {
		steps[n++] = (int)s;
		done |= s;
		if (s == MSST_DL) { f.crd = 1; f.pack = 1; }
		if (s == MSST_CARD) { f.card = 1; f.sysfiles = 1; }
	}
	return n;
}

int main(void)
{
	struct msst_facts none = { 0, 0, 0, 0, 0 }, whole = { 1, 1, 1, 1, 0 };
	struct msst_keys k;
	char a[96], b[96];
	int st[8], n;
	int games2[2] = { 1, 1 }, games1[2] = { 1, 0 }, ns[2];

	/* a console with nothing: all four steps, in the order net, disc, download, card */
	CHECK(msst_setup_needed(&none) && msst_plan(&none) == MSST_ALL);
	n = run(none, 0, st);
	CHECK(n == 4 && st[0] == MSST_NET && st[1] == MSST_ISO && st[2] == MSST_DL && st[3] == MSST_CARD);
	/* everything there: no set-up, nothing to run */
	CHECK(!msst_setup_needed(&whole) && msst_plan(&whole) == 0 && msst_next(0, 0) == 0);
	/* partial set-ups resume at the missing step */
	{ struct msst_facts f = { 0, 1, 1, 0, 0 }; CHECK(msst_plan(&f) == (MSST_ISO | MSST_CARD)); n = run(f, 0, st); CHECK(n == 2 && st[0] == MSST_ISO && st[1] == MSST_CARD); }   /* files downloaded, no card: no network step */
	{ struct msst_facts f = { 1, 1, 1, 0, 0 }; CHECK(msst_setup_needed(&f) && msst_plan(&f) == (MSST_ISO | MSST_CARD)); }                      /* a card without the system files */
	{ struct msst_facts f = { 1, 1, 0, 1, 0 }; CHECK(msst_setup_needed(&f) && msst_plan(&f) == (MSST_NET | MSST_DL)); }                          /* pack data missing only: download, no new card */
	{ struct msst_facts f = { 0, 1, 1, 0, 0 }; unsigned done = MSST_NET | MSST_ISO; CHECK(msst_next(msst_plan(&f), done) == MSST_CARD); }          /* retry after a failed card step */
	/* a failed download is not 'done': the same step comes again; a finished one is not repeated */
	CHECK(msst_next(MSST_ALL, MSST_NET | MSST_ISO) == MSST_DL);
	CHECK(msst_next(MSST_ALL, MSST_NET | MSST_ISO | MSST_DL) == MSST_CARD);
	/* statuses of the list */
	CHECK(msst_status(MSST_NET, MSST_ALL, 0, MSST_NET, 0) == MSST_S_RUN);
	CHECK(msst_status(MSST_NET, MSST_ALL, MSST_NET, 0, 0) == MSST_S_DONE);
	CHECK(msst_status(MSST_DL, MSST_ALL, MSST_NET, 0, MSST_DL) == MSST_S_FAIL);
	CHECK(msst_status(MSST_CARD, MSST_ALL, 0, MSST_NET, 0) == MSST_S_TODO);
	CHECK(msst_status(MSST_NET, MSST_ISO | MSST_CARD, 0, 0, 0) == MSST_S_DONE);        /* not needed = already in place */
	/* the main key */
	CHECK(msst_main_action(1, 1) == MSST_A_SETUP && msst_main_action(1, 0) == MSST_A_SETUP);
	CHECK(msst_main_action(0, 1) == MSST_A_LAUNCH && msst_main_action(0, 3) == MSST_A_LAUNCH && msst_main_action(0, 0) == MSST_A_NONE);
	/* autoboot */
	CHECK(msst_autoboot_allowed(0) && !msst_autoboot_allowed(1));
	CHECK(msst_autoboot_path(1, 1) == 0 && msst_autoboot_path(1, 0) == 0 && msst_autoboot_path(0, 2) == 1 && msst_autoboot_path(0, 0) == -1);
	/* the device */
	ns[0] = 0; ns[1] = 1; CHECK(msst_pick_dev(0, ns) == 1);
	ns[0] = 1; CHECK(msst_pick_dev(1, ns) == 1);
	ns[0] = 1; ns[1] = 0; CHECK(msst_pick_dev(1, ns) == 0);
	ns[0] = 0; ns[1] = 0; CHECK(msst_pick_dev(1, ns) == 1 && msst_pick_dev(0, ns) == 0);
	CHECK(msst_show_pill(games2) && !msst_show_pill(games1));
	/* footers: every screen states its keys */
	msst_footer(MSST_A_LAUNCH, &k);
	CHECK(k.n == 5 && !strcmp(k.key[0], "A") && !strcmp(k.act[0], "Launch") && !strcmp(k.act[1], "Browse") && !strcmp(k.act[2], "Settings") && !strcmp(k.key[3], "Start") && !strcmp(k.act[3], "About") && !strcmp(k.key[4], "Hold B") && k.disabled == 0);
	msst_footer(MSST_A_SETUP, &k); CHECK(!strcmp(k.act[0], "Set up Multishine") && k.disabled == 0);
	msst_footer(MSST_A_NONE, &k); CHECK(k.disabled == 1u);
	msst_setup_footer(0, &k); CHECK(k.n == 0);
	msst_setup_footer(1, &k); CHECK(k.n == 2 && !strcmp(k.act[0], "Retry") && !strcmp(k.key[1], "B") && !strcmp(k.act[1], "Back"));
	msst_setup_footer(2, &k); CHECK(k.n == 2 && !strcmp(k.act[0], "Launch"));
	/* texts */
	msst_none_text(a, sizeof a, b, sizeof b, "sd", 0, 12, 0); CHECK(!strcmp(a, "No stock Melee v1.02 found in sd:/games") && strstr(b, "No disc image found (12 places searched)"));
	msst_none_text(a, sizeof a, b, sizeof b, "usb", 3, 40, 0); CHECK(!strcmp(b, "Found 3 images, none is stock (40 places searched)."));
	msst_none_text(a, sizeof a, b, sizeof b, "sd", 1, 5, 0); CHECK(!strcmp(b, "Found 1 image, it is not stock (5 places searched).")); msst_none_text(a, sizeof a, b, sizeof b, "sd", 0, 7, 1); CHECK(strstr(b, "Search stopped (7 places)") && strstr(b, "X: Browse"));
	msst_card_line(a, sizeof a, 1, 1); CHECK(!strcmp(a, "Memory card: virtual card on the SD card"));
	msst_card_line(a, sizeof a, 0, 1); CHECK(strstr(a, "real card") != NULL);
	CHECK(msst_fail_text(MSST_NET)[0] && msst_fail_text(MSST_ISO)[0] && msst_fail_text(MSST_DL)[0] && msst_fail_text(MSST_CARD)[0]);
	/* the three start rows and their marks follow the facts; A is Launch exactly when all three are good */
	{
		struct msst_row r[3];
		int disc, card, crd, pack, sys;
		for (disc = 0; disc < 3; disc++) for (card = 0; card < 2; card++) for (crd = 0; crd < 2; crd++) for (pack = 0; pack < 2; pack++) for (sys = 0; sys < 2; sys++) {
			struct msst_facts f; f.card = card; f.crd = crd; f.pack = pack; f.sysfiles = sys; f.phys = 0;
			msst_rows(disc, &f, r);
			CHECK(r[0].good == (disc > 0) && r[1].good == card && r[2].good == (pack && sys));
			CHECK((msst_main_action(msst_setup_needed(&f), disc) == MSST_A_LAUNCH) == msst_rows_all_good(r));
			if (!card || !(pack && sys)) CHECK(msst_main_action(msst_setup_needed(&f), disc) == MSST_A_SETUP);
		}
		msst_rows(1, &whole, r);
		CHECK(!strcmp(r[0].label, "Melee disc v1.02") && !strcmp(r[1].label, "Multishine card") && !strcmp(r[2].label, "Multishine files") && msst_rows_all_good(r));
		msst_rows(0, &whole, r); CHECK(!r[0].good && !msst_rows_all_good(r) && msst_main_action(msst_setup_needed(&whole), 0) == MSST_A_NONE);
	}
	CHECK(msst_step_mark(MSST_S_DONE) == MSST_MARK_GOOD && msst_step_mark(MSST_S_FAIL) == MSST_MARK_BAD && msst_step_mark(MSST_S_RUN) == MSST_MARK_WORK && msst_step_mark(MSST_S_TODO) == MSST_MARK_NONE);
	/* the disc in the drive comes before any file; the autoboot-once rule; the probe's guard */
	CHECK(msst_source(1, 0) == MSST_SRC_DRIVE && msst_source(1, 3) == MSST_SRC_DRIVE && msst_source(0, 1) == MSST_SRC_FILE && msst_source(0, 0) == MSST_SRC_NONE);
	CHECK(!strcmp(msst_game_name(MSST_SRC_DRIVE), "Melee disc in the drive") && !strcmp(msst_game_name(MSST_SRC_FILE), "Super Smash Bros. Melee"));
	CHECK(msst_drive_probe_allowed(0, 0, 0) && !msst_drive_probe_allowed(1, 0, 0) && !msst_drive_probe_allowed(0, 1, 0) && !msst_drive_probe_allowed(0, 0, 1));
	CHECK(strstr(msst_setup_iso_drive_text(), "copy of the game on the card") != NULL);
	CHECK(msst_main_action(0, 1) == MSST_A_LAUNCH);                                                  /* files there + the drive disc counted as one: A Launch */
	CHECK(msst_autoboot_set_now(0, MSST_SRC_FILE, 0, 0) == 1);                                       /* complete set-up, a locked-in image file, first time */
	CHECK(msst_autoboot_set_now(1, MSST_SRC_FILE, 0, 0) == 0);                                       /* set-up incomplete: the set-up comes first */
	CHECK(msst_autoboot_set_now(0, MSST_SRC_DRIVE, 0, 0) == 0 && msst_autoboot_set_now(0, MSST_SRC_NONE, 0, 0) == 0);   /* never for the drive disc, never without a disc */
	CHECK(msst_autoboot_set_now(0, MSST_SRC_FILE, 1, 0) == 0);                                       /* the marker is there: never again, whatever the player chose */
	CHECK(msst_autoboot_set_now(0, MSST_SRC_FILE, 0, 1) == 0);                                       /* the player touched the settings this session */
	/* no suitable image: A = the disc drive (Wii only), X = the file browser; with a drive disc and an incomplete set-up X is the browser as well */
	CHECK(msst_noiso(0, 0) && !msst_noiso(1, 0) && !msst_noiso(0, 1) && !msst_noiso(1, 2));
	CHECK(msst_x_opens_browser(0, 0, 0) && msst_x_opens_browser(0, 0, 1));                         /* nothing found: the browser, set up or not */
	CHECK(!msst_x_opens_browser(1, 0, 0) && msst_x_opens_browser(1, 0, 1));                        /* the drive disc: the list, unless the system files still need an image file */
	CHECK(!msst_x_opens_browser(0, 1, 0) && !msst_x_opens_browser(0, 2, 1));                       /* an image exists: the existing Browse (list of accepted games) */
	/* the key table on the no-image screen (Wii, files set up or not) */
	CHECK(msst_key_decision(MSST_KEY_A, MSST_A_NONE, 0, 0, 0, 1) == MSST_D_DRIVE);
	CHECK(msst_key_decision(MSST_KEY_A, MSST_A_SETUP, 0, 0, 1, 1) == MSST_D_DRIVE);                 /* set-up needed but no image: first the game, not the set-up */
	CHECK(msst_key_decision(MSST_KEY_A, MSST_A_NONE, 0, 0, 0, 0) == MSST_D_NONE);                   /* Wii U / Wii VC: no drive, A does nothing */
	CHECK(msst_key_decision(MSST_KEY_A, MSST_A_SETUP, 0, 0, 1, 0) == MSST_D_NONE);
	CHECK(msst_key_decision(MSST_KEY_X, MSST_A_NONE, 0, 0, 0, 1) == MSST_D_BROWSER && msst_key_decision(MSST_KEY_X, MSST_A_NONE, 0, 0, 0, 0) == MSST_D_BROWSER);
	CHECK(msst_key_decision(MSST_KEY_Y, MSST_A_NONE, 0, 0, 0, 1) == MSST_D_SETTINGS && msst_key_decision(MSST_KEY_START, MSST_A_NONE, 0, 0, 0, 1) == MSST_D_ABOUT);
	/* with an image the keys are the old ones */
	CHECK(msst_key_decision(MSST_KEY_A, MSST_A_LAUNCH, 0, 1, 0, 1) == MSST_D_LAUNCH && msst_key_decision(MSST_KEY_A, MSST_A_SETUP, 0, 1, 1, 1) == MSST_D_SETUP);
	CHECK(msst_key_decision(MSST_KEY_X, MSST_A_LAUNCH, 0, 1, 0, 1) == MSST_D_LIST);
	CHECK(msst_key_decision(MSST_KEY_A, MSST_A_LAUNCH, 1, 0, 0, 1) == MSST_D_LAUNCH && msst_key_decision(MSST_KEY_A, MSST_A_SETUP, 1, 0, 1, 1) == MSST_D_SETUP);   /* a drive disc */
	CHECK(msst_key_decision(MSST_KEY_X, MSST_A_SETUP, 1, 0, 1, 1) == MSST_D_BROWSER && msst_key_decision(MSST_KEY_X, MSST_A_LAUNCH, 1, 0, 0, 1) == MSST_D_LIST);
	/* the footer: 5 pairs on a Wii, 4 where there is no drive; the texts of the order */
	msst_footer_ex(MSST_A_NONE, 1, 1, 1, &k);
	CHECK(k.n == 5 && !strcmp(k.key[0], "A") && !strcmp(k.act[0], "Use the disc drive") && !strcmp(k.key[1], "X") && !strcmp(k.act[1], "Browse for a game") && !strcmp(k.act[2], "Settings") && !strcmp(k.key[3], "Start") && !strcmp(k.act[3], "About") && !strcmp(k.key[4], "Hold B") && !strcmp(k.act[4], "Exit") && k.disabled == 0);
	msst_footer_ex(MSST_A_NONE, 1, 1, 0, &k);
	CHECK(k.n == 4 && !strcmp(k.key[0], "X") && !strcmp(k.act[0], "Browse for a game") && !strcmp(k.key[3], "Hold B"));
	msst_footer_ex(MSST_A_LAUNCH, 0, 0, 1, &k);
	CHECK(k.n == 5 && !strcmp(k.act[0], "Launch") && !strcmp(k.act[1], "Browse"));
	msst_footer_ex(MSST_A_SETUP, 0, 1, 1, &k);
	CHECK(!strcmp(k.act[0], "Set up Multishine") && !strcmp(k.act[1], "Browse for a game"));
	msst_footer_ex(MSST_A_NONE, 0, 0, 1, &k);
	CHECK(k.disabled == 1u);
	CHECK(strstr(msst_noiso_setup_hint(1), "disc drive") != NULL && strstr(msst_noiso_setup_hint(0), "disc drive") == NULL && strstr(msst_noiso_setup_hint(0), "browse") != NULL);
	/* the disc drive flow's texts, and the red lines of the no-image screen stay */
	CHECK(!strcmp(msst_drive_prompt(), "Put your Melee disc in the drive"));
	CHECK(!strcmp(msst_drive_miss_text(MSST_DRV_NONE), "No disc found in the drive") && strstr(msst_drive_miss_text(MSST_DRV_NOTGC), "not a GameCube disc") && strstr(msst_drive_miss_text(MSST_DRV_NOTSTOCK), "not Melee v1.02"));
	msst_none_text(a, sizeof a, b, sizeof b, "sd", 2, 31, 0);
	CHECK(!strcmp(a, "No stock Melee v1.02 found in sd:/games") && !strcmp(b, "Found 2 images, none is stock (31 places searched)."));
	/* ONE red line when no stock image is found: what was found and the next step, never the places count */
	{
		char o[160];
		msst_none_line(o, sizeof o, "SD", 0, "", 0, 0, 1, 1); CHECK(!strcmp(o, "No Melee game found on SD. A: Use the disc drive, X: Browse."));
		msst_none_line(o, sizeof o, "SD", 0, NULL, 0, 0, 0, 1); CHECK(!strcmp(o, "No Melee game found on SD. X: Browse for it."));
		msst_none_line(o, sizeof o, "SD", 0, "sd:/games/Super Smash Bros. Melee.iso", 1459978240UL - 32768UL, 0, 1, 1);
		CHECK(!strcmp(o, "Found Super Smash Bros. Melee.iso, not a stock v1.02 image (1392 MB). X: Browse for another."));
		msst_none_line(o, sizeof o, "USB", 2, "", 0, 0, 1, 1); CHECK(!strcmp(o, "Your Melee image on USB is not stock v1.02. X: Browse for another."));
		msst_none_line(o, sizeof o, "SD", 0, "", 0, 1, 1, 1); CHECK(!strcmp(o, "Search stopped. X: Browse for your Melee game."));
		msst_none_line(o, sizeof o, "SD", 0, "", 0, 0, 1, 0); CHECK(!strcmp(o, "No Melee game found on SD. Go back, then X: Browse for it."));     /* the set-up screen has no X */
		msst_none_line(o, sizeof o, "SD", 0, "sd:/a/Super Smash Bros Melee (USA) (v1.02) [rev 2] nkit.iso", 500000000UL, 0, 0, 1);
		CHECK(strstr(o, "...") && strlen(o) < 120 && !strstr(o, "places"));
	}
	/* the set-up onto the memory card in slot A: a marker on the SD, no disc step, no pack data */
	{
		struct msst_facts p0 = { 0, 0, 0, 0, 0 }, p1 = { 0, 1, 0, 0, 0 }, done = { 0, 0, 0, 0, 1 }, both = { 1, 1, 1, 0, 1 };
		CHECK(msst_plan_phys(&p0) == (MSST_NET | MSST_DL | MSST_CARD) && msst_plan_phys(&p1) == MSST_CARD);
		CHECK(msst_plan_phys(&done) == 0 && !msst_setup_needed(&done));
		CHECK(msst_setup_needed(&both) && msst_plan(&both) == (MSST_ISO | MSST_CARD));                /* a virtual card exists: it rules, the marker is ignored */
		{ struct msst_row r[3]; msst_rows(1, &done, r); CHECK(msst_rows_all_good(r)); msst_rows(1, &both, r); CHECK(!r[2].good); }
		CHECK(!strcmp(msst_step_text_route(MSST_CARD, 1), "Install on the memory card in slot A") && !strcmp(msst_step_text_route(MSST_CARD, 0), "Create the memory card") && !strcmp(msst_step_text_route(MSST_DL, 1), msst_step_text(MSST_DL)));
	}
	printf("OK\n");
	return 0;
}
