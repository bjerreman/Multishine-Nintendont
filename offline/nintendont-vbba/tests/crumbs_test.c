/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/* Encoding of the breadcrumb words (core/vbba_crumbs.h): every field round-trips at its limits, the age handles the 26-bit wrap, the trail keeps the
   last five sites, the name tables cover every site, the sites fit the 6-bit field.  The kernel writes these words, the probe reads them. */
#include <stdio.h>
#include <string.h>
#define VBBA_BC_NAMES
#include "../core/vbba_crumbs.h"
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
	unsigned s, t, i, tr, w;
	CHECK(BCF_COUNT <= 64 && BCM_COUNT <= 64 && BCD_COUNT <= 64 && BCI_COUNT <= 16);
	for (s = 0; s < BCF_COUNT; s++) { w = vbba_bc_w(s, 0x03ABCDEFu); CHECK(vbba_bc_site(w) == s && vbba_bc_tick(w) == 0x03ABCDEFu); CHECK(strcmp(vbba_bcf_name(s), "?") != 0 && strlen(vbba_bcf_name(s)) <= 8); }
	for (s = 0; s < BCM_COUNT; s++) CHECK(strcmp(vbba_bcm_name(s), "?") != 0 && strlen(vbba_bcm_name(s)) <= 8);
	for (s = 0; s < BCD_COUNT; s++) CHECK(strcmp(vbba_bcd_name(s), "?") != 0 && strlen(vbba_bcd_name(s)) <= 8);
	for (s = 0; s < BCI_COUNT; s++) CHECK(strcmp(vbba_bci_name(s), "?") != 0 && strlen(vbba_bci_name(s)) <= 8);
	CHECK(!strcmp(vbba_bcf_name(BCF_COUNT), "?") && !strcmp(vbba_bcm_name(63), "?") && !strcmp(vbba_bcd_name(40), "?") && !strcmp(vbba_bci_name(15), "?"));
	{   /* a tick above 26 bits is cut, never spills into the site */
		w = vbba_bc_w(BCF_DMAIN, 0xFFFFFFFFu); CHECK(vbba_bc_site(w) == BCF_DMAIN && vbba_bc_tick(w) == 0x03FFFFFFu);
	}
	{   /* ages: plain, across the 26-bit wrap, and a crumb newer than the reference reads 0 */
		unsigned ref = 5000100u;
		CHECK(vbba_bc_age(ref, vbba_bc_w(1, ref - 3796u)) == 3796u);
		CHECK(vbba_bc_age(ref, vbba_bc_w(1, ref)) == 0u);
		CHECK(vbba_bc_age(0x00000010u, vbba_bc_w(1, 0x03FFFFF0u)) == 0x20u);         /* the reference wrapped past the stamp */
		CHECK(vbba_bc_age(ref, vbba_bc_w(1, ref + 50u)) == 0u);                        /* stamped just after the reference (another thread's newer tick) */
		CHECK(vbba_bc_age(ref, vbba_bc_w(1, ref - 0x01FFFFFFu)) == 0x01FFFFFFu);     /* the largest age it can show: 17 s */
	}
	for (i = 0; i < 4; i++) {
		static const unsigned cnt[4] = { 0, 1, 1023, 1024 + 17 };
		static const int res[4] = { 0, -1, -116, 32767 };
		w = vbba_bc_done_w(BCD_PUSHED, cnt[i], (unsigned)res[i]);
		CHECK(vbba_bc_site(w) == BCD_PUSHED && vbba_bc_done_count(w) == (cnt[i] & 1023u) && vbba_bc_done_res(w) == res[i]);
	}
	CHECK(vbba_bc_done_res(vbba_bc_done_w(1, 0, 0xFFFF8000u)) == -32768);       /* a result outside 16 bits keeps its low half, sign included */
	for (s = BCI_NCD; s < BCI_COUNT; s++) for (t = 1; t <= 3; t++) {
		int rc = t == BCS_ERR ? -22 : 0;
		w = vbba_bc_ios_w(s, t, 31, rc);
		CHECK(vbba_bc_ios_id(w) == s && vbba_bc_ios_state(w) == t && vbba_bc_ios_slot(w) == 31 && vbba_bc_ios_rc(w) == rc);
	}
	CHECK(vbba_bc_ios_rc(vbba_bc_ios_w(BCI_KD, BCS_OK, 0, -15)) == -15 && vbba_bc_ios_slot(vbba_bc_ios_w(BCI_KD, BCS_OK, 255, 0)) == 255);
	CHECK(vbba_bc_ios_w(BCI_CLOSE, BCS_IN, 3, 0) >> 28 == BCI_CLOSE);
	w = vbba_bc_hdr_w(0, 0); CHECK(vbba_bc_hdr_ok(w) && vbba_bc_hdr_fast(w) == 0 && vbba_bc_hdr_done(w) == 0);
	w = vbba_bc_hdr_w(8192, 1024); CHECK(vbba_bc_hdr_ok(w) && vbba_bc_hdr_fast(w) == 8192u && vbba_bc_hdr_done(w) == 1024u);
	w = vbba_bc_hdr_w(1824, 392); CHECK(vbba_bc_hdr_fast(w) == 1824u && vbba_bc_hdr_done(w) == 392u);
	w = vbba_bc_hdr_w(100000, 100000); CHECK(vbba_bc_hdr_ok(w) && vbba_bc_hdr_fast(w) == 1023u * 16u && vbba_bc_hdr_done(w) == 1023u * 4u);   /* saturates, never spills */
	CHECK(!vbba_bc_hdr_ok(0) && !vbba_bc_hdr_ok(0x56425854u) /* 'VBXT' of the mainline extension pointer word */ && !vbba_bc_hdr_ok(0xBC000000u | (2u << 20)));
	for (tr = 0, s = 1; s <= 9; s++) tr = vbba_bc_trail_push(tr, s);
	CHECK(vbba_bc_trail_at(tr, 0) == 9 && vbba_bc_trail_at(tr, 1) == 8 && vbba_bc_trail_at(tr, 4) == 5 && (tr >> 30) == 0);
	CHECK(vbba_bc_trail_push(0x3FFFFFFFu, 63) == 0x3FFFFFFFu && vbba_bc_trail_push(0, 64 + 5) == 5u);
	/* the words 121..127 are the pad of the 512-byte block: seven words, no overlap with 'VBXT'/pointer of the mainline extension convention except word 126 (checked above) */
	CHECK(VBBA_BC_FAST == 121 && VBBA_BC_TRAIL == 127 && VBBA_BC_HDR == 126);
	printf("crumbs: %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
	return fails != 0;
}
