/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Which server address the Wii loader tries, in which order.  Pure, host-tested (tests/srvpick_test.c).  Written for the Wii loader;
 * the GameCube shell has its own copy: the two trees share no code.
 *
 * The rule: never worse than the baked address alone.  The baked address keeps its place and its time share; every new source only comes AFTER it, except an address that already worked in
 * THIS run with the pinned key.  The pinned TLS key is the only thing that makes an address "good": a DNS answer or a plain-HTTP answer never does.
 *   1. proven  an address that completed a pinned-key TLS request in this run (the next requests go there first, so a dead baked address costs one timeout per start, not one per request)
 *   2. baked   MSOTA_IP of this build
 *   3. cache   SD:/multishine/ota.ip: an address that completed a pinned-key TLS request in an EARLIER run and is not the baked one (a moved server is found again without DNS)
 *   4. dns     this run's answer for MSOTA_HOSTNAME, looked up only after the baked address failed (the gateway guess, then 1.1.1.1 and 8.8.8.8)
 * Equal addresses and zeros are dropped (never the same address twice). */
#ifndef MS_SRVPICK_H
#define MS_SRVPICK_H

typedef unsigned int mssp_u32;

#define MSSP_MAX 4
enum { MSSP_C_NONE = 0, MSSP_C_WRITE = 1, MSSP_C_CLEAR = 2 };

/* the resolvers of the fallback lookup, in order, duplicates and zeros dropped: the gateway guess (libogc does not expose the DHCP resolver), 1.1.1.1, 8.8.8.8.  Returns the count. */
static inline int mssp_resolvers(mssp_u32 gw, mssp_u32 pub1, mssp_u32 pub2, mssp_u32 out[3])
{
	const mssp_u32 in[3] = { gw, pub1, pub2 };
	int n = 0, i, k, dup;
	for (i = 0; i < 3; i++) {
		if (!in[i]) continue;
		for (dup = 0, k = 0; k < n; k++) if (out[k] == in[i]) dup = 1;
		if (!dup) out[n++] = in[i];
	}
	return n;
}

/* a usable unicast public IPv4 (a release refuses private, loopback, link-local, CGNAT, multicast and reserved answers: a resolver on the LAN cannot steer the shell into the LAN) */
static inline int mssp_public(mssp_u32 ip)
{
	mssp_u32 a = ip >> 24, b = (ip >> 16) & 255u;
	if (a == 0 || a == 10 || a == 127 || a >= 224) return 0;
	if (a == 169 && b == 254) return 0;
	if (a == 172 && b >= 16 && b <= 31) return 0;
	if (a == 192 && b == 168) return 0;
	if (a == 100 && b >= 64 && b <= 127) return 0;
	if (ip == 0xFFFFFFFFu) return 0;
	return 1;
}
/* lab builds (Dolphin, a server inside the container) accept any non-zero address */
static inline int mssp_answer_ok(mssp_u32 ip, int lab) { return ip && (lab || mssp_public(ip)); }

/* the order (see the top); how[i] names the source for the log.  Returns the count (>= 1 when baked is not zero). */
static inline int mssp_order(mssp_u32 proven, mssp_u32 baked, mssp_u32 cache, mssp_u32 dns, mssp_u32 out[MSSP_MAX], const char *how[MSSP_MAX])
{
	const mssp_u32 in[MSSP_MAX] = { proven, baked, cache, dns };
	static const char *const nm[MSSP_MAX] = { "proven", "baked", "ota.ip", "dns" };
	int n = 0, i, k, dup;
	for (i = 0; i < MSSP_MAX; i++) {
		if (!in[i]) continue;
		for (dup = 0, k = 0; k < n; k++) if (out[k] == in[i]) dup = 1;
		if (dup) continue;
		out[n] = in[i]; how[n] = nm[i]; n++;
	}
	return n;
}

/* after a request that ended at ip: tls_ok = it completed over the pinned-key TLS.  WRITE: remember ip in ota.ip (a non-baked address proved itself); CLEAR: the baked address
   proved itself again and an ota.ip exists (back to 's behaviour); NONE otherwise (a plain-HTTP answer proves nothing). */
static inline int mssp_after(int tls_ok, mssp_u32 ip, mssp_u32 baked, mssp_u32 cache)
{
	if (!tls_ok || !ip) return MSSP_C_NONE;
	if (ip == baked) return cache ? MSSP_C_CLEAR : MSSP_C_NONE;
	return ip == cache ? MSSP_C_NONE : MSSP_C_WRITE;
}

#endif
