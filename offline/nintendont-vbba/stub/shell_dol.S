/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-2.0-only */
/* the embedded fallback shell: the Nintendont loader DOL (build_wii.sh's shell.dol), path given by -DSHELL_DOL_PATH */
	.section .rodata
	.balign 32
	.globl ms_embedded_shell
ms_embedded_shell:
	.incbin SHELL_DOL_PATH
	.globl ms_embedded_shell_end
ms_embedded_shell_end:
	.balign 4
	.globl ms_embedded_shell_len
ms_embedded_shell_len:
	.long ms_embedded_shell_end - ms_embedded_shell
