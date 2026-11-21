/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The read speed test of the Diagnostics page (Start): pure arithmetic and wording, host-tested (tests/speed_test.c).
 * The loader reads a few MiB in 128 KiB chunks from each place a game can come from (the stock image on SD, on USB, and the disc in the drive), times it and shows the figures.
 * The test only gathers numbers (sequential KiB/s, random reads/s and latency); it does not judge them.  Units: KiB/s inside (bytes / 1024 / seconds), shown as MB/s with one decimal (1 MB = 1024 KiB, what a file copy dialog shows). */
#ifndef MS_SPEED_H
#define MS_SPEED_H

#define MSSPD_CHUNK     0x20000u        /* bytes per read: 128 KiB */
#define MSSPD_MAX_BYTES (8u << 20)      /* at most 8 MiB per place */
#define MSSPD_MAX_MS    6000u           /* or 6 s, whichever comes first (a card that crawls must not hold the menu) */
#define MSSPD_RND_MS    2500u           /* each random pass (4 KiB reads, then 32 KiB reads) runs this long */
#define MSSPD_RND_MAX   4000u           /* or this many reads */
#define MSSPD_SMALL     0x1000u
#define MSSPD_LARGE     0x8000u

/* KiB/s of `bytes` read in `ms`; 0 when nothing was read, ms 0 counts as 1 ms (a read can finish inside one timer tick) */
static inline unsigned msspd_kibs(unsigned long bytes, unsigned long ms)
{
	if (!bytes) return 0;
	if (!ms) ms = 1;
	return (unsigned)(((unsigned long long)bytes * 1000ull / 1024ull) / ms);   /* 64-bit: 8 MiB * 1000 overflows the Wii's 32-bit long */
}
/* random reads: reads per second and average microseconds per read; 0 when nothing was read */
static inline unsigned msspd_iops(unsigned long n, unsigned long ms) { if (!n) return 0; if (!ms) ms = 1; return (unsigned)((unsigned long long)n * 1000ull / ms); }
static inline unsigned msspd_lat_us(unsigned long n, unsigned long ms) { return n ? (unsigned)((unsigned long long)ms * 1000ull / n) : 0; }
/* the next scattered offset: a 32-bit LCG state s, result aligned down to `align` (a power of two) inside [0, size - len]; size < len gives 0 */
static inline unsigned long msspd_rnd_off(unsigned *s, unsigned long size, unsigned len, unsigned align)
{
	unsigned long span;
	*s = *s * 1664525u + 1013904223u;
	if (size <= len) return 0;
	span = size - len;
	return (unsigned long)(((unsigned long long)(*s >> 8) * (span >> 8 ? (span >> 8) : 1) + (*s & 255u)) % ((unsigned long long)span + 1ull)) & ~(unsigned long)(align - 1);
}
/* "12.3 MB/s" (one decimal, rounded down) or "-" for 0; -> characters written (snprintf style, n > 0) */
static inline int msspd_fmt(char *out, unsigned n, unsigned kibs)
{
	unsigned t;
	char tmp[24];
	int len = 0, i;
	if (!kibs) { tmp[0] = '-'; len = 1; }
	else {
		t = kibs * 10u / 1024u;
		len = 0;
		{ const char *u = " MB/s"; unsigned w = t / 10u, f = t % 10u; char d[12]; int k = 0;
		  do { d[k++] = (char)('0' + w % 10u); w /= 10u; } while (w && k < 10);
		  while (k) tmp[len++] = d[--k];
		  tmp[len++] = '.'; tmp[len++] = (char)('0' + f);
		  for (; *u; u++) tmp[len++] = *u; }
	}
	for (i = 0; i < len && (unsigned)i + 1 < n; i++) out[i] = tmp[i];
	out[i] = 0;
	return len;
}
#endif
