/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Wi-Fi SOAK mode of the loader.  Included by ms_ota_loader.c (it uses that file's statics: ota_log, ota_net_up, ota_tls_get, diag_console, ...); the pure part (bursts, statistics, health rules, lines,
 * queue, file names) is ota/ms_soak_core.[ch], host-tested.
 *
 * START: the marker SD:/multishine/soak_on (any content; the words "link" = also read ncd GetLinkStatus once a minute, "noupload" = SD log only), checked when the loader's start screen opens and again
 * at the game start (autoboot).  The loader then stays on its own screen and never starts the game.  Settings > Network > Y starts it too (and creates the marker).
 * STOP: hold B for 2 s (the marker is removed, the loader goes on as normal), or create SD:/multishine/soak_off (checked every 10 s; both markers are removed), or 12 hours (cap; marker removed).
 *       A power loss or a crash leaves the marker: the next start resumes with a new session.
 * WHAT: every 10 s a burst (20 probes 50 ms apart, interleaved) to the router guess (.1 of the Wii's /24), 1.1.1.1 and 8.8.8.8 (UDP DNS "localhost", the connection check's packet); every 60 s a TLS GET of the
 * update 'latest' endpoint (one try, 8 s, timings split into connect / handshake) and a network line; events as they happen; every 5 minutes (first one after 45 s) the lines not yet sent go to the server in one
 * diagnostics body (POST /v1/update/diag/shell, the opt-in channel the Diagnostics screen uses; this IS the opt-in for the soak).  Every line is also appended to SD:/multishine/soak/soak-<session>.csv
 * (opened, written, closed, flushed per line: nothing sits in a buffer; rotated at 2 MB).
 * NEVER in a line: the Wi-Fi name, the password, a MAC, the Wii's address.  The network config is not touched (the profile the boot check already selects is selected again, which writes nothing when set).
 * ROCK SOLID: every wait is bounded (burst 4 s, TLS 8 s / 20 s, network bring-up 30 s); all legs silent for three bursts (or a network status that is not "up") restarts the stack; a loop that has not completed a
 * burst for 60 s (the watchdog) restarts it too; a failed restart is tried again at the next burst. */
#ifndef MS_SOAK_H
#define MS_SOAK_H
#include "ms_soak_core.h"

#define MSOAK_ON  "sd:/multishine/soak_on"
#define MSOAK_OFF "sd:/multishine/soak_off"
#define MSOAK_DIR "sd:/multishine/soak"

static struct sk_queue g_sq;
static struct sk_ctx g_sc;
static struct sk_health g_sh;
static struct sk_agg g_ag_min, g_ag_hr;
static struct sk_stats g_sl[SK_LEGS];
static u32 g_s_opts, g_s_t0, g_s_sd_fail, g_s_ul_ok, g_s_ul_fail, g_s_tls_ok, g_s_tls_fail, g_s_part, g_s_fsize, g_s_serial, g_s_last_tls_ms, g_s_last_ul_ms, g_s_hold_t0, g_s_hold_on;
static int g_s_link_rc, g_s_want_restart;
static u32 g_s_link_word;
static char g_s_msg[80], g_s_last_why[48];
static const char *const g_s_leg_name[SK_LEGS] = { "Router (.1)", "1.1.1.1", "8.8.8.8" };
static char g_s_lines[SK_BODY_MAX];

static u32 soak_up_s(void) { return (u32)(now_ms() - g_s_t0) / 1000u; }
static u32 soak_wall(void) { time_t t = time(NULL); return t > 0 ? (u32)t : 0u; }
static void soak_ctx_stamp(void) { g_sc.up_s = soak_up_s(); g_sc.wall = soak_wall(); }

/* append one line to the session's csv: open for append, write, close, flush (a new part after 2 MB; every new file starts with a comment line naming the columns) */
static void soak_sd_append(const char *line)
{
	static const char hdr[] = "#MSK1 session,kind,seq,up_s,wall,... (ota/ms_soak_core.h): S sample, T tls get, N net state, E event, U upload\n";
	char path[96], buf[SK_LINE_MAX + 2];
	u32 len = (u32)strlen(line) + 1u;
	FIL f;
	UINT put = 0;
	FRESULT r;
	if (!g_s_part) { g_s_part = 1; f_mkdir_char(MSOTA_DIR); f_mkdir_char(MSOAK_DIR); }
	if (sk_need_rotate(g_s_fsize, len)) { g_s_part++; g_s_fsize = 0; }
	if (!sk_part_name(path, sizeof path, g_sc.session, g_s_part)) { g_s_sd_fail++; return; }
	memcpy(buf, line, len - 1u); buf[len - 1u] = '\n';
	r = f_open_char(&f, path, FA_WRITE | FA_OPEN_APPEND);
	if (r != FR_OK) { g_s_sd_fail++; return; }
	if (f_size(&f) == 0) { if (f_write(&f, hdr, sizeof hdr - 1u, &put) == FR_OK) g_s_fsize += (u32)put; }
	r = f_write(&f, buf, len, &put);
	if (r != FR_OK || put != len) g_s_sd_fail++;
	else g_s_fsize += len;
	f_close(&f);                                                   /* the loader's disk layer reports an error here for data that is written (see write_file_once): not counted */
	FlushDevices();
}

/* one record: the csv and the upload queue */
static void soak_put(const char *line, u32 n)
{
	if (!n) return;
	soak_sd_append(line);
	sk_q_push(&g_sq, line);
}
static void soak_event(const char *type, u32 a, u32 b)
{
	char l[SK_LINE_MAX];
	soak_ctx_stamp();
	soak_put(l, sk_fmt_event(l, sizeof l, &g_sc, type, a, b));
}

/* the buttons: hold B 2 s stops.  Returns 1 when the hold is complete (FPAD_Cancel(0) only counts a fresh press, FPAD_Cancel(1) = still held) */
static int soak_b_held(void)                                           /* B held right now.  Before FPAD_Init (the autoboot path) only a GameCube pad can be read: FPAD_Update there crashed the first soak on a Wii (exception screen after the first burst,) */
{
	if (pads_up) { FPAD_Update(); return FPAD_Cancel(1) ? 1 : 0; }
	PAD_ScanPads();
	return (PAD_ButtonsHeld(0) & PAD_BUTTON_B) ? 1 : 0;
}
static int soak_hold_b(void)
{
	const u32 now = now_ms();
	const int b = soak_b_held();
	if (!b) { g_s_hold_on = 0; return 0; }
	if (!g_s_hold_on) { g_s_hold_on = 1; g_s_hold_t0 = now; return 0; }
	if ((u32)(now - g_s_hold_t0) >= 2000u) { g_s_hold_on = 0; return 1; }
	return 0;
}
static int soak_poll_stop(void)
{
	if (pads_up && Shutdown) LoaderShutdown();
	return soak_hold_b();
}

/* ncd GetLinkStatus (ioctlv 7, one 32 byte output; the call libogc's own net_init and the kernel glue make).  Opt-in ("link" in the marker): another open of ncd/manage while the stack runs is the one thing
   here that is not proven on a Wii.  Only the return code and the first word are kept. */
static void soak_link(int *rc, u32 *word)
{
	ioctlv *cv;
	u8 *cb;
	s32 fd, r;
	*rc = 1000; *word = 0;                                         /* 1000 = not read */
	if (!(g_s_opts & SK_O_LINK)) return;
	cv = (ioctlv *)memalign(32, sizeof(ioctlv));
	cb = (u8 *)memalign(32, 0x20);
	if (!cv || !cb) { if (cv) free(cv); if (cb) free(cb); *rc = -2000; return; }
	memset(cb, 0, 0x20);
	cv[0].data = cb; cv[0].len = 0x20;
	DCFlushRange(cb, 0x20); DCFlushRange(cv, sizeof(ioctlv));
	fd = IOS_Open("/dev/net/ncd/manage", 0);
	if (fd < 0) *rc = (int)fd;
	else {
		r = IOS_Ioctlv(fd, 7, 0, 1, cv);
		DCInvalidateRange(cb, 0x20);
		*rc = (int)r; *word = *(u32 *)cb;
		IOS_Close(fd);
	}
	free(cv); free(cb);
}

static int soak_net_ok(void) { return net_started && net_get_status() == 0 && net_gethostip() != 0; }

/* ---- one burst: SK_BURST_N probes to each of the three legs, interleaved, bounded to 4 s ---- */
static void soak_burst(u32 now_ms_in)
{
	struct sk_burst b[SK_LEGS];
	struct sk_stats st[SK_LEGS];
	u8 buf[128];
	struct sockaddr_in sa, from;
	u32 hip = net_gethostip(), ip[SK_LEGS], l, t0, sent_k = 0, tot_s = 0, tot_r = 0, serial;
	s32 s = -1, fcl;
	int ok = soak_net_ok(), act;
	char line[SK_LINE_MAX];
	(void)now_ms_in;
	memset(st, 0, sizeof st);
	if (ok) {
		ip[0] = (hip & 0xFFFFFF00u) | 1u; ip[1] = 0x01010101u; ip[2] = 0x08080808u;
		s = net_socket(PF_INET, SOCK_DGRAM, 0);
		if (s >= 0) { fcl = net_fcntl(s, F_GETFL, 0); if (fcl < 0 || net_fcntl(s, F_SETFL, fcl | 4) < 0) { net_close(s); s = -1; } }
	}
	if (s >= 0) {
		serial = ++g_s_serial;
		for (l = 0; l < SK_LEGS; l++) sk_burst_init(&b[l], ip[l]);
		t0 = conn_us();
		for (;;) {
			u32 now = conn_us(), el = now - t0, k, drained = 0;
			int more = 1;
			if (sent_k < SK_BURST_N && el >= sent_k * SK_BURST_GAP_US) {
				for (l = 0; l < SK_LEGS; l++) {
					u32 idx = sk_burst_send(&b[l], now), n;
					if (idx == 0xFFFFFFFFu) continue;
					n = msp_dns_build(buf, sk_txid(serial, l, idx));
					memset(&sa, 0, sizeof sa);
					sa.sin_family = PF_INET; sa.sin_len = 8; sa.sin_port = htons(53); sa.sin_addr.s_addr = htonl(ip[l]);
					net_sendto(s, buf, (s32)n, 0, (struct sockaddr *)&sa, 8);              /* a failed send stays a lost probe */
				}
				sent_k++;
			}
			for (k = 0; k < 12 && more; k++) {                                              /* what has arrived */
				s32 rr;
				socklen_t fl = 8;
				u32 txid, lg, ix;
				memset(&from, 0, sizeof from); from.sin_len = 8;
				rr = net_recvfrom(s, buf, (s32)sizeof buf, 0, (struct sockaddr *)&from, &fl);
				if (rr <= 0) { more = 0; break; }
				drained++;
				if (ntohs(from.sin_port) != 53 || !msp_dns_parse(buf, (msp_u32)rr, &txid)) continue;
				if (!sk_txid_parse(txid, serial, &lg, &ix) || lg >= SK_LEGS || ntohl(from.sin_addr.s_addr) != ip[lg]) continue;
				sk_burst_reply(&b[lg], ix, conn_us());
			}
			el = conn_us() - t0;
			if (sent_k >= SK_BURST_N) {
				u32 got = 0;
				for (l = 0; l < SK_LEGS; l++) got += b[l].recv;
				if (got >= SK_BURST_N * SK_LEGS || el >= SK_BURST_N * SK_BURST_GAP_US + 1200000u) break;      /* all back, or a 1.2 s drain after the last send */
			}
			if (el > 4000000u) break;
			(void)drained;
			usleep(2 * 1000);
		}
		net_close(s);
		for (l = 0; l < SK_LEGS; l++) { sk_burst_stats(&b[l], &st[l]); tot_s += st[l].sent; tot_r += st[l].recv; }
	}
	memcpy(g_sl, st, sizeof st);
	sk_agg_add(&g_ag_min, st); sk_agg_add(&g_ag_hr, st);
	soak_ctx_stamp();
	soak_put(line, sk_fmt_sample(line, sizeof line, &g_sc, (u32)net_get_status(), &g_sh, g_s_ul_ok, g_s_ul_fail, g_s_sd_fail, st));
	act = sk_health_burst(&g_sh, now_ms(), ok, tot_s, tot_r);
	if (g_sh.ev == SK_EV_DROP) { soak_event("drop", g_sh.ev_a, 0); ota_log("soak: drop (%s)", g_sh.ev_a == 1 ? "network status" : "all legs silent"); }
	else if (g_sh.ev == SK_EV_UP) { soak_event("up", g_sh.ev_a, g_sh.ev_b); ota_log("soak: link back after %lu ms (%lu bad bursts)", (unsigned long)g_sh.ev_a, (unsigned long)g_sh.ev_b); }
	g_s_want_restart = (act == SK_ACT_RESTART);                                      /* the loop restarts the stack (soak_restart, below) */
}

static void soak_draw(void);
static void soak_restart(u32 reason)
{
	u32 t = now_ms();
	int r;
	snprintf(g_s_msg, sizeof g_s_msg, "Restarting the network...");
	soak_draw();
	ota_log("soak: restarting the network (reason %lu: 1 status, 2 silent, 3 watchdog)", (unsigned long)reason);
	ota_net_down();
	usleep(500 * 1000);
	g_conn_t0 = 0; g_net_wait_user = 0;
	r = ota_net_up(30000, 0);
	sk_health_restarted(&g_sh, now_ms());
	soak_event(r == 0 ? "restart" : "restartfail", reason, (u32)(now_ms() - t));
	ota_log("soak: network restart %s after %lu ms (rc %d)", r == 0 ? "ok" : "FAILED", (unsigned long)(now_ms() - t), r);
	g_s_msg[0] = 0;
}

/* ---- the TLS GET (the real server path) and the upload ---- */
static u32 soak_server_ip(int *lab)
{
	u32 ip = 0;
	u16 port;
	char host[64];
	ota_server(&ip, &port, host, sizeof host);
	*lab = ota_url_used;
	if (ota_ip_override) ip = ota_ip_override;
	return ip;
}
static void soak_tls(void)
{
	char path[96], line[SK_LINE_MAX];
	u8 *body = NULL;
	u32 blen = 0, status = 0, t0, ip;
	int lab = 0, r;
	soak_ctx_stamp();
	if (MS_OTA_NTLSPIN <= 0) { snprintf(g_s_last_why, sizeof g_s_last_why, "no pin in this build"); g_s_tls_fail++; soak_put(line, sk_fmt_tls(line, sizeof line, &g_sc, 0, 0, 0, 0, 0, g_s_last_why)); return; }
	if (!soak_net_ok()) { snprintf(g_s_last_why, sizeof g_s_last_why, "no network"); g_s_tls_fail++; soak_put(line, sk_fmt_tls(line, sizeof line, &g_sc, 0, 0, 0, 0, 0, g_s_last_why)); return; }
	ip = soak_server_ip(&lab);
	if (lab || !ip) { snprintf(g_s_last_why, sizeof g_s_last_why, "lab address"); g_s_tls_fail++; soak_put(line, sk_fmt_tls(line, sizeof line, &g_sc, 0, 0, 0, 0, 0, g_s_last_why)); return; }
	snprintf(path, sizeof path, "/v1/update/shl/latest?have=%lu", (unsigned long)shell_have());
	g_tls_conn_ms = 0; g_tls_hs_ms = 0;
	t0 = now_ms();
	r = ota_tls_get(ip, path, 1024, 8000, &body, &blen, &status);
	g_s_last_tls_ms = (u32)(now_ms() - t0);
	if (body) free(body);
	snprintf(g_s_last_why, sizeof g_s_last_why, "%s", r == 0 ? "" : g_tls_why);
	if (r == 0) g_s_tls_ok++; else g_s_tls_fail++;
	soak_ctx_stamp();
	soak_put(line, sk_fmt_tls(line, sizeof line, &g_sc, r == 0, g_s_last_tls_ms, g_tls_conn_ms, g_tls_hs_ms, status, g_s_last_why));
}

static void soak_net_line(void)
{
	char line[SK_LINE_MAX];
	int lrc;
	u32 lw;
	soak_link(&lrc, &lw);
	g_s_link_rc = lrc; g_s_link_word = lw;
	soak_ctx_stamp();
	soak_put(line, sk_fmt_net(line, sizeof line, &g_sc, (u32)net_get_status(), lrc, lw, &g_sh, g_s_tls_ok, g_s_tls_fail, g_s_ul_ok, g_s_ul_fail, g_sq.dropped, g_s_sd_fail));
}

/* one POST: the unsent lines, newest that fit, in the diagnostics body.  Failure keeps everything queued (the next slot carries it with the new lines). */
static void soak_upload(void)
{
	char line[SK_LINE_MAX], why[48], pr[17];
	u8 *rb = NULL;
	u32 n = 0, skipped = 0, bytes, head, ip, t0, status = 0, blen = 0, total;
	int lab = 0, r = -1;
	if (g_s_opts & SK_O_NOUP) return;
	soak_ctx_stamp();
	why[0] = 0;
	if (MS_OTA_NTLSPIN <= 0) snprintf(why, sizeof why, "no pin in this build");
	else if (!soak_net_ok()) snprintf(why, sizeof why, "no network");
	else {
		diag_console();
		ip = soak_server_ip(&lab);
		if (strlen(g_cid) != 16) snprintf(why, sizeof why, "no console id");
		else if (lab || !ip) snprintf(why, sizeof why, "lab address");
		else {
			bytes = sk_q_build(&g_sq, g_s_lines, SK_BODY_MAX - 200u, &n, &skipped);
			head = sk_body_head(g_diag_buf, sizeof g_diag_buf, g_cid, g_sc.session, (u32)MS_SHELL_SEQ, n, skipped, g_sc.up_s);
			if (!head || head + bytes + 40u > sizeof g_diag_buf) snprintf(why, sizeof why, "body too big");
			else {
				u32 tot;
				memcpy(g_diag_buf + head, g_s_lines, bytes);
				tot = head + bytes;
				msd_proof(pr, g_cid, g_diag_buf, tot);
				tot += (u32)snprintf(g_diag_buf + tot, sizeof g_diag_buf - tot, "proof %s\n", pr);
				g_tls_conn_ms = 0; g_tls_hs_ms = 0;
				g_post_body = (const u8 *)g_diag_buf; g_post_len = tot;
				t0 = now_ms();
				r = ota_tls_get(ip, "/v1/update/diag/shell", 1024, 20000, &rb, &blen, &status);
				g_post_body = NULL;
				g_s_last_ul_ms = (u32)(now_ms() - t0);
				if (rb) free(rb);
				if (r != 0) snprintf(why, sizeof why, "%s", g_tls_why);
				total = tot;
				soak_ctx_stamp();
				if (r == 0) { sk_q_done(&g_sq, 1, skipped); g_s_ul_ok++; } else g_s_ul_fail++;
				soak_put(line, sk_fmt_upload(line, sizeof line, &g_sc, r == 0, g_s_last_ul_ms, g_tls_conn_ms, g_tls_hs_ms, status, total, n, skipped, why));
				return;
			}
		}
	}
	g_s_ul_fail++;                                                  /* it never started: no network, no id, ... */
	soak_ctx_stamp();
	soak_put(line, sk_fmt_upload(line, sizeof line, &g_sc, 0, 0, 0, 0, 0, 0, 0, 0, why));
}

/* ---- the screen: calm, text only, redrawn once a second ---- */
static void soak_loss(char *o, size_t n, const struct sk_agg *a, u32 l)
{
	u32 x = sk_agg_loss_x100(a, l);
	snprintf(o, n, "%lu.%02lu%% p50 %lu p95 %lu", (unsigned long)(x / 100u), (unsigned long)(x % 100u), (unsigned long)(sk_agg_p50(a, l) / 1000u), (unsigned long)(sk_agg_p95(a, l) / 1000u));
}
static void soak_draw(void)
{
	char a[64], b[64], c[64];
	int y = MENU_POS_Y + 34, l;
	const u32 up = soak_up_s();
	ClearScreen();
	ms_title("Wi-Fi soak");
	PrintFormat(14, BLACK, MENU_POS_X, y, "Session %s   running %luh %02lum of %luh   %s", g_sc.session, (unsigned long)(up / 3600u), (unsigned long)((up % 3600u) / 60u), (unsigned long)(SK_CAP_S / 3600u), (g_s_opts & SK_O_NOUP) ? "SD log only" : "uploading to the server");
	y += 22;
	if (g_s_msg[0]) PrintFormat(14, BLACK, MENU_POS_X, y, "%s", g_s_msg);
	else PrintFormat(14, BLACK, MENU_POS_X, y, "Network %s   drops %lu   restarts %lu   longest drop %lu s   watchdog %lu", soak_net_ok() ? "up" : "DOWN", (unsigned long)g_sh.drops, (unsigned long)g_sh.restarts, (unsigned long)((g_sh.down_max_ms + 500u) / 1000u), (unsigned long)g_sh.wd);
	y += 28;
	{
		static const int cx[6] = { 0, 150, 230, 320, 410, 500 };
		static const char *const hd[6] = { "Last 10 s", "lost", "p50 ms", "p95 ms", "max ms", "gap ms" };
		int k;
		for (k = 0; k < 6; k++) PrintFormat(14, BLACK, MENU_POS_X + cx[k], y, "%s", hd[k]);
		y += 20;
		for (l = 0; l < (int)SK_LEGS; l++) {
			const struct sk_stats *s = &g_sl[l];
			PrintFormat(14, BLACK, MENU_POS_X + cx[0], y, "%s", g_s_leg_name[l]);
			PrintFormat(14, BLACK, MENU_POS_X + cx[1], y, "%lu/%lu", (unsigned long)(s->sent - s->recv), (unsigned long)s->sent);
			PrintFormat(14, BLACK, MENU_POS_X + cx[2], y, "%lu", (unsigned long)(s->p50 / 1000u));
			PrintFormat(14, BLACK, MENU_POS_X + cx[3], y, "%lu", (unsigned long)(s->p95 / 1000u));
			PrintFormat(14, BLACK, MENU_POS_X + cx[4], y, "%lu", (unsigned long)(s->max / 1000u));
			PrintFormat(14, BLACK, MENU_POS_X + cx[5], y, "%lu", (unsigned long)s->gap_ms);
			y += 20;
		}
	}
	y += 8;
	for (l = 0; l < (int)SK_LEGS; l++) {
		soak_loss(a, sizeof a, &g_ag_min, (u32)l); soak_loss(b, sizeof b, &g_ag_hr, (u32)l);
		snprintf(c, sizeof c, "%s", g_s_leg_name[l]);
		PrintFormat(14, BLACK, MENU_POS_X, y, "%s: minute %s   hour %s", c, a, b);
		y += 20;
	}
	y += 8;
	PrintFormat(14, BLACK, MENU_POS_X, y, "TLS ok %lu failed %lu (last %lu ms)   upload ok %lu failed %lu (last %lu ms)", (unsigned long)g_s_tls_ok, (unsigned long)g_s_tls_fail, (unsigned long)g_s_last_tls_ms, (unsigned long)g_s_ul_ok, (unsigned long)g_s_ul_fail, (unsigned long)g_s_last_ul_ms);
	y += 20;
	PrintFormat(14, BLACK, MENU_POS_X, y, "SD write failures %lu   unsent lines %lu   dropped from the queue %lu", (unsigned long)g_s_sd_fail, (unsigned long)g_sq.unsent, (unsigned long)g_sq.dropped);
	if (g_s_last_why[0]) { y += 20; PrintFormat(14, BLACK, MENU_POS_X, y, "last failure: %.60s", g_s_last_why); }
	ms_foot(0, "Hold B", "Stop", NULL);
	UpdateScreen();
	ClearScreen();
}

/* the whole mode; returns when it is over (B held, soak_off, the cap).  `where` goes to the log. */
static void soak_run(const char *where)
{
	u32 next_burst, next_min, next_up, last_draw = 0, up_cnt = 0;
	int stop = 0;
	char line[SK_LINE_MAX];
	u32 sz = 0;
	u8 *mk = NULL;
	memset(&g_sc, 0, sizeof g_sc);
	g_s_opts = 0;
	if (read_file(MSOAK_ON, &mk, &sz, 256) == 0 && mk) { g_s_opts = sk_parse_opts((const char *)mk, sz); free(mk); }
	sk_session_id(g_sc.session, (u32)gettime(), soak_wall());
	g_s_t0 = now_ms();
	sk_q_init(&g_sq); sk_health_init(&g_sh, now_ms()); sk_agg_reset(&g_ag_min); sk_agg_reset(&g_ag_hr);
	memset(g_sl, 0, sizeof g_sl);
	g_s_sd_fail = g_s_ul_ok = g_s_ul_fail = g_s_tls_ok = g_s_tls_fail = g_s_part = g_s_fsize = g_s_serial = 0;
	g_s_last_tls_ms = g_s_last_ul_ms = 0; g_s_hold_on = 0; g_s_msg[0] = 0; g_s_last_why[0] = 0; g_s_want_restart = 0;
	ota_log("soak: start (%s) session %s options %lu shell %lu", where, g_sc.session, (unsigned long)g_s_opts, (unsigned long)MS_SHELL_SEQ);
	soak_event("start", (u32)MS_SHELL_SEQ, g_s_opts);
	snprintf(g_s_msg, sizeof g_s_msg, "Starting the network...");
	soak_draw();
	conn_apply_profile(file_net_profile());
	g_conn_t0 = 0; g_net_wait_user = 0;
	(void)ota_net_up(30000, 0);                                      /* a failure is no reason to stop: the loop below restarts the stack */
	sk_health_restarted(&g_sh, now_ms());
	g_s_msg[0] = 0;
	next_burst = now_ms(); next_min = now_ms() + SK_MINUTE_MS; next_up = now_ms() + SK_UPLOAD_FIRST_MS;
	while (!stop) {
		const u32 now = now_ms();
		if (sk_health_watchdog(&g_sh, now)) { soak_event("wd", 0, 0); soak_restart(3); }
		if ((u32)(now - next_burst) < 0x80000000u) {                 /* due */
			soak_burst(now);
			if (g_s_want_restart) { g_s_want_restart = 0; soak_restart(soak_net_ok() ? 2u : 1u); }
			next_burst += SK_PERIOD_MS;
			if ((u32)(now_ms() - next_burst) < 0x80000000u && (u32)(now_ms() - next_burst) > SK_PERIOD_MS) next_burst = now_ms() + SK_PERIOD_MS;   /* far behind (a restart, a long TLS): no catching up */
			if ((u32)(now_ms() - next_min) < 0x80000000u) {
				next_min += SK_MINUTE_MS;
				if ((u32)(now_ms() - next_min) < 0x80000000u) next_min = now_ms() + SK_MINUTE_MS;
				soak_net_line(); soak_tls();
				sk_agg_reset(&g_ag_min);
				if (!(++up_cnt % 60u)) sk_agg_reset(&g_ag_hr);
			}
			if ((u32)(now_ms() - next_up) < 0x80000000u) {
				next_up = now_ms() + SK_UPLOAD_MS;
				soak_upload();
			}
			if (file_exists(MSOAK_OFF)) { ota_log("soak: soak_off found"); stop = 1; }
			if (soak_up_s() >= SK_CAP_S) { ota_log("soak: 12 h cap reached"); stop = 1; }
			if (soak_poll_stop()) stop = 1;
			last_draw = 0;
		}
		if ((u32)(now_ms() - last_draw) >= 1000u) { soak_draw(); last_draw = now_ms(); }
		VIDEO_WaitVSync();
		if (soak_poll_stop()) stop = 1;
	}
	soak_event("stop", soak_up_s(), 0);
	if (soak_net_ok()) { snprintf(g_s_msg, sizeof g_s_msg, "Sending the last lines..."); soak_draw(); soak_upload(); g_s_msg[0] = 0; }
	(void)line;
	ota_net_down();                                                  /* the normal flow starts its own network */
	unlink_path(MSOAK_ON); unlink_path(MSOAK_OFF);
	ota_log("soak: stopped after %lu s, %lu drop(s), %lu restart(s), uploads %lu ok %lu failed, sd failures %lu", (unsigned long)soak_up_s(), (unsigned long)g_sh.drops, (unsigned long)g_sh.restarts, (unsigned long)g_s_ul_ok, (unsigned long)g_s_ul_fail, (unsigned long)g_s_sd_fail);
	ClearScreen();
}

/* the hooks: the start screen opens, the game starts (autoboot).  soak_off alone (no soak_on) is removed without running. */
static void soak_maybe_run(const char *where)
{
	if (!devices[DEV_SD]) return;
	if (file_exists(MSOAK_OFF)) { unlink_path(MSOAK_OFF); unlink_path(MSOAK_ON); ota_log("soak: soak_off found at %s: not started", where); return; }
	if (!file_exists(MSOAK_ON)) return;
	if (!pads_up) { ota_log("soak: pads not up at %s: deferred to the start screen", where); return; }   /* the first Wii soak crashed in this early phase; the start screen hook runs it with the pads up */
	ota_log("soak: starting at %s (pads_up %d)", where, pads_up);
	soak_run(where);
}
/* Settings > Network > Y */
static void soak_from_settings(void)
{
	if (!devices[DEV_SD]) MountDevice(DEV_SD);
	if (!devices[DEV_SD]) return;
	if (!file_exists(MSOAK_ON)) { f_mkdir_char(MSOTA_DIR); write_file(MSOAK_ON, (const u8 *)"\n", 1); }
	unlink_path(MSOAK_OFF);
	soak_run("settings");
}
#endif
