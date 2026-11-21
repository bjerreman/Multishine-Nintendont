/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* prints the teardown plan of ms_chain.c: "chain_steps_test full|default" -> one step name per line, in the order ms_chain_run crumbs and calls them */
#include <stdio.h>
#include <string.h>
#include "ms_chain_steps.h"
int main(int argc, char **argv)
{
	int i, full = argc > 1 && !strcmp(argv[1], "full");
	for (i = 0; i < MSCH_NSTEPS; i++) if (full || !msch_steps[i].full_only) printf("%s\n", msch_steps[i].name);
	return 0;
}
