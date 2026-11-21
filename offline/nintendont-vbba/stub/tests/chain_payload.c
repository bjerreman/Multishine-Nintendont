/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* The "shell" of the emulator test (chain_test.sh): a GameCube DOL that proves the stub's trampoline worked.  When it runs it writes, in a loop,
 *   0x80002000  0xC0DEC0DE                        it ran at all
 *   0x80002004  the value of an initialised .data word (0x11223344 if the data section was copied)
 *   0x80002008  a sum of 256 .bss words (0 if the bss was cleared; the memory it landed on held the stub's data)
 *   0x8000200C  the first word of a .rodata table copied from the DOL (0x0BADF00D)
 *   0x80002010  an incrementing counter (it keeps running) */
#include <gccore.h>
volatile u32 probe_data = 0x11223344u;
static volatile u32 probe_bss[256];
static const u32 probe_ro[4] = { 0x0BADF00Du, 1, 2, 3 };
int main(void)
{
	u32 i, sum, n = 0;
	for (;;) {
		sum = 0;
		for (i = 0; i < 256; i++) sum += probe_bss[i];
		*(volatile u32 *)0x80002000 = 0xC0DEC0DEu;
		*(volatile u32 *)0x80002004 = probe_data;
		*(volatile u32 *)0x80002008 = sum;
		*(volatile u32 *)0x8000200C = probe_ro[0];
		*(volatile u32 *)0x80002010 = ++n;
	}
	return 0;
}
