/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The opt-in diagnostics upload of the shell: pure, host-tested (tests/diag_test.c).
 *   - a RAM ring of the log lines (the log stays available when SD:/multishine/ota.log cannot be written),
 *   - a scrubber (IPv4 addresses, MAC addresses, SD/USB paths and disc / Wi-Fi / account lines never leave the console),
 *   - the payload builder (fixed fields first, then the tail of the SD log and of the RAM log, 16 KB at most, cut at a line start),
 *   - the console id (16 hex = 8 bytes of SHA-256("ms-diag1" + MAC + Wii device id)), the proof header and the short on-screen code.
 * No libc beyond string.h / stdio.h (snprintf); links ms_sha256.c. */
#ifndef MS_DIAG_CORE_H
#define MS_DIAG_CORE_H
#include <string.h>
#include <stdio.h>
#include "ms_sha256.h"

#define MSD_PAYLOAD_MAX 15872u   /* + the "proof" line stays under the https body cap of 16 KB */
#define MSD_RING        8192u
#define MSD_MAGIC       "MSDIAG1"

struct msd_ring { char b[MSD_RING]; unsigned n; };

/* append text (whole lines, '\n' terminated); when full the oldest lines go (to at most half full), at a line start */
static inline void msd_ring_add(struct msd_ring *r, const char *s, unsigned n)
{
	if (n > MSD_RING / 2) { s += n - MSD_RING / 2; n = MSD_RING / 2; }
	if (r->n + n > MSD_RING) {
		unsigned skip = r->n + n - MSD_RING / 2;
		if (skip > r->n) skip = r->n;
		while (skip < r->n && r->b[skip - 1] != '\n') skip++;
		memmove(r->b, r->b + skip, r->n - skip);
		r->n -= skip;
	}
	memcpy(r->b + r->n, s, n);
	r->n += n;
}

static inline int msd_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static inline int msd_isdig(int c) { return c >= '0' && c <= '9'; }
static inline int msd_ishex(int c) { return msd_isdig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int msd_has(const char *s, unsigned n, const char *w)
{
	unsigned i, k, m = (unsigned)strlen(w);
	for (i = 0; i + m <= n; i++) { for (k = 0; k < m && msd_lower(s[i + k]) == w[k]; k++) { } if (k == m) return 1; }
	return 0;
}

/* a line (no '\n') that is withheld whole: it can carry a disc path / game, a Wi-Fi or account detail */
static inline int msd_withheld(const char *l, unsigned n)
{
	static const char *const w[] = { "disc lock", "system files: disc", "virtual card: old pack", "disc image", "ssid", "password", "passwd", "user.json", "token", "wi-fi", "wifi", "wpa", "mac ", "mac:", "email", "account", 0 };
	int i;
	for (i = 0; w[i]; i++) if (msd_has(l, n, w[i])) return 1;
	return 0;
}

/* IN PLACE (the result is never longer): withheld lines become "[w]", dotted IPv4 -> x.x.x.x, MAC (six hex pairs) -> xx:xx:xx:xx:xx:xx, any sd:/ or usb:/ path other than sd:/multishine... -> <path>.  Returns the new length. */
static inline unsigned msd_scrub(char *b, unsigned n)
{
	unsigned i = 0, o = 0;
	while (i < n) {
		unsigned e = i, L;
		while (e < n && b[e] != '\n') e++;
		L = e - i;
		if (msd_withheld(b + i, L)) {
			memcpy(b + o, "[w]", 3); o += 3;
		} else {
			unsigned p = i;
			while (p < e) {
				if (msd_isdig(b[p]) && (p == i || (!msd_isdig(b[p - 1]) && b[p - 1] != '.'))) {          /* a.b.c.d */
					unsigned q = p; int g = 0, ok = 1;
					for (g = 0; g < 4 && ok; g++) {
						unsigned d = 0;
						while (q < e && msd_isdig(b[q]) && d < 3) { q++; d++; }
						if (!d) ok = 0;
						else if (g < 3) { if (q < e && b[q] == '.') q++; else ok = 0; }
					}
					if (ok && !(q < e && msd_isdig(b[q]))) { memcpy(b + o, "x.x.x.x", 7); o += 7; p = q; continue; }
				}
				if (msd_ishex(b[p]) && p + 17 <= e && (p == i || !msd_ishex(b[p - 1]))) {                /* aa:bb:cc:dd:ee:ff */
					int k, ok = 1;
					for (k = 0; k < 6 && ok; k++) {
						if (!msd_ishex(b[p + 3 * k]) || !msd_ishex(b[p + 3 * k + 1])) ok = 0;
						if (k < 5 && b[p + 3 * k + 2] != ':' && b[p + 3 * k + 2] != '-') ok = 0;
					}
					if (ok && !(p + 17 < e && msd_ishex(b[p + 17]))) { memcpy(b + o, "xx:xx:xx:xx:xx:xx", 17); o += 17; p += 17; continue; }
				}
				if ((p == i || b[p - 1] == ' ' || b[p - 1] == '(' || b[p - 1] == '=') && ((p + 4 <= e && !memcmp(b + p, "sd:/", 4)) || (p + 5 <= e && !memcmp(b + p, "usb:/", 5)))) {
					if (!(p + 14 <= e && !memcmp(b + p, "sd:/multishine", 14))) {
						p = e;                                                  /* a file name can hold spaces: the rest of the line goes */
						memcpy(b + o, "<path>", 6); o += 6;
						continue;
					}
				}
				b[o++] = b[p++];
			}
		}
		if (e < n) b[o++] = '\n';
		i = e + 1;
	}
	return o;
}

/* libogc's SI / PAD state machine, read straight from its statics (no SI traffic, no SI_GetType):.  sym = 0 when the build had no symbol table. */
struct msd_sm {
	int sym;                                   /* the statics could be read (the symbol header was generated) */
	int sic_chan;                              /* sicntrl.chan: -1 = no transfer owned, 0..3 = the channel a transfer is waiting on */
	unsigned sic_poll;                         /* sicntrl.poll */
	unsigned si_type[4];                       /* si_type[]: the controller id per port (0x80 = busy: a type transfer is in flight) */
	int pkt_chan[4];                           /* sipacket[n].chan: -1 = nothing queued, else the channel that still has to go */
	unsigned fix0;                             /* cmdfixdevice[0]: 0x4E.. = a wireless fix-ID command was issued for port 0 */
	unsigned rchan, rbits, pend, en, wait;     /* __pad_resettingchan / resettingbits / pendingbits / enabledbits / waitingbits */
	unsigned pi_intsr, pi_intmr;               /* PI_INTSR / PI_INTMR (SI is bit 3) */
};

static inline void msd_sm_line(char *o, unsigned cap, const struct msd_sm *m)
{
	if (!m->sym) { snprintf(o, cap, "pad: no symbols | PI_INTSR %08X INTMR %08X", m->pi_intsr, m->pi_intmr); return; }
	snprintf(o, cap, "pad: sic.chan %d poll %X | type %08X %08X %08X %08X | pkt %d %d %d %d | fix0 %08X | rchan %u rbits %08X pend %08X en %08X wait %08X | PI_INTSR %08X INTMR %08X",
	         m->sic_chan, m->sic_poll, m->si_type[0], m->si_type[1], m->si_type[2], m->si_type[3], m->pkt_chan[0], m->pkt_chan[1], m->pkt_chan[2], m->pkt_chan[3], m->fix0,
	         m->rchan, m->rbits, m->pend, m->en, m->wait, m->pi_intsr, m->pi_intmr);
}

/* what the payload says about this console (all numbers; strings are short fixed words) */
struct msd_info {
	unsigned shell_seq, kernel_floor, kernel_active, stub_floor, stub_active, abi_kernel, abi_shell, abi_stub;
	const char *dev;            /* "SD" or "USB": the device the loader menu is set to */
	int sd_mounted;             /* the shell's own SD mount worked */
	unsigned log_fail;          /* writes of ota.log that failed in this run */
	unsigned long uptime_ms;
	const char *reason;         /* short code, [A-Za-z0-9_.:-] only, cut at 24 */
	int pad_valid;
	struct { unsigned si; int err; unsigned btn; } pad[4];
	unsigned si_poll, si_comcsr, si_sisr, si_exilk;
	struct msd_sm sm;           /* libogc's pad / SI state machine (statics read through ms_padsyms.h) and PI_INTSR / PI_INTMR */
	const char *last_key;       /* "GC pad", "Wiimote", "none yet" */
	const char *console_id;     /* 16 hex or "unknown" */
};

static inline unsigned msd_put(char *o, unsigned cap, unsigned at, const char *s) { unsigned n = (unsigned)strlen(s); if (at + n >= cap) n = cap - at - 1; memcpy(o + at, s, n); return at + n; }

/* line start for taking the last `take` bytes of s[0..n) */
static inline unsigned msd_tail_from(const char *s, unsigned n, unsigned take)
{
	unsigned st;
	if (take >= n) return 0;
	st = n - take;
	while (st < n && s[st - 1] != '\n') st++;
	return st;
}

/* the pad line the Controllers page and the payload share */
static inline void msd_pad_line(char *o, unsigned cap, const struct msd_info *in, int port)
{
	snprintf(o, cap, "port %d: si %08X err %d btn %04X", port, in->pad[port].si, in->pad[port].err, in->pad[port].btn);
}

/* the payload; -> its length (never more than cap - 1, never more than MSD_PAYLOAD_MAX).  sd / ram: the raw log texts (scrubbed here, on the copy in o). */
static inline unsigned msd_build(char *o, unsigned cap, const struct msd_info *in, const char *sd, unsigned sdn, const char *ram, unsigned ramn)
{
	char t[256], rs[28];
	unsigned at = 0, avail, rt, st, lim = cap < MSD_PAYLOAD_MAX ? cap : MSD_PAYLOAD_MAX, k;
	for (k = 0; in->reason && in->reason[k] && k < 24; k++) {
		char c = in->reason[k];
		rs[k] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || msd_isdig(c) || c == '_' || c == '.' || c == ':' || c == '-') ? c : '_';
	}
	rs[k] = 0;
	if (!lim) return 0;
	at = msd_put(o, lim, 0, MSD_MAGIC "\n");
	snprintf(t, sizeof t, "console %s\n", in->console_id ? in->console_id : "unknown"); at = msd_put(o, lim, at, t);
	snprintf(t, sizeof t, "shell %u abi stub %u shell %u kernel %u\n", in->shell_seq, in->abi_stub, in->abi_shell, in->abi_kernel); at = msd_put(o, lim, at, t);
	snprintf(t, sizeof t, "kernel floor %u active %u | stub shell floor %u active %u\n", in->kernel_floor, in->kernel_active, in->stub_floor, in->stub_active); at = msd_put(o, lim, at, t);
	snprintf(t, sizeof t, "device %s sd_mounted %d log_write_fail %u uptime_ms %lu\n", in->dev ? in->dev : "?", in->sd_mounted, in->log_fail, in->uptime_ms); at = msd_put(o, lim, at, t);
	snprintf(t, sizeof t, "reason %s\n", rs[0] ? rs : "none"); at = msd_put(o, lim, at, t);
	if (in->pad_valid) {
		int p;
		for (p = 0; p < 4; p++) { msd_pad_line(t, sizeof t, in, p); at = msd_put(o, lim, at, t); at = msd_put(o, lim, at, "\n"); }
		snprintf(t, sizeof t, "si SIPOLL %08X SICOMCSR %08X SISR %08X SIEXILK %08X\n", in->si_poll, in->si_comcsr, in->si_sisr, in->si_exilk); at = msd_put(o, lim, at, t);
		msd_sm_line(t, sizeof t, &in->sm); at = msd_put(o, lim, at, t); at = msd_put(o, lim, at, "\n");
	}
	snprintf(t, sizeof t, "last menu key from %s\n", in->last_key ? in->last_key : "unknown"); at = msd_put(o, lim, at, t);
	if (at + 160 >= lim) return at;
	avail = lim - at - 100;                                      /* room for the two markers */
	{
		unsigned sd_cap = sdn < avail / 3 ? sdn : avail / 3;
		rt = ramn < avail - sd_cap ? ramn : avail - sd_cap;
		sd_cap = sdn < avail - rt ? sdn : avail - rt;
		st = msd_tail_from(sd, sdn, sd_cap);
		sdn -= st; sd += st;
		{ unsigned rs0 = msd_tail_from(ram, ramn, rt); ram += rs0; ramn -= rs0; }
	}
	{
		unsigned a0;
		at = msd_put(o, lim, at, "---- ota.log tail (SD)\n");
		a0 = at;
		if (sdn && at + sdn < lim) { memcpy(o + at, sd, sdn); at += msd_scrub(o + a0, sdn); }
		at = msd_put(o, lim, at, "---- ram log (this run)\n");
		a0 = at;
		if (ramn && at + ramn < lim) { memcpy(o + at, ram, ramn); at += msd_scrub(o + a0, ramn); }
	}
	o[at] = 0;
	return at;
}

/* ---- console id, proof, code ---- */
static inline void msd_hex(char *o, const unsigned char *b, unsigned n) { static const char H[] = "0123456789abcdef"; unsigned i; for (i = 0; i < n; i++) { o[2 * i] = H[b[i] >> 4]; o[2 * i + 1] = H[b[i] & 15]; } o[2 * n] = 0; }

/* console id: first 8 bytes of SHA-256("ms-diag1" + MAC (6 bytes) + Wii device id (4 bytes, big endian)); out[17] */
static inline void msd_console_id(char out[17], const unsigned char mac[6], unsigned long devid)
{
	ms_sha256_ctx c; unsigned char d[32], id[4];
	id[0] = (unsigned char)(devid >> 24); id[1] = (unsigned char)(devid >> 16); id[2] = (unsigned char)(devid >> 8); id[3] = (unsigned char)devid;
	ms_sha256_init(&c);
	ms_sha256_update(&c, (const unsigned char *)"ms-diag1", 8);
	ms_sha256_update(&c, mac, 6);
	ms_sha256_update(&c, id, 4);
	ms_sha256_final(&c, d);
	msd_hex(out, d, 8);
}

/* the shared proof (a filter against random scanners, NOT authentication: it is computable by anyone who reads this code): first 8 bytes of SHA-256("multishine-diag-v1\n" + console id + "\n" + body); out[17] */
static inline void msd_proof(char out[17], const char *console_id, const char *body, unsigned n)
{
	ms_sha256_ctx c; unsigned char d[32];
	ms_sha256_init(&c);
	ms_sha256_update(&c, (const unsigned char *)"multishine-diag-v1\n", 19);
	ms_sha256_update(&c, (const unsigned char *)console_id, (unsigned)strlen(console_id));
	ms_sha256_update(&c, (const unsigned char *)"\n", 1);
	ms_sha256_update(&c, (const unsigned char *)body, n);
	ms_sha256_final(&c, d);
	msd_hex(out, d, 8);
}

/* the short Code for a photo: shell, SD state, log state, the four pad types and 4 hex of the payload hash.  e.g. "MS60 sd1 log0 pads 09/00/00/00 7C2A" */
static inline void msd_code(char *o, unsigned cap, const struct msd_info *in, const char *payload, unsigned n)
{
	ms_sha256_ctx c; unsigned char d[32];
	ms_sha256_init(&c); ms_sha256_update(&c, (const unsigned char *)payload, n); ms_sha256_final(&c, d);
	snprintf(o, cap, "MS%u sd%d log%u pads %02X/%02X/%02X/%02X %02X%02X", in->shell_seq, in->sd_mounted ? 1 : 0, in->log_fail > 9 ? 9u : in->log_fail,
	         (in->pad[0].si >> 24) & 255, (in->pad[1].si >> 24) & 255, (in->pad[2].si >> 24) & 255, (in->pad[3].si >> 24) & 255, d[0], d[1]);
}
#endif
