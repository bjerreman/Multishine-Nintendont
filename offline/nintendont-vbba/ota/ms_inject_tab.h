/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The Name Entry inject table (ms_inject.c applies it after the apploader has loaded the stock NTSC-U Melee v1.02 game, before any of it runs).
 * Each hook is one instruction word: its address, the stock word that must be there (the safety check; a game that differs is left alone) and our new word.
 * P1-P3 make the game enter the Vs menu's Name Entry screen by itself after the boot sequence (where the memory card's Multishine loader save takes over),
 * P5 never starts the progressive-scan question, P6 (six words) turns 'No Game Data, Create Game Data?' into a silent create.
 * The cave is our own code (52 words) written over an unreferenced stock function (HSD_TevStage2Index, first stock word MSIJ_CAVE_OLD_FIRST):
 * CAVE_C (word 0) = the GM_MENU first-entry case, CAVE_A (word 13) = the mnMain set-up hook, CAVE_THINK (word 20) = the Name Entry press; on a Wii it leaves
 * the video mode to Nintendont (the bus clock word 0x800000F8 decides).  Word 47 is the cave's one-shot 'done' flag.  No other game bytes are stored here. */
#ifndef MS_INJECT_TAB_H
#define MS_INJECT_TAB_H
#define MSIJ_NHOOKS 10
static const unsigned ms_inject_hooks[MSIJ_NHOOKS][3] = {      /* address, stock word (the check), new word */
	{ 0x801BFA20U, 0x887F0004U, 0x38600001U },   /* P1 boot scene leaves to GM_MENU (stock: lbz r3,4(r31) = mode_id, GM_VS) */
	{ 0x801B1360U, 0x38000000U, 0x481B14D8U },   /* P2 GM_MENU onEnter default case -> CAVE_C */
	{ 0x8022E924U, 0x48161431U, 0x48133F49U },   /* P3 mnMain_Scene_OnEnter SetupProc call -> CAVE_A */
	{ 0x801A4578U, 0x41820028U, 0x48000028U },   /* P5 gm_801A4510: never start the progressive-scan prompt (beq boot -> b boot) */
	{ 0x801AF3A0U, 0x80BE007CU, 0x80BE0080U },   /* P6 card scene: 'Create Game Data?' becomes the silent create (no question, no acknowledgement) */
	{ 0x801AF3A8U, 0x80BE0020U, 0x80BE0024U },   /* P6 card scene: 'Create Game Data?' becomes the silent create (no question, no acknowledgement) */
	{ 0x801AF3ACU, 0x38600001U, 0x38600000U },   /* P6 card scene: 'Create Game Data?' becomes the silent create (no question, no acknowledgement) */
	{ 0x801AF3B0U, 0x3880000AU, 0x3880000BU },   /* P6 card scene: 'Create Game Data?' becomes the silent create (no question, no acknowledgement) */
	{ 0x801AF3B8U, 0x38000005U, 0x38000006U },   /* P6 card scene: 'Create Game Data?' becomes the silent create (no question, no acknowledgement) */
	{ 0x801AFAD4U, 0x41820014U, 0x48000020U },   /* P6 card scene: 'Create Game Data?' becomes the silent create (no question, no acknowledgement) */
};
#define MSIJ_CAVE_ADDR 0x80362838U
#define MSIJ_CAVE_NWORDS 52
#define MSIJ_CAVE_OLD_FIRST 0x7C0802A6U
static const unsigned ms_inject_cave[MSIJ_CAVE_NWORDS] = {
	0x3D808036U, 0x816C28F8U, 0x2C0B0000U, 0x40820018U, 0x38000002U, 0x981F0000U,
	0x38000004U, 0x981F0001U, 0x4BE4EB14U, 0x38000000U, 0x981F0000U, 0x981F0001U,
	0x4BE4EB04U, 0x3D808036U, 0x816C28F8U, 0x2C0B0000U, 0x4082000CU, 0x3C808036U,
	0x60842888U, 0x4802D4D0U, 0x7C0802A6U, 0x9421FFF0U, 0x90010014U, 0x93E1000CU,
	0x7C7F1B78U, 0x3D808000U, 0x816C00F8U, 0x3D400BECU, 0x7C0B5040U, 0x40800014U,
	0x38000001U, 0x3D808047U, 0x980CB0F8U, 0x4BDFCC45U, 0x3D808036U, 0x38000001U,
	0x900C28F8U, 0x3D80804AU, 0x980C0501U, 0x4BED836DU, 0x7FE3FB78U, 0x4802D94DU,
	0x83E1000CU, 0x80010014U, 0x38210010U, 0x7C0803A6U, 0x4E800020U, 0x00000000U,
	0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
};
#endif

