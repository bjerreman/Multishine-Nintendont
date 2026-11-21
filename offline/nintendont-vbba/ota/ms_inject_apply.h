/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Name Entry inject: the table the shell leaves in memory and the routine that applies it.  Pure C89 header, no library calls, included by the
 * Nintendont multi-DOL launcher (multidol/apploader.c: runs right after the game's apploader loaded the DOL, before the jump) and by the host test (tests/inject_test.c).
 * Table (u32 words, at MSIJ_AREA):
 *   [0] 'MSIJ'  [1] version 1  [2] n hooks  [3] cave address  [4] cave words  [5] retail first word of the cave  [6] status (written here)  [7] 0
 *   then n * { address, retail word, new word }, then the cave words.
 * Status: 0 not run, 1 applied, 2 a retail word did not match (NOTHING was written: another disc / revision / already patched), 3 bad table (nothing written). */
#ifndef MS_INJECT_APPLY_H
#define MS_INJECT_APPLY_H

typedef unsigned int msij_u32;
#define MSIJ_AREA   0x81309000U         /* below the chain's guard word (0x81309F00) and mailbox (0x81309F40), above the chain's argument block (0x81308000..0x81309000) */
#define MSIJ_MAGIC  0x4D53494AU
#define MSIJ_MAX_HOOKS 16U              /* 10 today: 4 PROG hooks + 6 silent-create words */
#define MSIJ_MAX_CAVE  64U
#define MSIJ_LO 0x80003100U            /* the game's text / data: nothing below, nothing above the apploader buffers */
#define MSIJ_HI 0x81200000U
#ifndef MSIJ_MEM
#define MSIJ_MEM(a) ((volatile msij_u32 *)(a))
#endif
#ifndef MSIJ_FLUSH
#define MSIJ_FLUSH(a, n) ((void)0)
#endif

/* when the inject is on (DEFAULT ON): the disc passed the lock, no no_inject marker, memory card emulation on, the card file saves/GALE.raw present */
enum { MSIJ_ON = 0, MSIJ_OFF_DISC, MSIJ_OFF_MARKER, MSIJ_OFF_EMU, MSIJ_OFF_NOCARD };
__attribute__((unused)) static int msij_decide(int disc_ok, int no_inject_marker, int memcard_emu, int card_file)
{
    if (!disc_ok) return MSIJ_OFF_DISC;
    if (no_inject_marker) return MSIJ_OFF_MARKER;
    if (!memcard_emu) return MSIJ_OFF_EMU;
    if (!card_file) return MSIJ_OFF_NOCARD;
    return MSIJ_ON;
}

static int msij_ok_addr(msij_u32 a) { return !(a & 3U) && a >= MSIJ_LO && a < MSIJ_HI; }

/* validate, check every retail word, then write; returns the status (also stored in t[6]) */
static msij_u32 msij_apply(volatile msij_u32 *t)
{
    msij_u32 n, nw, i, ca;
    volatile msij_u32 *h;
    if (t[0] != MSIJ_MAGIC || t[1] != 1U) return 0U;                       /* no table: not armed */
    n = t[2]; ca = t[3]; nw = t[4];
    if (n == 0U || n > MSIJ_MAX_HOOKS || nw == 0U || nw > MSIJ_MAX_CAVE || !msij_ok_addr(ca) || !msij_ok_addr(ca + 4U * (nw - 1U))) { t[6] = 3U; return 3U; }
    h = t + 8;
    for (i = 0; i < n; i++) if (!msij_ok_addr(h[3 * i]) || h[3 * i] == ca) { t[6] = 3U; return 3U; }
    for (i = 0; i < n; i++) if (*MSIJ_MEM(h[3 * i]) != h[3 * i + 1]) { t[6] = 2U; return 2U; }
    if (*MSIJ_MEM(ca) != t[5]) { t[6] = 2U; return 2U; }
    for (i = 0; i < nw; i++) *MSIJ_MEM(ca + 4U * i) = h[3 * n + i];
    for (i = 0; i < n; i++) *MSIJ_MEM(h[3 * i]) = h[3 * i + 2];
    MSIJ_FLUSH(ca, 4U * nw);
    for (i = 0; i < n; i++) MSIJ_FLUSH(h[3 * i], 4U);
    t[6] = 1U;
    return 1U;
}

/* build the table (the loader side): dst needs 8 + 3 * n + nw words */
__attribute__((unused)) static unsigned msij_pack(volatile msij_u32 *dst, unsigned n, const unsigned (*hooks)[3], msij_u32 cave, unsigned nw, msij_u32 cave_old, const unsigned *words)
{
    unsigned i;
    dst[0] = MSIJ_MAGIC; dst[1] = 1U; dst[2] = n; dst[3] = cave; dst[4] = nw; dst[5] = cave_old; dst[6] = 0U; dst[7] = 0U;
    for (i = 0; i < n; i++) { dst[8 + 3 * i] = hooks[i][0]; dst[9 + 3 * i] = hooks[i][1]; dst[10 + 3 * i] = hooks[i][2]; }
    for (i = 0; i < nw; i++) dst[8 + 3 * n + i] = words[i];
    return 8U + 3U * n + nw;
}
#endif
