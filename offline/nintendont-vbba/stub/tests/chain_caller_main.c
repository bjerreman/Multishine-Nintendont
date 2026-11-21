/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* The caller of the in-place hand-over for the emulator test (chain_caller_test.sh): a GameCube DOL that plays the early shell.  It copies the embedded payload DOL (chain_payload) into a buffer
 * above the sections, builds the argument vector the Homebrew Channel would have given and runs ms_chain_run (ms_chain.c, the loader's own file, built with MS_CHAIN_TEST): the trampoline must copy the
 * payload's sections over this very program, clear its bss, set the loop-guard word and start it.  The payload (chain_payload.c) then writes its markers; the harness also watches the guard word. */
#include <gccore.h>
#include <string.h>
extern const unsigned char ms_embedded_shell[];
extern const unsigned int ms_embedded_shell_len;
struct ms_chain_hooks;
extern int ms_chain_run(const u8 *dol, u32 len, int argc, char **argv, const struct ms_chain_hooks *hk);
int main(void)
{
	static char a0[] = "sd:/apps/Nintendont-Multishine/boot.dol";
	char *av[1];
	u8 *buf = (u8 *)0x80D00000u;
	av[0] = a0;
	memcpy(buf, ms_embedded_shell, ms_embedded_shell_len);
	DCFlushRange(buf, ms_embedded_shell_len);
	ms_chain_run(buf, ms_embedded_shell_len, 1, av, 0);
	for (;;) *(volatile u32 *)0x80002014 = 0xBAD0BAD0u;              /* it came back: the hand-over refused (the test fails) */
	return 1;
}
