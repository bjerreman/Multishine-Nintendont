/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* host test of ota/ms_srvpick.h (Wii ): the address order, the resolver pair, the answer filter, the cache decision and the cache line.
   The last part plays whole starts against a fake network (which address answers with the pinned key, which only with plain HTTP, which not at all) with the same order as ms_ota_loader.c ota_get (TLS over the order, then plain), and checks that the baked address is always tried first unless an address already proved itself in this start. */
#include <stdio.h>
#include <string.h>
#include "ms_srvpick.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define IP(a, b, c, d) (((mssp_u32)(a) << 24) | ((mssp_u32)(b) << 16) | ((mssp_u32)(c) << 8) | (mssp_u32)(d))

static const mssp_u32 BAKED = IP(136, 148, 214, 38), NEW = IP(203, 0, 113, 7), LIE = IP(198, 51, 100, 66);

/* a fake world: who completes a pinned-key TLS request, who answers plain HTTP (any server), who is dead */
struct world { mssp_u32 tls_ok[3], plain_ok[3]; };
static int in(const mssp_u32 *l, mssp_u32 ip) { int i; for (i = 0; i < 3; i++) if (l[i] && l[i] == ip) return 1; return 0; }
struct run { mssp_u32 proven, cache; int tried; mssp_u32 order[16]; int tls_used; mssp_u32 at; };
/* one request like gcnet.c do_req_locked: TLS pass over the order, then plain; returns 1 = served */
static int request(const struct world *w, struct run *r, mssp_u32 dns)
{
	mssp_u32 a[MSSP_MAX]; const char *how[MSSP_MAX];
	int n = mssp_order(r->proven, BAKED, r->cache, dns, a, how), pass, k;
	for (pass = 0; pass < 2; pass++)
		for (k = 0; k < n; k++) {
			if (r->tried < 16) r->order[r->tried] = a[k];
			r->tried++;
			if (pass == 0 && in(w->tls_ok, a[k])) {
				r->proven = a[k];
				switch (mssp_after(1, a[k], BAKED, r->cache)) { case MSSP_C_WRITE: r->cache = a[k]; break; case MSSP_C_CLEAR: r->cache = 0; break; }
				r->tls_used = 1; r->at = a[k]; return 1;
			}
			if (pass == 0 && a[k] == r->proven) r->proven = 0;
			if (pass == 1 && in(w->plain_ok, a[k])) { CHECK(mssp_after(0, a[k], BAKED, r->cache) == MSSP_C_NONE); r->tls_used = 0; r->at = a[k]; return 1; }
		}
	return 0;
}

int main(void)
{
	mssp_u32 o[MSSP_MAX], x, y;
	const char *h[MSSP_MAX];
	int n;

	/* order */
	n = mssp_order(0, BAKED, 0, 0, o, h); CHECK(n == 1 && o[0] == BAKED && !strcmp(h[0], "baked"));                       /* with no DNS answer */
	n = mssp_order(0, BAKED, 0, BAKED, o, h); CHECK(n == 1 && o[0] == BAKED);                                              /* DNS agrees: nothing extra */
	n = mssp_order(0, BAKED, 0, NEW, o, h); CHECK(n == 2 && o[0] == BAKED && o[1] == NEW && !strcmp(h[1], "dns"));         /* DNS differs: after the baked one */
	n = mssp_order(0, BAKED, NEW, NEW, o, h); CHECK(n == 2 && o[0] == BAKED && o[1] == NEW && !strcmp(h[1], "ota.ip"));    /* cache before dns, after baked */
	n = mssp_order(0, BAKED, NEW, LIE, o, h); CHECK(n == 3 && o[0] == BAKED && o[1] == NEW && o[2] == LIE);
	n = mssp_order(NEW, BAKED, NEW, LIE, o, h); CHECK(n == 3 && o[0] == NEW && !strcmp(h[0], "proven") && o[1] == BAKED && o[2] == LIE);
	n = mssp_order(BAKED, BAKED, 0, NEW, o, h); CHECK(n == 2 && o[0] == BAKED && o[1] == NEW);
	n = mssp_order(0, 0, 0, 0, o, h); CHECK(n == 0);

	/* fallback resolvers: the gateway guess, 1.1.1.1, 8.8.8.8, duplicates dropped */
	{ mssp_u32 r[3]; int k;
	  k = mssp_resolvers(IP(192, 168, 1, 1), IP(1, 1, 1, 1), IP(8, 8, 8, 8), r); CHECK(k == 3 && r[0] == IP(192, 168, 1, 1) && r[1] == IP(1, 1, 1, 1) && r[2] == IP(8, 8, 8, 8));
	  k = mssp_resolvers(0, IP(1, 1, 1, 1), IP(8, 8, 8, 8), r); CHECK(k == 2 && r[0] == IP(1, 1, 1, 1));
	  k = mssp_resolvers(IP(1, 1, 1, 1), IP(1, 1, 1, 1), IP(8, 8, 8, 8), r); CHECK(k == 2 && r[1] == IP(8, 8, 8, 8)); (void)x; (void)y; }

	/* answers */
	CHECK(mssp_answer_ok(BAKED, 0) && mssp_answer_ok(NEW, 0));
	CHECK(!mssp_answer_ok(0, 0) && !mssp_answer_ok(0, 1));
	CHECK(!mssp_answer_ok(IP(192, 168, 1, 50), 0) && !mssp_answer_ok(IP(10, 0, 0, 1), 0) && !mssp_answer_ok(IP(127, 0, 0, 1), 0) && !mssp_answer_ok(IP(172, 20, 0, 1), 0));
	CHECK(!mssp_answer_ok(IP(169, 254, 3, 3), 0) && !mssp_answer_ok(IP(100, 64, 0, 1), 0) && !mssp_answer_ok(IP(224, 0, 0, 1), 0) && !mssp_answer_ok(0xFFFFFFFFu, 0));
	CHECK(mssp_answer_ok(IP(172, 32, 0, 1), 0) && mssp_answer_ok(IP(100, 128, 0, 1), 0));
	CHECK(mssp_answer_ok(IP(127, 0, 0, 1), 1));                                                                              /* lab */

	/* cache decisions: only the pinned key writes; the baked address proving itself clears */
	CHECK(mssp_after(0, NEW, BAKED, 0) == MSSP_C_NONE);
	CHECK(mssp_after(1, NEW, BAKED, 0) == MSSP_C_WRITE);
	CHECK(mssp_after(1, NEW, BAKED, NEW) == MSSP_C_NONE);
	CHECK(mssp_after(1, NEW, BAKED, LIE) == MSSP_C_WRITE);
	CHECK(mssp_after(1, BAKED, BAKED, NEW) == MSSP_C_CLEAR);
	CHECK(mssp_after(1, BAKED, BAKED, 0) == MSSP_C_NONE);
	CHECK(mssp_after(1, 0, BAKED, 0) == MSSP_C_NONE);

	/* whole starts */
	{   /* today: the baked server answers with the pinned key: one try, nothing cached, the DNS answer (same) is never used */
		struct world w = { { BAKED }, { BAKED } };
		struct run r; memset(&r, 0, sizeof r);
		CHECK(request(&w, &r, BAKED) && r.at == BAKED && r.tried == 1 && r.cache == 0);
	}
	{   /* the resolver lies (another address): the baked server still answers first; the lie is never contacted */
		struct world w = { { BAKED }, { BAKED, LIE } };
		struct run r; memset(&r, 0, sizeof r);
		CHECK(request(&w, &r, LIE) && r.at == BAKED && r.tried == 1 && r.cache == 0);
	}
	{   /* the server moved, DNS knows: start 1 pays the baked timeout once, proves NEW, caches it; the second request in the same start goes to NEW at once */
		struct world w = { { NEW }, { NEW } };
		struct run r; memset(&r, 0, sizeof r);
		CHECK(request(&w, &r, NEW) && r.at == NEW && r.tls_used && r.tried == 2 && r.order[0] == BAKED && r.cache == NEW);
		r.tried = 0;
		CHECK(request(&w, &r, NEW) && r.at == NEW && r.tried == 1);
		/* start 2, DNS dead (or the record gone): the cache finds the server */
		r.proven = 0; r.tried = 0;
		CHECK(request(&w, &r, 0) && r.at == NEW && r.tried == 2 && r.order[0] == BAKED && r.order[1] == NEW);
	}
	{   /* moved, DNS lies: TLS fails on the lie, plain HTTP on the lie answers (files are verified by their signature, nothing proven, nothing cached) */
		struct world w = { { NEW }, { NEW, LIE } };
		struct run r; memset(&r, 0, sizeof r);
		CHECK(!request(&w, &r, 0));                                                              /* moved, no DNS, no cache: nothing (only a net patch or an update helps) */
		memset(&r, 0, sizeof r);
		CHECK(request(&w, &r, LIE) && r.at == LIE && !r.tls_used && r.cache == 0);
	}
	{   /* the baked server comes back after a move: it is tried first, proves itself, the cache goes */
		struct world w = { { BAKED }, { BAKED } };
		struct run r; memset(&r, 0, sizeof r); r.cache = NEW;
		CHECK(request(&w, &r, NEW) && r.at == BAKED && r.tried == 1 && r.cache == 0);
	}
	{   /* nothing answers, no DNS, no cache: the baked address only */
		struct world w = { { 0 }, { 0 } };
		struct run r; memset(&r, 0, sizeof r);
		CHECK(!request(&w, &r, 0) && r.tried == 2 && r.order[0] == BAKED && r.order[1] == BAKED);
	}
	{   /* the proven address dies during the start: the next request starts from the baked one again */
		struct world w = { { BAKED }, { BAKED } };
		struct run r; memset(&r, 0, sizeof r); r.proven = NEW;
		CHECK(request(&w, &r, 0) && r.order[0] == NEW && r.order[1] == BAKED && r.at == BAKED && r.proven == BAKED);
	}
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
