/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The values of the settings rows the Multishine shell hides, forced at every launch and whenever the settings screen opens, so the kernel behaves exactly as with the shipped config
 * (offline/nintendont-vbba/gen_nincfg.py): cheats off, no cheat path, MaxPads = NIN_CFG_MAXPAD (4), language auto, Patch PAL50 off, TRI arcade off, Wiimote CC rumble off, Skip IPL off,
  * BBA emulation ON, Unlock Read Speed ON, memory card emulation ON while the virtual card file exists, Log ON (the kernel's ndebug.log, shutdump.log and ramlog.log exist only with it: a tester must never lose them).  Everything else in the config is left as the player has it.
 * NATIVE CONTROL (NIN_CFG_NATIVE_SI) is hidden too and is forced ON on every Wii, the one escape being the opt-out marker SD:/multishine/no_native_si (forced OFF then, for a player on Bluetooth / USB pads), so a stale
 * value of an old config can never leave it either way.  A Wii U never gets it (the kernel ignores it there; the setting is meaningless).  With it on the kernel installs no SI / pad patches and the game reads the real GameCube
 * ports itself: Bluetooth / USB pads stop working in the game (kernel/Patch.c DisableSIPatch).  The includer provides NIN_CFG and the NIN_* constants (CommonConfig.h). */
#ifndef MS_CFG_FORCE_H
#define MS_CFG_FORCE_H
#define MS_NATIVE_OFF  0      /* the opt-out marker is there (Wii) */
#define MS_NATIVE_ON   1      /* a Wii without the opt-out marker */
#define MS_NATIVE_WIIU 2      /* a Wii U: never on */
static inline int ms_native_mode(int optout, int wiiu) { return wiiu ? MS_NATIVE_WIIU : (optout ? MS_NATIVE_OFF : MS_NATIVE_ON); }
static inline const char *ms_native_text(int mode) { return mode == MS_NATIVE_ON ? "Native control: on" : mode == MS_NATIVE_WIIU ? "Native control: not on Wii U" : "Native control: off"; }

static inline void ms_cfg_force(NIN_CFG *c, int native_optout, int wiiu, int vcard)
{
	c->Config &= ~(NIN_CFG_CHEATS | NIN_CFG_CHEAT_PATH | NIN_CFG_ARCADE_MODE | NIN_CFG_CC_RUMBLE | NIN_CFG_SKIP_IPL | NIN_CFG_NATIVE_SI);
	c->Config |= NIN_CFG_BBA_EMU | NIN_CFG_LOG | NIN_CFG_REMLIMIT;      /* REMLIMIT = Unlock Read Speed: on for both consoles (the tested configuration; off would emulate the original drive: 50 ms seeks, 3 MB/s) */
	if (vcard) c->Config |= NIN_CFG_MEMCARDEMU;                           /* the virtual card file exists: memory card emulation is on (the row is hidden then) */
	if (vcard) c->MemCardBlocks = 2;                                      /* 251 blocks: the size of the 2 MiB card file the shell makes.  The loader's first-run default was 4 (1019 blocks) = the kernel expected an 8 MiB card and read past the end of the 2 MiB file (every hang / 'damaged card' run of had 4, every healthy one 2) */
	if (ms_native_mode(native_optout, wiiu) == MS_NATIVE_ON) c->Config |= NIN_CFG_NATIVE_SI;
	c->VideoMode &= ~NIN_VID_PATCH_PAL50;
	c->Language = (unsigned int)NIN_LAN_AUTO;
	c->MaxPads = NIN_CFG_MAXPAD;
	memset(c->CheatPath, 0, sizeof c->CheatPath);
}
#endif
