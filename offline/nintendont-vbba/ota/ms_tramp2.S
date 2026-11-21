/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The SHELL's trampoline for the in-place hand-over (seq 25).  tramp.S (the stub's, proven on every launch) copies the new DOL over the
 * stub; this one does the same over the running SHELL, which is a different situation (the old code, data and cache lines are those of a program that was running a second ago), so it adds:
 *   - interrupts off AND every PI interrupt source masked (nothing may fire into half replaced code or into the new libogc before it installs its vectors)
 *   - the old shell's data cache lines for the target ranges and the bss are written back and DROPPED (dcbf) BEFORE the copy
 *   - the copy, the bss clear, then dcbf (write back + drop) over everything written: the next read comes from memory
 *   - VERIFY: every section is compared word by word with its source through the UNCACHED alias (0xC0000000 + physical), the bss is checked for zero; a mismatch repeats the whole copy (3 tries);
 *     after the third the tramp paints the screen red (the XFB is passed in) and stops: the old shell is gone by then, there is nothing to return to
 *   - a report in memory (mailbox, read by the new shell at its start and written to ota.log) and in r14..r17 (what the exception screen shows if the new shell dies at once)
 *   - the instruction cache: icbi over the copied ranges, then a flash invalidate of the whole L1 I-cache (HID0 ICFI), sync, isync, then the jump
 * Stage squares (seq 34): 16x16 pixel squares painted into the XFB (args xfb, uncached; stride in words) so a black screen can be told apart: yellow = the tramp was entered, green = the copy
 *   verified, cyan = about to jump; the new shell's main paints a white one as its very first action.  They stay until the new shell's VIDEO_Init.
 * struct tramp2_args { u32 entry, n, bss, bss_len; struct { u32 src, dst, len; } sec[18]; u32 probe0, probe1, mbox, xfb, xfb_words, xfb_stride; }  (position independent, no calls, no globals)
 * mailbox: [0] 'TRMP'  [1] status (1 ok, 2 failed)  [2] copy attempts used  [3] word at entry  [4] probe0 address  [5] its word  [6] probe1 address  [7] its word  (words as read through the uncached alias) */
.macro PAINT idx, hi, lo
	lwz	r4,16+18*12+12(r31)		/* xfb */
	cmpwi	r4,0
	beq	9f
	lwz	r5,16+18*12+20(r31)		/* stride in words */
	cmpwi	r5,0
	beq	9f
	lwz	r6,8(r31)			/* never paint into the new shell: the XFB must lie above the end of its bss (the new shell's heap and stack start there, its own start-up owns that memory) */
	lwz	r7,12(r31)
	add	r6,r6,r7
	lis	r8,0x4000
	subf	r9,r8,r4			/* the cached address of the XFB */
	cmplw	r9,r6
	blt	9f
	slwi	r6,r5,6				/* 16 lines down */
	add	r4,r4,r6
	addi	r4,r4,(4+\idx*10)*4
	li	r7,16
	lis	r8,\hi
	ori	r8,r8,\lo
1:
	li	r9,8
	mtctr	r9
	addi	r10,r4,-4
2:
	stwu	r8,4(r10)
	bdnz	2b
	slwi	r11,r5,2
	add	r4,r4,r11
	addic.	r7,r7,-1
	bne	1b
9:
.endm
	.text
	.globl ms_tramp2_start
	.globl ms_tramp2_end
ms_tramp2_start:
	mfmsr	r0
	rlwinm	r0,r0,0,17,15			/* MSR[EE] off */
	mtmsr	r0
	isync
	mr	r31,r3				/* args */
	lis	r4,0xCC00			/* PI_INTMR = 0: no interrupt source reaches the CPU any more */
	li	r5,0
	stw	r5,0x3004(r4)
	sync
	PAINT	0,0xE200,0xE294			/* yellow: the tramp was entered */
	li	r30,3				/* tries */
.Lretry:
	/* A: drop the old shell's lines for the target ranges and the bss */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.LAbss
.LAsec:
	lwz	r7,4(r5)
	lwz	r8,8(r5)
	rlwinm	r6,r7,0,0,26
	add	r8,r7,r8
.LAl:
	dcbf	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.LAl
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.LAsec
.LAbss:
	lwz	r7,8(r31)
	lwz	r8,12(r31)
	cmpwi	r8,0
	beq	.LAdone
	rlwinm	r6,r7,0,0,26
	add	r8,r7,r8
.LAb:
	dcbf	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.LAb
.LAdone:
	sync
	/* B: copy */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.LCbss
.LBsec:
	lwz	r6,0(r5)
	lwz	r7,4(r5)
	lwz	r8,8(r5)
	srwi.	r9,r8,2
	beq	.LBnext
	mtctr	r9
	addi	r10,r6,-4
	addi	r11,r7,-4
.LBcopy:
	lwzu	r12,4(r10)
	stwu	r12,4(r11)
	bdnz	.LBcopy
.LBnext:
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.LBsec
.LCbss:
	lwz	r6,8(r31)
	lwz	r8,12(r31)
	srwi.	r9,r8,2
	beq	.LDflush
	mtctr	r9
	li	r12,0
	addi	r11,r6,-4
.LCzero:
	stwu	r12,4(r11)
	bdnz	.LCzero
.LDflush:
	/* D: write back and drop everything that was written */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.LDbss
.LDsec:
	lwz	r7,4(r5)
	lwz	r8,8(r5)
	rlwinm	r6,r7,0,0,26
	add	r8,r7,r8
.LDl:
	dcbf	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.LDl
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.LDsec
.LDbss:
	lwz	r7,8(r31)
	lwz	r8,12(r31)
	cmpwi	r8,0
	beq	.LEgo
	rlwinm	r6,r7,0,0,26
	add	r8,r7,r8
.LDb:
	dcbf	0,r6
	addi	r6,r6,32
	cmplw	r6,r8
	blt	.LDb
.LEgo:
	sync
	/* E: verify through the uncached alias (0xC0000000 + physical = virtual + 0x40000000) */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.LEbss
.LEsec:
	lwz	r6,0(r5)
	lwz	r7,4(r5)
	lwz	r8,8(r5)
	srwi.	r9,r8,2
	beq	.LEnext
	mtctr	r9
	addi	r10,r6,-4
	lis	r0,0x4000
	add	r11,r7,r0
	addi	r11,r11,-4
.LEcmp:
	lwzu	r12,4(r10)
	lwzu	r9,4(r11)
	cmplw	r12,r9
	bne	.Lbad
	bdnz	.LEcmp
.LEnext:
	addi	r5,r5,12
	addic.	r4,r4,-1
	bne	.LEsec
.LEbss:
	lwz	r7,8(r31)
	lwz	r8,12(r31)
	srwi.	r9,r8,2
	beq	.Lok
	mtctr	r9
	lis	r0,0x4000
	add	r11,r7,r0
	addi	r11,r11,-4
.LEz:
	lwzu	r9,4(r11)
	cmpwi	r9,0
	bne	.Lbad
	bdnz	.LEz
	b	.Lok
.Lbad:
	addic.	r30,r30,-1
	bne	.Lretry
	/* the third copy still differs: status 2 in the mailbox, the screen red, stop */
	lwz	r4,16+18*12+8(r31)		/* mbox */
	lis	r5,0x5452			/* 'TR' */
	ori	r5,r5,0x4D50			/* 'MP' */
	stw	r5,0(r4)
	li	r5,2
	stw	r5,4(r4)
	dcbf	0,r4
	lwz	r6,16+18*12+12(r31)		/* xfb */
	lwz	r7,16+18*12+16(r31)		/* xfb words */
	cmpwi	r6,0
	beq	.Lhalt
	cmpwi	r7,0
	beq	.Lhalt
	mtctr	r7
	lis	r8,0x4C54			/* YUYV red */
	ori	r8,r8,0x4CFF
	addi	r6,r6,-4
.Lred:
	stwu	r8,4(r6)
	bdnz	.Lred
.Lhalt:
	sync
	b	.Lhalt
/* the copy verified */
.Lok:
	PAINT	1,0x962C,0x9615			/* green: the copy verified */
	/* report: mailbox in memory, r14..r17 */
	lwz	r4,16+18*12+8(r31)		/* mbox */
	lis	r5,0x5452
	ori	r5,r5,0x4D50
	stw	r5,0(r4)
	li	r5,1
	stw	r5,4(r4)
	li	r5,4
	subf	r5,r30,r5			/* attempts used = 4 - tries left (3 left = 1 attempt) */
	stw	r5,8(r4)
	mr	r17,r5
	lwz	r6,0(r31)			/* entry */
	lis	r0,0x4000
	add	r7,r6,r0
	lwz	r14,0(r7)
	stw	r14,12(r4)
	lwz	r8,16+18*12(r31)		/* probe0 */
	stw	r8,16(r4)
	add	r7,r8,r0
	lwz	r15,0(r7)
	stw	r15,20(r4)
	lwz	r8,16+18*12+4(r31)		/* probe1 */
	stw	r8,24(r4)
	add	r7,r8,r0
	lwz	r16,0(r7)
	stw	r16,28(r4)
	lwz	r5,16+18*12+12(r31)		/* xfb and its stride for the new shell's own stage square */
	stw	r5,32(r4)
	lwz	r5,16+18*12+20(r31)
	stw	r5,36(r4)
	lwz	r5,16+18*12+16(r31)		/* xfb words: the new shell clears the whole XFB */
	stw	r5,40(r4)
	dcbf	0,r4
	addi	r6,r4,32
	dcbf	0,r6
	sync
	/* instruction cache: icbi over the copied ranges, then the whole L1 I-cache flash invalidated */
	lwz	r4,4(r31)
	addi	r5,r31,16
	cmpwi	r4,0
	beq	.Lflash
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
.Lflash:
	sync
	mfspr	r0,1008				/* HID0 */
	ori	r0,r0,0x0800			/* ICFI */
	mtspr	1008,r0
	sync
	isync
	PAINT	2,0xB2AA,0xB210			/* cyan: about to jump */
	lwz	r5,0(r31)			/* entry */
	mtctr	r5
	li	r3,0
	li	r4,0
	bctr
ms_tramp2_end:
	nop
