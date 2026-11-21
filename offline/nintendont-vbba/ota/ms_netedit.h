/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Settings > Network, the Wii network profile editor.  Included by ms_ota_loader.c (it uses that file's statics: ota_log, ota_net_up, conn_loop,...); the pure
 * part (layout, validation, row edit) is ota/ms_netcfg_core.[ch], host-tested.  Wii only: the loader's Settings page offers it through ms_net_edit_available (hidden on a Wii U / vWii, where ioctlv 4 is
 * untested, and on any build with MS_NO_NETEDIT).
 *
 * RULES (design rule): the password is NEVER logged, written to the SD, or kept after the screen closes (struct and buffers wiped); the log gets lengths and codes only; the full config block is
 * never stored on the SD; one row is edited, read back and compared, and the old row is put back when the write or the test fails; a bad result is always fixable in Wii Settings > Internet.
 *
 * CALLS (WiiBrew /dev/net/ncd/manage, https://wiibrew.org/wiki//dev/net/ncd/manage; the loader's own conn_apply_profile makes the same two calls):
 *   ioctlv 3 (0 in, 2 out: 0x1B5C config + 0x20 status) = the live config the network stack uses, ioctlv 4 (1 in: config, 1 out: status) = hand it back (Nintendont's main.c does exactly this at game launch),
 *   ioctlv 5 = NCDReadConfig, config.dat as stored (read only here: used after a write to say whether the Wii also kept it for next time).
 * WI-FI SCAN: not built.  WiiBrew lists /dev/net/wd/command WD_Scan (ioctl 0x100A) but documents neither the BSS result layout, nor the open/initialise/disable sequence, nor the output buffer size;
 * guessing them blind risks the IOS Wi-Fi driver the network stack is using.  The name is typed (or taken from another saved connection); see the design doc.
 */
#ifndef MS_NETEDIT_H
#define MS_NETEDIT_H
#include "ms_netcfg_core.h"

enum { NE_LEN = 0x1B5C, NE_ST = 0x1B60, NE_TOT = 0x1B80 };
extern NIN_CFG *ncfg;

int ms_net_edit_available(void)
{
#ifdef MS_NO_NETEDIT
	return 0;
#else
	return !IsWiiU();
#endif
}

/* ---- ncd/manage config I/O ------------------------------------------------------------------------------------------------------------------------------------------- */
struct ne_ctx { ioctlv *cv; u8 *cb; };
static int ne_ctx_new(struct ne_ctx *c)
{
	c->cv = (ioctlv *)memalign(32, sizeof(ioctlv) * 2);
	c->cb = (u8 *)memalign(32, NE_TOT);
	return (c->cv && c->cb) ? 0 : -1;
}
static void ne_ctx_free(struct ne_ctx *c)
{
	if (c->cb) { msn_wipe(c->cb, NE_TOT); free(c->cb); }
	if (c->cv) free(c->cv);
	c->cb = NULL; c->cv = NULL;
}
/* cmd 3 (live) or 5 (stored): the block into blk (NE_LEN).  0 = ok, negative = IOS error, -9999 = status word not 0 */
static int ne_rd(struct ne_ctx *c, s32 fd, int cmd, u8 *blk, int *st)
{
	s32 rc;
	memset(c->cb, 0, NE_TOT);
	c->cv[0].data = c->cb; c->cv[0].len = NE_LEN; c->cv[1].data = c->cb + NE_ST; c->cv[1].len = 0x20;
	DCFlushRange(c->cb, NE_TOT); DCFlushRange(c->cv, sizeof(ioctlv) * 2);
	rc = IOS_Ioctlv(fd, cmd, 0, 2, c->cv);
	DCInvalidateRange(c->cb, NE_TOT);
	*st = (int)*(u32 *)(c->cb + NE_ST);
	if (rc < 0) return (int)rc;
	if (*st != 0) return -9999;
	memcpy(blk, c->cb, NE_LEN);
	return 0;
}
/* ioctlv 4 (live config) or 6 (NCDWriteConfig: the stored config.dat): hand a block over.  The status word is logged, not required to be 0 (its value after a good write is not documented); the read-back decides */
static int ne_wr(struct ne_ctx *c, s32 fd, int cmd, const u8 *blk, int *st)
{
	s32 rc;
	memset(c->cb, 0, NE_TOT);
	memcpy(c->cb, blk, NE_LEN);
	c->cv[0].data = c->cb; c->cv[0].len = NE_LEN; c->cv[1].data = c->cb + NE_ST; c->cv[1].len = 0x20;
	DCFlushRange(c->cb, NE_TOT); DCFlushRange(c->cv, sizeof(ioctlv) * 2);
	rc = IOS_Ioctlv(fd, cmd, 1, 1, c->cv);
	DCInvalidateRange(c->cb, NE_TOT);
	*st = (int)*(u32 *)(c->cb + NE_ST);
	return rc < 0 ? (int)rc : 0;
}
/* one read with its own open/close; blk = NE_LEN bytes */
static int ne_read_once(int cmd, u8 *blk, int *st)
{
	struct ne_ctx c;
	s32 fd;
	int rc;
	*st = 0;
	if (ne_ctx_new(&c) != 0) { ne_ctx_free(&c); return -1; }
	fd = IOS_Open("/dev/net/ncd/manage", 0);
	if (fd < 0) { ne_ctx_free(&c); return (int)fd; }
	rc = ne_rd(&c, fd, cmd, blk, st);
	IOS_Close(fd);
	ne_ctx_free(&c);
	return rc;
}
/* write `blk` (a block made from a fresh read) with cmd 4 (live) or 6 (stored), then read it back the same way (cmd 3 live, cmd 5 stored) into chk: 0 = written and read back */
static int ne_write_verify(int cmd, const u8 *blk, u8 *chk, int *wst, int *rst)
{
	struct ne_ctx c;
	s32 fd;
	int rc;
	if (ne_ctx_new(&c) != 0) { ne_ctx_free(&c); return -1; }
	fd = IOS_Open("/dev/net/ncd/manage", 0);
	if (fd < 0) { ne_ctx_free(&c); return (int)fd; }
	rc = ne_wr(&c, fd, cmd, blk, wst);
	IOS_Close(fd);
	if (rc == 0) {
		fd = IOS_Open("/dev/net/ncd/manage", 0);
		if (fd < 0) rc = (int)fd;
		else { rc = ne_rd(&c, fd, cmd == 6 ? 5 : 3, chk, rst); IOS_Close(fd); }
	}
	ne_ctx_free(&c);
	return rc;
}

/* put one old block's row (and its selection) back onto a fresh read of the same kind (cmd 4 / 3 live, cmd 6 / 5 stored); 0 = the row reads back as it was */
static int ne_restore_one(int wcmd, unsigned prof, const u8 *old)
{
	u8 *cur = (u8 *)malloc(NE_LEN), *chk = (u8 *)malloc(NE_LEN);
	int rc = -1, st = 0, wst = 0, rst = 0;
	if (cur && chk) {
		rc = ne_read_once(wcmd == 6 ? 5 : 3, cur, &st);
		if (rc == 0 && msn_restore(cur, NE_LEN, prof, old) == MSN_OK) {
			rc = ne_write_verify(wcmd, cur, chk, &wst, &rst);
			if (rc == 0 && !msn_row_equal(old, chk, NE_LEN, prof)) rc = -2;
		} else if (rc == 0) rc = -3;
	}
	ota_log("netedit: old row of profile %u put back in the %s config: %s (rc %d, write status %d, read status %d)", prof, wcmd == 6 ? "stored" : "live", rc == 0 ? "ok" : "FAILED", rc, wst, rst);
	if (cur) { msn_wipe(cur, NE_LEN); free(cur); }
	if (chk) { msn_wipe(chk, NE_LEN); free(chk); }
	return rc;
}
/* both configs; old_s = NULL when the stored one was never changed */
static int ne_restore(unsigned prof, const u8 *old_live, const u8 *old_s)
{
	int a = old_live ? ne_restore_one(4, prof, old_live) : 0, b = old_s ? ne_restore_one(6, prof, old_s) : 0;
	return (a == 0 && b == 0) ? 0 : -1;
}

/* ---- the screen state (wiped when the screen closes) ---------------------------------------------------------------------------------------------------------------- */
static struct msn_row ne_row;             /* the edit buffer of the chosen connection (holds the password) */
static unsigned ne_prof;                  /* 1..3 the connection being edited, 0 = none chosen */
static int ne_keep;                       /* the live connection is not the one the Wii keeps for the next start */
static int ne_edit;                       /* a field of the row was changed on this screen (name, type, security, password).  0 with ne_dirty = only the selection differs: Save writes the selection alone and leaves every byte of the row as the Wii wrote it (a rewritten but unchanged row did not connect) */
static int ne_dirty;                      /* the buffer differs from what the Wii holds */
static unsigned ne_pend;                  /* the connection chosen on this screen (0 = Auto): Nintendont's Network Profile is set to it only after a good Save and test */
static int ne_rd_rc, ne_rd_st;            /* the last config read */
static unsigned ne_start_prof;            /* the Network Profile in SD:/multishine/nincfg.bin (what the loader selects at every start) */
static int ne_start_wired = -1;           /* that connection's kind in the live config: 1 cable, 0 Wi-Fi, -1 not known */
static unsigned ne_snap_prof;             /* the profile that belongs to the put-back snapshot (ne_snap_take): a put-back returns the file to it too */
static int ne_prof_written;               /* the profile was written in this visit: applied (conn_apply_profile) when the screen closes */
/* The settings that were live when the screen OPENED (live config and stored config, RAM only: wiped and freed when the screen closes, never written anywhere, never logged).  A put-back after a failed
   Save and test always restores from these, never from a read made at the save, so a failed save that was kept can not become the 'old' settings of the next put-back.
   Refreshed only after a save whose test PASSED (that save is a good state of its own). */
static u8 *ne_ent_l, *ne_ent_s;
static void ne_snap_free(void)
{
	if (ne_ent_l) { msn_wipe(ne_ent_l, NE_LEN); free(ne_ent_l); }
	if (ne_ent_s) { msn_wipe(ne_ent_s, NE_LEN); free(ne_ent_s); }
	ne_ent_l = ne_ent_s = NULL;
}
static void ne_snap_take(void)
{
	int st = 0;
	ne_snap_free();
	ne_ent_l = (u8 *)malloc(NE_LEN); ne_ent_s = (u8 *)malloc(NE_LEN);
	if (!ne_ent_l || !ne_ent_s || ne_read_once(3, ne_ent_l, &st) != 0 || ne_read_once(5, ne_ent_s, &st) != 0) ne_snap_free();       /* no snapshot: a put-back falls back to the read made at the save */
}

static void ne_clear(void) { msn_wipe(&ne_row, (u32)sizeof ne_row); ne_prof = 0; ne_dirty = 0; ne_keep = 0; ne_edit = 0; }

/* which connection the screen edits: Nintendont's Network Profile when set, else the one the Wii has selected */
static unsigned ne_target(const u8 *blk)
{
	unsigned p = ne_pend, k;
	if (p) return p;
	for (k = 1; k <= MSN_ROWS; k++) if (blk[msn_row_off(k)] & MSN_F_SELECTED) return k;
	return 0;
}
/* read the Wii's config and load the chosen connection into the buffer (the rest of the block is dropped at once) */
static void ne_reload(void)
{
	u8 *blk = (u8 *)malloc(NE_LEN);
	ne_clear();
	if (!blk) { ne_rd_rc = -1; return; }
	ne_rd_rc = ne_read_once(3, blk, &ne_rd_st);
	if (ne_rd_rc == 0) {
		ne_prof = ne_target(blk);
		if (ne_prof) msn_parse(blk, NE_LEN, ne_prof, &ne_row);
		{ struct msn_info in; ne_start_wired = -1; if (ne_start_prof >= 1 && ne_start_prof <= 3 && msn_describe(blk, NE_LEN, ne_start_prof, &in) == MSN_OK) ne_start_wired = in.wired ? 1 : 0; }   /* the 'Used at start' line */
		if (ne_prof && !ne_row.present) { ne_row.sec = MSN_SEC_WPA2_AES; ne_row.ip_dhcp = ne_row.dns_dhcp = 1; }    /* a connection that was never set up starts as Wi-Fi with WPA2 */
		if (ne_prof && ne_row.present) {                         /* : the live config had connection 1 selected (the loader's boot-time select) while the STORED file still said connection 2: offer the save without an edit, or the Wii never keeps it */
			int sst = 0;
			if (ne_read_once(5, blk, &sst) == 0 && (!(blk[msn_row_off(ne_prof)] & MSN_F_SELECTED) || !msn_row_stored(blk, NE_LEN, ne_prof, &ne_row))) { ne_dirty = 1; ne_keep = 1; }
		}
	}
	ota_log("netedit: config read rc %d status %d, editing profile %u (chosen %u, Network Profile %u)", ne_rd_rc, ne_rd_st, ne_prof, ne_pend, (unsigned)(ncfg ? (ncfg->NetworkProfile & 3u) : 0u));
	msn_wipe(blk, NE_LEN);
	free(blk);
}

/* ---- the Test: restart the network on the config the Wii holds now, then a short run of the connection check's own probe ----------------------------------------- */
static void ne_msg_screen(const char *title, const char *const *l, const u32 *col, int n);
/* wait for the network after a (re)start: up to budget_ms, one calm line "Connecting... N s" (BLACK), B stops.  0 up, -1 init call failed, -2 error status, -3 not up in time, -4 stopped by the user.
   Status -EBUSY / -EAGAIN / -ETIMEDOUT mean "still working" (the Wii needs 4 s to 25 s here). */
static int ne_net_wait(u32 budget_ms)
{
	u32 t0, shown = 0xFFFFFFFFu;
	s32 st;
	if (ota_net_start() != 0) return -1;
	t0 = now_ms();
	for (;;) {
		const u32 el = (u32)(now_ms() - t0);
		st = net_get_status();
		g_net_st = (int)st;
		if (st == 0) { ota_log("netedit: net up after %lu ms", (unsigned long)el); return 0; }
		if (st < 0 && st != -EBUSY && st != -ETIMEDOUT && st != -EAGAIN) { ota_log("netedit: net error status %d after %lu ms", (int)st, (unsigned long)el); return -2; }
		if (el >= budget_ms) { ota_log("netedit: net not up after %lu ms (last status %d)", (unsigned long)el, (int)st); return -3; }
		if (el / 1000u != shown) {
			char a[40];
			const char *l[2];
			shown = el / 1000u;
			snprintf(a, sizeof a, "Connecting... %lu s", (unsigned long)shown);
			l[0] = a; l[1] = "B stops the test.";
			ne_msg_screen("Network", l, NULL, 2);
		}
		FPAD_Update();
		if (FPAD_Cancel(0)) { ota_log("netedit: net wait stopped by the user at %lu ms", (unsigned long)el); return -4; }
		usleep(20 * 1000);
	}
}

/* 0 = connected and the internet answered (verdict in *vd), 1 = connected but nothing answered, 2 = could not connect, 3 = the user stopped it.
   The network is restarted on the config the Wii holds now and given 90 s; the probe starts only once it is up, with its own clock (CONN_DURATION_US, the connection check's normal run); when it did not
   even send (our log: sent 0 after a 23 s start) it is run once more on a fresh socket after a pause.  The log gets the call timings that would show where the time goes. */
static int ne_test(int *vd, char *why, size_t wn)
{
	struct msp_result r;
	u32 hip, gw, t0, tsock;
	int nr, att, skipped = 0;
	*vd = MSP_V_NOTRUN;
	why[0] = 0;
	ota_net_down();
	usleep(300 * 1000);
	nr = ne_net_wait(90000);          /* was 30 s: after a config write the Wii needed longer to rejoin (first Save and test "no network" at 30 s, the second connected) */
	if (nr == -4) { ota_net_down(); snprintf(why, wn, "Test stopped"); return 3; }
	if (nr != 0) {
		snprintf(why, wn, "No network (code %d)", g_net_rc < 0 ? g_net_rc : g_net_st);
		ota_log("netedit: test: no network (return %d, init %d, status %d)", nr, g_net_rc, g_net_st);
		ota_net_down();
		return 2;
	}
	hip = net_gethostip();
	if (!hip) { snprintf(why, wn, "The network is up but has no address"); ota_log("netedit: test: up without an address"); ota_net_down(); return 2; }
	gw = (hip & 0xFFFFFF00u) | 1u;
	usleep(1000 * 1000);                                                        /* a calm second: the stack has just come up */
	memset(&r, 0, sizeof r);
	for (att = 0; att < 2; att++) {
		s32 s, fcl;
		const u32 tb = conn_us();
		s = net_socket(PF_INET, SOCK_DGRAM, 0);
		if (s < 0) { snprintf(why, wn, "Could not open a socket (%d)", (int)s); ota_log("netedit: test: socket failed %d", (int)s); ota_net_down(); return 2; }
		fcl = net_fcntl(s, F_GETFL, 0);
		if (fcl < 0 || net_fcntl(s, F_SETFL, fcl | 4) < 0) { net_close(s); snprintf(why, wn, "Could not set up the check"); ota_net_down(); return 2; }
		t0 = conn_us();
		tsock = (u32)(t0 - tb);
		msp_init(&g_probe, t0, t0 ^ (hip * 2654435761u) ^ (u32)att, CONN_DURATION_US, 0);
		msp_default_legs(&g_probe, hip, gw, 0);
		skipped = conn_loop(s, &g_probe, CONN_DURATION_US, t0);
		net_close(s);
		msp_result(&g_probe, &r);
		ota_log("netedit: test: try %d ip %u.%u.%u.%u, leg %u, verdict %u, loss %lu.%02lu%%, p50 %luus, sent %lu, recv %lu, send_fail %lu, call_max %luus, slow_calls %lu, socket %lums, loop %lums%s", att + 1, IPQ(hip),
		        (unsigned)r.leg, (unsigned)r.verdict, (unsigned long)(r.loss_x100 / 100u), (unsigned long)(r.loss_x100 % 100u), (unsigned long)r.p50, (unsigned long)r.sent, (unsigned long)r.recv, (unsigned long)r.send_fail,
		        (unsigned long)r.call_max_us, (unsigned long)r.slow_calls, (unsigned long)(tsock / 1000u), (unsigned long)((conn_us() - t0) / 1000u), skipped ? ", stopped by the user" : "");
		if (skipped || r.sent > 0) break;
		usleep(2000 * 1000);
	}
	*vd = (int)r.verdict;
	ota_net_down();
	if (skipped) return 3;
	return (r.verdict == MSP_V_GOOD || r.verdict == MSP_V_OKAY || r.verdict == MSP_V_POOR) ? 0 : 1;
}

static void ne_msg_draw(const char *title, const char *const *l, const u32 *col, int n)
{
	int i, y = MENU_POS_Y + 60;
	ms_title(title);
	for (i = 0; i < n; i++) { y += 20 * wrap_text(MENU_POS_X, y, l[i], col ? col[i] : BLACK, 590, 20, DEFAULT_SIZE) + 4; }
}
static void ne_msg_screen(const char *title, const char *const *l, const u32 *col, int n)
{
	ClearScreen();
	ne_msg_draw(title, l, col, n);
	UpdateScreen();
	ClearScreen();
}
static const char *ne_verdict_text(int rc, int vd, const char *why)
{
	if (rc == 0) return vd == MSP_V_GOOD ? "Connected. The connection is fine." : vd == MSP_V_OKAY ? "Connected. The connection is decent. A cable is best." : "Connected, but unsteady right now. A cable is best.";      /* wording: may be a cable or Wi-Fi, so no "Wi-Fi" claim; never alarming, a cable is best */
	if (rc == 1) return "Connected to the network, but the internet did not answer.";
	if (rc == 3) return "Test stopped.";
	return why[0] ? why : "Could not connect.";
}

/* ---- Save and test: edit one row in the STORED config (ioctlv 6) and the live one (ioctlv 4), read both back, compare, test, put the old row back when anything fails ------------------------ */
/* "Saved" is said only when the stored file (ioctlv 5) really holds the row.  The old blocks live in RAM only, until this function ends.
   returns 0 = saved and tested, 1 = saved (kept although the test did not pass), -1 = written and then put back (the edit buffer stays for another try), -2 = nothing was touched */
static int ne_save(char *msg, size_t mn, u32 *color)
{
	u8 *oldl = NULL, *olds = NULL, *cur = NULL, *chk = NULL;
	int rc = -1, st = 0, wst = 0, rst = 0, ret = -2, v, vd = 0, tr, s_written = 0;
	const u8 *rl, *rs;                                                         /* what a put-back restores: the settings of the screen's opening (snapshot), else this save's own read */
	char why[80];
	*color = MAROON;
	v = msn_validate(&ne_row);
	if (!ne_prof) { snprintf(msg, mn, "%s", msn_v_text(MSN_E_PROFILE)); return -2; }
	if (v != MSN_OK) { snprintf(msg, mn, "%s", msn_v_text(v)); return -2; }
	oldl = (u8 *)malloc(NE_LEN); olds = (u8 *)malloc(NE_LEN); cur = (u8 *)malloc(NE_LEN); chk = (u8 *)malloc(NE_LEN);
	if (!oldl || !olds || !cur || !chk) { snprintf(msg, mn, "Out of memory. Nothing was changed."); goto done; }
	rc = ne_read_once(3, oldl, &st);
	if (rc == 0) rc = ne_read_once(5, olds, &st);
	if (rc != 0) { snprintf(msg, mn, "The Wii network settings could not be read (code %d). Nothing was changed.", rc); ota_log("netedit: save: read failed rc %d status %d", rc, st); goto done; }
	rl = ne_ent_l ? ne_ent_l : oldl; rs = ne_ent_s ? ne_ent_s : olds;
	ota_log("netedit: save: %s; profile %u kind %s, name length %u, security %d, key length %u", ne_edit ? "row rewritten" : "selection only, row bytes untouched", ne_prof, ne_row.wired ? "cable" : "wifi", (unsigned)strlen(ne_row.ssid), ne_row.sec, (unsigned)strlen(ne_row.key));
	/* 1. the stored config */
	memcpy(cur, olds, NE_LEN);
	if ((ne_edit && msn_serialize(cur, NE_LEN, ne_prof, &ne_row) != MSN_OK) || msn_select(cur, NE_LEN, ne_prof) != MSN_OK) { snprintf(msg, mn, "These settings cannot be written. Nothing was changed."); goto done; }
	rc = ne_write_verify(6, cur, chk, &wst, &rst);
	s_written = 1;
	ota_log("netedit: save: stored config write (ioctlv 6) rc %d, write status %d, read-back (ioctlv 5) status %d, row stored %d, rest unchanged %d", rc, wst, rst, rc == 0 ? msn_row_stored(chk, NE_LEN, ne_prof, &ne_row) : -1, rc == 0 ? msn_rest_equal(olds, chk, NE_LEN, ne_prof) : -1);
	if (rc != 0 || !msn_row_stored(chk, NE_LEN, ne_prof, &ne_row) || !msn_rest_equal(olds, chk, NE_LEN, ne_prof)) {
		int rr = ne_restore(ne_prof, NULL, rs);
		snprintf(msg, mn, rr == 0 ? "The Wii did not store the change. The old settings are back." : "The Wii did not store the change and the old settings could not be put back. Fix it in Wii Settings > Internet.");
		ret = -1;
		goto done;
	}
	/* 2. the live config (what the network stack uses now) */
	memcpy(cur, oldl, NE_LEN);
	if ((ne_edit && msn_serialize(cur, NE_LEN, ne_prof, &ne_row) != MSN_OK) || msn_select(cur, NE_LEN, ne_prof) != MSN_OK) { snprintf(msg, mn, "These settings cannot be written."); ret = -1; ne_restore(ne_prof, NULL, rs); goto done; }
	rc = ne_write_verify(4, cur, chk, &wst, &rst);
	ota_log("netedit: save: live config write (ioctlv 4) rc %d, write status %d, read-back status %d, row stored %d, rest unchanged %d", rc, wst, rst, rc == 0 ? msn_row_stored(chk, NE_LEN, ne_prof, &ne_row) : -1, rc == 0 ? msn_rest_equal(oldl, chk, NE_LEN, ne_prof) : -1);
	if (rc != 0 || !msn_row_stored(chk, NE_LEN, ne_prof, &ne_row) || !msn_rest_equal(oldl, chk, NE_LEN, ne_prof)) {
		int rr = ne_restore(ne_prof, rl, rs);
		snprintf(msg, mn, rr == 0 ? "The Wii did not take the change. The old settings are back." : "The Wii did not take the change and the old settings could not be put back. Fix it in Wii Settings > Internet.");
		ret = -1;
		goto done;
	}
	{ const char *l[2] = { "Saved. Testing the connection...", "After a save this can take over a minute. B stops the test." }; ne_msg_screen("Network", l, NULL, 2); }
	tr = ne_test(&vd, why, sizeof why);
	if (tr == 0 || tr == 1 || tr == 3) {
		/* 0: good.  1: up but the internet did not answer: the settings work as far as the network goes, keep them.  3: stopped by the user: keep them. */
		snprintf(msg, mn, "Saved. %s", ne_verdict_text(tr, vd, why));
		*color = BLACK; ret = tr == 0 ? 0 : 1;
		if (tr == 0) ne_snap_take();                                          /* a save that passed its test is a good state: a later put-back returns to it, not past it */
		goto done;
	}
	{                                                                          /* could not connect: offer to put the old row back */
		const char *l[4]; u32 col[4]; u64 t;
		int keep = 0, armed = 0;
		snprintf(msg, mn, "%s", why);
		l[0] = "The test failed:"; col[0] = MAROON;
		l[1] = msg; col[1] = MAROON;
		l[2] = ne_row.wired ? "Check that the cable is plugged in." : "Check the name, the password and the security type."; col[2] = BLACK;
		l[3] = "Your old password will be lost if you keep this."; col[3] = MAROON;
		for (;;) {
			ClearScreen(); ne_msg_draw("Network", l, col, 4);
			if (armed) ms_foot(0, "A", "Put the old settings back", "Y", "Keep these anyway", NULL);          /* second step: Y again keeps */
			else ms_foot(0, "A", "Put the old settings back", "Y", "Keep these", NULL);
			UpdateScreen(); ClearScreen();
			t = ticks_to_millisecs(gettime()) + 600;
			for (;;) {
				VIDEO_WaitVSync(); FPAD_Update();
				if (ticks_to_millisecs(gettime()) < t) continue;
				if (FPAD_OK(0)) { keep = 0; armed = -1; break; }
				if (FPAD_Y(0)) { if (armed) { keep = 1; armed = -1; } else { armed = 1; l[3] = "Your old password will be lost if you keep this. Press Y again to keep."; } break; }
			}
			if (armed < 0) break;
		}
		if (keep) { snprintf(msg, mn, "Kept, although the test failed."); ret = 1; goto done; }
		{ int rr = ne_restore(ne_prof, rl, rs);
		  snprintf(msg, mn, rr == 0 ? "The old settings are back." : "The old settings could not be put back. Fix it in Wii Settings > Internet.");
		  ret = -1; }
	}
done:
	(void)s_written;
	if (oldl) { msn_wipe(oldl, NE_LEN); free(oldl); }
	if (olds) { msn_wipe(olds, NE_LEN); free(olds); }
	if (cur) { msn_wipe(cur, NE_LEN); free(cur); }
	if (chk) { msn_wipe(chk, NE_LEN); free(chk); }
	return ret;
}

/* ---- on-screen keyboard ---------------------------------------------------------------------------------------------------------------------------------------------- */
static const char *const ne_kb_page[3][4] = {
	{ "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@" },
	{ "!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~        " },
};
enum { KB_X0 = 60, KB_W = 52, KB_H = 30 };
static void kb_draw(const char *title, const char *buf, unsigned maxlen, int secret, int reveal, int page, int cr, int cc)
{
	char shown[72];
	const int len = (int)strlen(buf), off = len > 50 ? len - 50 : 0;
	int y = MENU_POS_Y + 36, r, c, i;
	static const char *const act[5] = { "", "Space", "Delete", "Done", "Cancel" };
	ms_title(title);
	for (i = 0; i < len - off && i < 70; i++) shown[i] = (secret && !reveal) ? '*' : buf[off + i];
	shown[i] = 0;
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 20, y, "%s_", shown);
	PrintFormat(14, BLACK, MENU_POS_X + 20, y + 24, "%d of %u characters%s", len, maxlen, secret ? (reveal ? "  (shown while Y is held)" : "  (hold Y to show)") : "");
	y += 62;
	for (r = 0; r < 4; r++) {
		for (c = 0; c < 10; c++) {
			const char ch = ne_kb_page[page][r][c];
			const int x = KB_X0 + c * KB_W + (KB_W - 10) / 2, yy = y + r * KB_H;
			if (cr == r && cc == c) ms_sel_pill(x, yy, 34, 22);
			if (ch != ' ') PrintFormat(DEFAULT_SIZE, BLACK, x, yy, "%c", ch);
		}
	}
	for (c = 0; c < 5; c++) {
		const char *lab = c == 0 ? (page == 0 ? "Shift" : page == 1 ? "Symbols" : "Letters") : act[c];
		const int w = (int)GRRLIB_WidthTTF(myFont, lab, DEFAULT_SIZE), x = KB_X0 + 10 + c * 104, yy = y + 4 * KB_H + 6;
		if (cr == 4 && cc / 2 == c) ms_sel_pill(x, yy, w + 20, 22);
		PrintFormat(DEFAULT_SIZE, BLACK, x + 0, yy, "%s", lab);
	}
	if (secret) ms_foot(0, "A", "Type", "B", "Delete", "X", "Page", "Start", "Done", "Hold Y", "Show", NULL);
	else ms_foot(0, "A", "Type", "B", "Delete", "X", "Page", "Start", "Done", NULL);
}
/* edits buf (capacity maxlen + 1); 1 = Done with a changed text, 0 = cancelled or unchanged (buf unchanged) */
/* one direction step per 220 ms at most: the loader's FPAD repeats a held direction far too fast for a grid or a short list.  The four FPAD calls are made ONCE here (a call that
   returns true locks the others for the frame, so asking again later gives false); *dx / *dy = the step.  The limit runs on time alone: FPAD returns false on the frames between its own repeats. */
static int ne_dir_step(u64 *next, int *dx, int *dy)
{
	const int u = FPAD_Up(0), d = FPAD_Down(0), l = FPAD_Left(0), r = FPAD_Right(0);
	const u64 now = ticks_to_millisecs(gettime());
	*dx = 0; *dy = 0;
	if (!(u || d || l || r)) return 0;
	if (now < *next) return 0;
	*next = now + 220;
	if (u) *dy = -1; else if (d) *dy = 1; else if (l) *dx = -1; else *dx = 1;
	return 1;
}

static int ne_keyboard(const char *title, char *buf, unsigned maxlen, int secret)
{
	char work[72];
	int page = 0, cr = 1, cc = 0, redraw = 1, reveal = 0, done = -1, dx, dy;
	u64 delay = ticks_to_millisecs(gettime()) + 400, dir_next = 0;
	snprintf(work, sizeof work, "%.*s", (int)(maxlen < 70 ? maxlen : 70), buf);
	while (done < 0) {
		const int rv = (secret && FPAD_Y(1)) ? 1 : 0;
		if (rv != reveal) { reveal = rv; redraw = 1; }
		if (redraw) { ClearScreen(); kb_draw(title, work, maxlen, secret, reveal, page, cr, cc); UpdateScreen(); ClearScreen(); redraw = 0; }
		VIDEO_WaitVSync();
		FPAD_Update();
		if (delay > ticks_to_millisecs(gettime())) continue;
		if (ne_dir_step(&dir_next, &dx, &dy)) {          /* directions are rate limited: a light hold must not fly through the grid */
			if (dy) cr = (cr + 5 + dy) % 5;
			else cc = (cc + 10 + dx) % 10;
			redraw = 1;
		}
		else if (FPAD_X(0)) { page = (page + 1) % 3; redraw = 1; }
		else if (FPAD_Start(0)) done = 1;
		else if (FPAD_Cancel(0)) { const size_t n = strlen(work); if (n) { work[n - 1] = 0; redraw = 1; } else done = 0; }
		else if (FPAD_OK(0)) {
			const size_t n = strlen(work);
			if (cr < 4) {
				const char ch = ne_kb_page[page][cr][cc];
				if (n < maxlen && n < sizeof work - 1) { work[n] = ch; work[n + 1] = 0; }
				redraw = 1;
			} else {
				switch (cc / 2) {
				case 0: page = (page + 1) % 3; break;
				case 1: if (n < maxlen && n < sizeof work - 1) { work[n] = ' '; work[n + 1] = 0; } break;
				case 2: if (n) work[n - 1] = 0; break;
				case 3: done = 1; break;
				default: done = 0; break;
				}
				redraw = 1;
			}
		}
	}
	if (done == 1 && strcmp(buf, work) != 0) snprintf(buf, maxlen + 1, "%s", work); else done = 0;     /* 1 only when the text really changed */
	msn_wipe(work, (u32)sizeof work);
	ClearScreen();
	return done == 1;
}

#ifdef MS_DEV_UI      /* developer tool: the read-only layout dump (MS_DEV_UI); its ota.log lines are written only when it is opened */
/* ---- the read-only layout screen (X) --------------------------------------------------------------------------------------------------------------------------------- */
static void ne_layout_screen(void)
{
	u8 *live = (u8 *)malloc(NE_LEN), *fil = (u8 *)malloc(NE_LEN);
	char l[8][120];
	int n = 0, i, y, rc3 = -1, rc5 = -1, st3 = 0, st5 = 0, k;
	if (!live || !fil) { if (live) free(live); if (fil) free(fil); return; }
	rc3 = ne_read_once(3, live, &st3);
	rc5 = ne_read_once(5, fil, &st5);
	snprintf(l[n++], sizeof l[0], "Live read: rc %d status %d, selected type %u. Stored read: rc %d status %d, selected type %u.", rc3, st3, rc3 == 0 ? (unsigned)live[MSN_HDR_SEL] : 0u, rc5, st5, rc5 == 0 ? (unsigned)fil[MSN_HDR_SEL] : 0u);
	for (k = 0; k < 2; k++) {
		const u8 *b = k == 0 ? live : fil;
		const int ok = k == 0 ? rc3 == 0 : rc5 == 0;
		for (i = 1; i <= 3; i++) {
			char t[100];
			struct msn_info in;
			t[0] = 0;
			if (ok && msn_describe(b, NE_LEN, (unsigned)i, &in) == MSN_OK) msn_info_line(t, sizeof t, (unsigned)i, &in);
			else snprintf(t, sizeof t, "%d: not readable", i);
			snprintf(l[n], sizeof l[0], "%s %s", k == 0 ? "Live" : "File", t);
			ota_log("netedit: layout %s", l[n]);
			n++;
		}
	}
	ClearScreen();
	ms_title("Wii network settings layout");
	y = MENU_POS_Y + 36;
	y += 16 * wrap_text(MENU_POS_X, y, "Lengths and flags only. No names or passwords are shown. Photograph this screen.", BLACK, 590, 16, 14) + 8;
	for (i = 0; i < n; i++) y += 16 * wrap_text(MENU_POS_X, y, l[i], BLACK, 590, 16, 14) + 6;
	ms_foot(0, "B", "Back", NULL);
	UpdateScreen(); ClearScreen();
	{ u64 t = ticks_to_millisecs(gettime()) + 500; while (ticks_to_millisecs(gettime()) < t) { VIDEO_WaitVSync(); FPAD_Update(); } }
	for (;;) { VIDEO_WaitVSync(); FPAD_Update(); if (FPAD_Cancel(0)) break; }
	msn_wipe(live, NE_LEN); msn_wipe(fil, NE_LEN);
	free(live); free(fil);
	ClearScreen();
}

#endif /* MS_DEV_UI */

/* ---- the Network Profile follows a saved connection --------------------------------------------------------------------------------------------------------
   The loader selects Nintendont's Network Profile (SD:/multishine/nincfg.bin byte 543, low 2 bits; file_net_profile -> conn_apply_profile) at EVERY start, before the network.  Its row was cut from Settings
   in 168, so a connection saved here was overridden at the next start (: connection 2, cable, saved; profile 0 = Auto; every start selected connection 1, Wi-Fi).  Now a save the user
   keeps writes the saved connection's number into that byte; a put-back returns it to the profile of the put-back snapshot.  Only byte 543 changes (msn_nincfg_set_profile, host-tested); the file is
   never created; written as nincfg.upd, read back, swapped (msota_swap_in, the old one kept as nincfg.prev until the swap is done). */
static int ne_profile_file(unsigned prof)
{
	static const struct msota_fs fs = { NULL, fs_exists, fs_unlink, fs_rename, fs_create };
	const char *bin = "sd:/multishine/nincfg.bin", *nw = "sd:/multishine/nincfg.upd", *old = "sd:/multishine/nincfg.prev";
	u8 *b = NULL;
	u32 n = 0;
	int r, sw;
	if (!devices[DEV_SD]) MountDevice(DEV_SD);
	if (read_file(bin, &b, &n, 4096) != 0 || !b) { ota_log("netedit: nincfg.bin not readable: network profile %u not written (the file is never created here)", prof); return -1; }
	r = msn_nincfg_set_profile(b, n, prof);
	if (r < 0) { ota_log("netedit: nincfg.bin is not a settings file (magic or length %lu): network profile not written", (unsigned long)n); free(b); return -1; }
	if (r == 0) { free(b); return 0; }
	unlink_path(nw);
	if (write_file(nw, b, n) != 0 || verify_head(nw, b, n, n) != 0) { unlink_path(nw); free(b); ota_log("netedit: writing nincfg.upd failed: network profile %u not written, nincfg.bin as it was", prof); return -1; }
	unlink_path(old);
	sw = msota_swap_in(&fs, bin, nw, old);
	if (sw != MSOTA_SW_OK) { unlink_path(nw); free(b); ota_log("netedit: the nincfg.bin swap failed (%d): network profile %u not written", sw, prof); return -1; }
	unlink_path(old);
	FlushDevices();
	free(b);
	ota_log("netedit: network profile %u written to nincfg.bin", prof);
	return 1;
}
/* make prof the one used at start (file + the loader's own copy, which the Settings page saves again) */
static void ne_profile_set(unsigned prof, int *changed)
{
	if (prof > 3u || ne_profile_file(prof) < 0) return;
	if (ncfg && (unsigned)(ncfg->NetworkProfile & 3u) != prof) { ncfg->NetworkProfile = (u8)((ncfg->NetworkProfile & 0xFCu) | prof); *changed = 1; }
	if (ne_start_prof != prof) ne_prof_written = 1;
	ne_start_prof = prof;
	ne_start_wired = (prof && prof == ne_prof) ? (ne_row.wired ? 1 : 0) : -1;   /* ne_reload reads it again from the Wii's config */
	ne_pend = prof;
}

/* ---- the Network screen ---------------------------------------------------------------------------------------------------------------------------------------------- */
enum { NR_CONN, NR_TYPE, NR_NAME, NR_SEC, NR_PASS, NR_SAVE, NR_TEST, NR_COUNT };   /* Save and test comes first, then Test again (Test before Save told you to save first) */
static int ne_row_visible(int r)
{
	if (r == NR_TEST) return 0;                       /* 'Test again' is merged into 'Save and test': with nothing to save it tests what the Wii holds now (one action) */
	if (r == NR_CONN) return 1;
	if (!ne_prof) return 0;
	if (r == NR_TYPE || r == NR_SAVE) return 1;
	if (ne_row.wired) return 0;
	if (r == NR_PASS) return ne_row.sec != MSN_SEC_OPEN;
	return 1;
}
static void ne_draw(int cur, const char *msg, u32 mcolor)
{
	static const char *const lab[NR_COUNT] = { "Connection", "Type", "Network name", "Security", "Password", "Save and test", "Test again" };
	const int x0 = MENU_POS_X + 50, x1 = MENU_POS_X + 270;
	int y = MENU_POS_Y + 36, r;
	char v[80];
	ms_title("Network");
	if (ne_rd_rc != 0) PrintFormat(14, MAROON, MENU_POS_X, y, "The Wii network settings could not be read. Check them in Wii Settings > Internet.");
	else if (ne_prof) PrintFormat(14, BLACK, MENU_POS_X, y, "Editing connection %u of the Wii. Save and test makes it the one in use.", ne_prof);
	else PrintFormat(14, BLACK, MENU_POS_X, y, "Choose connection 1, 2 or 3 to edit it.");
	{ char su[64]; msn_start_line(su, sizeof su, ne_start_prof, ne_start_wired); PrintFormat(14, BLACK, MENU_POS_X, y + 18, "%s", su); }   /* what the loader selects at the next start */
	y += 46;
	for (r = 0; r < NR_COUNT; r++) {
		if (!ne_row_visible(r)) continue;
		if (r == cur) ms_sel_pill(x0, y, 540, 22);
		PrintFormat(DEFAULT_SIZE, BLACK, x0, y, "%s", lab[r]);
		v[0] = 0;
		switch (r) {
		case NR_CONN: { const unsigned p = ne_pend; if (p) snprintf(v, sizeof v, "Connection %u", p); else snprintf(v, sizeof v, "Auto (the Wii's own choice)"); break; }
		case NR_TYPE: snprintf(v, sizeof v, "%s", ne_row.wired ? "Cable" : "Wi-Fi"); break;
		case NR_NAME: snprintf(v, sizeof v, "%s", ne_row.ssid[0] ? ne_row.ssid : "(not set)"); break;
		case NR_SEC: snprintf(v, sizeof v, "%s", msn_sec_name(ne_row.sec)); break;
		case NR_PASS: { const size_t n = strlen(ne_row.key); size_t i; if (!n) snprintf(v, sizeof v, "(not set)"); else { (void)i; snprintf(v, sizeof v, "********"); }    /* a fixed count: the row must not show the length */ break; }
		case NR_TEST: snprintf(v, sizeof v, "Try the connection the Wii holds now"); break;
		case NR_SAVE: snprintf(v, sizeof v, "%s", ne_dirty ? (ne_keep ? "Not kept by the Wii yet: write, then test" : "Write to the Wii, then test") : "Test the connection the Wii holds now"); break;
		}
		PrintFormat(DEFAULT_SIZE, BLACK, x1, y, "%s", v);
		y += 26;
	}
	if (msg && msg[0]) wrap_text(MENU_POS_X, y + 6, msg, mcolor, 590, 18, 14);
#ifdef MS_DEV_UI
	ms_foot(0, "B", "Back", "A", "Change", "X", "Layout", "Y", "Wi-Fi soak", NULL);
#else
	ms_foot(0, "B", "Back", "A", "Change", NULL);
#endif
}
/* returns 1 when Nintendont's Network Profile changed (the caller then saves the settings file) */
int ms_net_screen(void)
{
	int cur = NR_CONN, redraw = 1, changed = 0, leave_armed = 0, r, dx, dy;
	char msg[200];
	u32 mcolor = BLACK;
	u64 delay = ticks_to_millisecs(gettime()) + 400, dir_next = 0;
	msg[0] = 0;
	if (!devices[DEV_SD]) MountDevice(DEV_SD);
	ne_pend = ncfg ? (unsigned)(ncfg->NetworkProfile & 3u) : 0u;
	ne_start_prof = file_net_profile(); ne_snap_prof = ne_start_prof; ne_prof_written = 0;     /* the file decides what is used at start */
	ne_snap_take();                                                        /* the settings as they are now: what every put-back of this screen restores */
	ne_reload();
	for (;;) {
		int act = 0;                                                       /* 1 = A / right, -1 = left */
		if (redraw) { ClearScreen(); ne_draw(cur, msg, mcolor); UpdateScreen(); ClearScreen(); redraw = 0; }
		VIDEO_WaitVSync();
		FPAD_Update();
		if (delay > ticks_to_millisecs(gettime())) continue;
		if (ne_dir_step(&dir_next, &dx, &dy)) {
			if (dy < 0) { r = cur; do { r = (r + NR_COUNT - 1) % NR_COUNT; } while (!ne_row_visible(r)); cur = r; redraw = 1; }
			else if (dy > 0) { r = cur; do { r = (r + 1) % NR_COUNT; } while (!ne_row_visible(r)); cur = r; redraw = 1; }
			else act = dx;
		}
		else if (FPAD_OK(0)) act = 1;
#ifdef MS_DEV_UI
		else if (FPAD_X(0)) { ne_layout_screen(); redraw = 1; delay = ticks_to_millisecs(gettime()) + 300; }
		else if (FPAD_Y(0)) { soak_from_settings(); redraw = 1; delay = ticks_to_millisecs(gettime()) + 600; }      /* the long-running Wi-Fi soak (ms_soak.h); hold B 2 s stops it */
#endif
		else if (FPAD_Cancel(0)) {
			if (ne_dirty && !leave_armed) { leave_armed = 1; snprintf(msg, sizeof msg, "You have changes that are not saved. Press B again to leave without saving."); mcolor = MAROON; redraw = 1; continue; }
			break;
		}
		if (!act) continue;
		leave_armed = 0;
		msg[0] = 0; mcolor = BLACK;
		switch (cur) {
		case NR_CONN: {
			ne_pend = (ne_pend + (act > 0 ? 1u : 3u)) & 3u;                  /* only a choice on screen: nothing is written until Save and test */
			ne_reload();
			if (ne_prof && ncfg && ne_pend != (unsigned)(ncfg->NetworkProfile & 3u)) ne_dirty = 1;
			if (cur >= NR_COUNT || !ne_row_visible(cur)) cur = NR_CONN;
			break;
		}
		case NR_TYPE: ne_row.wired = !ne_row.wired; ne_dirty = 1; ne_edit = 1; break;
		case NR_SEC: ne_row.sec = (int)msn_sec_next(ne_row.sec, act); if (ne_row.sec == MSN_SEC_OPEN) ne_row.key[0] = 0; ne_dirty = 1; ne_edit = 1; break;
		case NR_NAME: if (act > 0) { if (ne_keyboard("Network name", ne_row.ssid, 32, 0)) { ne_dirty = 1; ne_edit = 1; } delay = ticks_to_millisecs(gettime()) + 300; } break;
		case NR_PASS: if (act > 0) { if (ne_keyboard("Password", ne_row.key, 63, 1)) { ne_dirty = 1; ne_edit = 1; } delay = ticks_to_millisecs(gettime()) + 300; } break;
		case NR_TEST: {
			int vd = 0, tr;
			char why[80];
			const char *l[2] = { "Testing the connection...", "This can take over a minute. B stops the test." };
			if (ne_edit) { snprintf(msg, sizeof msg, "Use Save and test first: this tests what the Wii holds now."); mcolor = MAROON; break; }
			ne_msg_screen("Network", l, NULL, 2);
			tr = ne_test(&vd, why, sizeof why);
			snprintf(msg, sizeof msg, "%s", ne_verdict_text(tr, vd, why));
			mcolor = tr == 0 ? BLACK : MAROON;
			delay = ticks_to_millisecs(gettime()) + 400;
			break;
		}
		case NR_SAVE: {
			int sr;
			u32 col;
			if (!ne_dirty) {                                                   /* nothing to write: the same row tests what the Wii holds now */
				int vd = 0, tr;
				char why[80];
				const char *l[2] = { "Testing the connection...", "This can take over a minute. B stops the test." };
				ne_msg_screen("Network", l, NULL, 2);
				tr = ne_test(&vd, why, sizeof why);
				snprintf(msg, sizeof msg, "%s", ne_verdict_text(tr, vd, why));
				mcolor = tr == 0 ? BLACK : MAROON;
				if (tr == 0 && ne_prof >= 1 && ne_prof <= 3 && ne_prof != ne_start_prof) { ne_profile_set(ne_prof, &changed); ne_snap_prof = ne_start_prof; }   /* Save on a row that passes its test: used at start from now on */
				delay = ticks_to_millisecs(gettime()) + 400;
				break;
			}
			sr = ne_save(msg, sizeof msg, &col);
			mcolor = col;
			if (sr >= 0 && ne_prof >= 1 && ne_prof <= 3) { ne_profile_set(ne_prof, &changed); if (sr == 0) ne_snap_prof = ne_start_prof; }   /* the saved connection (kept by the user) is the one used at start: nincfg.bin byte 543, never another connection guessed */
			else if (sr == -1 && ne_start_prof != ne_snap_prof) ne_profile_set(ne_snap_prof, &changed);   /* the old settings were put back: the profile goes back with them */
			if (sr >= 0) ne_reload();                                      /* what the Wii holds now; after a put-back or a refusal the typed settings stay for another try */
			if (cur >= NR_COUNT || !ne_row_visible(cur)) cur = NR_CONN;
			delay = ticks_to_millisecs(gettime()) + 400;
			break;
		}
		}
		if (!ne_row_visible(cur)) cur = NR_CONN;
		redraw = 1;
	}
	if (ne_prof_written && ne_start_prof >= 1 && ne_start_prof <= 3) conn_apply_profile(ne_start_prof);   /* this run uses it from now on too */
	ne_clear();
	ne_snap_free();
	ClearScreen();
	return changed;
}
#endif
