/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The stub's trampoline (position independent, no calls, no globals): void tramp(const struct tramp_args *a), copied by stub_main.c to 0x81300000 and run from there, because the
 * sections it copies land on top of the stub itself.
 *   struct tramp_args { u32 entry, n, bss, bss_len; struct { u32 src, dst, len; } sec[18]; }       src = a MEM2 address, dst / len = MEM1 (validated by msdol_check), len a multiple of 4
 * Steps: interrupts off, copy every section word by word, zero the bss, write the data cache back (dcbst) for everything that was written, then invalidate the instruction cache
 * (icbi) over the same ranges, sync / isync, and jump to the entry with r3 = r4 = 0 (the shell's crt0 takes its arguments from the Homebrew Channel block at 0x80001808). */
	.text
	.globl ms_tramp_start
	.globl ms_tramp_end
ms_tramp_start:
	mfmsr	r0
	rlwinm	r0,r0,0,17,15			/* MSR[EE] off: no interrupt while the code under us is replaced */
	mtmsr	r0
	isync
	mr	r31,r3				/* args */
	lwz	r4,4(r31)			/* n */
	addi	r5,r31,16			/* section table */
	cmpwi	r4,0
	beq	.Lbss
.Lsec:
	lwz	r6,0(r5)			/* src */
	lwz	r7,4(r5)			/* dst */
	lwz	r8,8(r5)			/* len */
	srwi.	r9,r8,2
	beq	.Lnext
	mtctr	r9
	addi	r10,r6,-4
	addi	r11,r7,-4
.Lcopy:
	lwzu	r12,4(r10)
	stwu	r12,4(r11)
	bdnz	.Lcopy
.Lnext:
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.Lsec
.Lbss:
	lwz	r6,8(r31)			/* bss */
	lwz	r8,12(r31)			/* bss len */
	srwi.	r9,r8,2
	beq	.Lflush
	mtctr	r9
	li	r12,0
	addi	r11,r6,-4
.Lzero:
	stwu	r12,4(r11)
	bdnz	.Lzero
.Lflush:
	/* data cache write-back of every section and the bss */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.Lflushbss
.Lfs:
	lwz	r7,4(r5)
	lwz	r8,8(r5)
	rlwinm	r6,r7,0,0,26			/* start aligned down to 32 */
	add	r8,r7,r8			/* end */
.Lfl:
	dcbst	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.Lfl
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.Lfs
.Lflushbss:
	lwz	r7,8(r31)
	lwz	r8,12(r31)
	cmpwi	r8,0
	beq	.Lsync
	rlwinm	r6,r7,0,0,26
	add	r8,r7,r8
.Lflb:
	dcbst	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.Lflb
.Lsync:
	sync
	/* instruction cache invalidate over the same ranges */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.Lgo
.Lis:
	lwz	r7,4(r5)
	lwz	r8,8(r5)
	rlwinm	r6,r7,0,0,26
	add	r8,r7,r8
.Lic:
	icbi	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.Lic
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.Lis
.Lgo:
	sync
	isync
	lwz	r5,0(r31)			/* entry */
	mtctr	r5
	li	r3,0
	li	r4,0
	bctr
ms_tramp_end:
	nop
