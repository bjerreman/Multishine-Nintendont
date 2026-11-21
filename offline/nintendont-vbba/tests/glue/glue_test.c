/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/*
 * glue_test: the glue file of the Nintendont patch (kernel/vbba_glue.c, compiled UNMODIFIED against tests/glue/kmock.c)
 * with its real threads (fast EXI thread + IOS completion thread), real vbba core, and OUR unchanged driver
 * (offline/bba) and sdnet (offline/net) running on a "PowerPC" thread that talks to the ARM side only through the two
 * EXI_CMD words, exactly like Nintendont's EXISelect/EXIImm/EXIDMA stubs (spin until the ARM clears EXI_CMD_0).
 * The EXI dispatcher below (EXIServiceStep) mirrors the hunks the patch adds to kernel/EXI.c; the real EXI.c is not
 * compiled here.  What this proves: thread/ring/slot logic, the fast-thread protocol, the memory-card hold, startup
 * chaining, shutdown, and that the glue's IOS encodings match the layouts kmock decodes (our reading of kernel/sock.c).
 * What it does not prove: anything about real IOS, real timing, real cache behaviour, EXI.c itself.
 */
#include "kshim.h"
#include "kmock.h"
#include "../../../bba/bba_mx98728.h"
#include "../../../net/sdnet.h"
#include <pthread.h>
#ifdef VBBA_TP_IOS_TX_INFLIGHT
#define EXP_INFLIGHT VBBA_TP_IOS_TX_INFLIGHT          /* the b15a-style build (one sendto in IOS) is tested with the same file */
#else
#define EXP_INFLIGHT 16
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* glue API (kernel/vbba_glue.h) */
extern vu32 vbbaWanted, vbbaActive, vbbaTakeover;
u32 VBBAMainDelay(u32 us); void VBBAConfigLoad(void); void VBBAConfigLog(void);
void VBBAUpdate(void); void VBBAShutdown(void); void VBBAHoldBegin(void); void VBBAHoldEnd(void);
void VBBAExiSelect(u32 chn, u32 dev); u32 VBBAExiImm(u32 data, u32 len, u32 mode); void VBBAExiDma(u8 *ptr, u32 len, u32 mode);
bool EXI_IRQ = 0;
u32 SI_IRQ = 0;                  /* kernel/SI.c (b22a engine lines) */
volatile u32 eng_fake[0x27000];  /* b22a/b22b: PowerPC MEM1 stand-in 0x80460000..0x804FC000, see kshim.h */
/* b22b: kernel/GCNCard.c accessor stand-in: slot A enabled, unsaved, system dirty, unsaved bytes, writes, saves, HW_TIMER of last write / save */
u32 kmock_card[8] = { 1, 1, 0, 0x2000, 7, 3, 0, 0 };
u32 GCNCard_VbbaState(u32 *o) { int k; for (k = 0; k < 8; k++) o[k] = kmock_card[k]; return o[0]; }
/* kernel/SDI.c counters of the patch (0004 / 0005 / 0020): sd commands, start tick of the one in flight, end tick and longest duration of the last; the host has no SD */
vu32 vbbaSdOps, vbbaSdBusy, vbbaSdLastEnd, vbbaSdMaxTicks;
/* kernel/GCNCard.c read-only view for the DIAG lines (0005): size, changed flags, current / low / high modified offset, write count; the host has no card context behind it */
void GCNCard_DiagState(u32 *o) { int k; for (k = 0; k < 6; k++) o[k] = 0; }
bool DI_IRQ = 0;                 /* kernel/DI.c: no disc read in flight (the periodic timeline lines are written only then) */

/* status block (VBBA_STATUS_ADDR override from the build line) */
volatile u32 status_blk[256] __attribute__((aligned(32)));      /* the working block (words 0..255) */
volatile u32 pub_blk[128] __attribute__((aligned(32)));         /* the published 512-byte block at 0x131BE000 */
unsigned char heap_blk[0x1E000] __attribute__((aligned(32)));

#if defined(VBBA_IOSBUF) && VBBA_IOSBUF
static uintptr_t so_lo;
#endif
static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define IP4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define WII_IP IP4(192, 168, 1, 50)
#define ECHO_LAN IP4(192, 168, 1, 20)

/* ---- the kernel's EXI dispatcher as patched (EXI.c: select / imm / DMA for channel 0 device 2) ---- */
static u8 dev_sel[3];
static volatile void *dma_ptr;          /* the host cannot put a 64-bit pointer into the 32-bit EXI_CMD_1 */
static unsigned memcard_cmds;
/* the patch's single EXI lock (EXI.c swap on exiHolder; whoever holds it serves the EXI commands) */
static volatile u32 exiHolder;
int EXILockTry(void) { return __sync_lock_test_and_set(&exiHolder, 1u) == 0; }
void EXIUnlock(void) { __sync_lock_release(&exiHolder); }
/* the patch's diagnostics counters (0005): commands served, last command tick; no event ring here */
u32 exiDiagCmds, exiDiagLastTick, exiDiagBlocked, exiDiagSlowTicks, exiDiagSlowW0;
u32 EXIDiagCount(void) { return 0; }
void EXIDiagEvent(u32 back, u32 now, char *out) { (void)back; (void)now; out[0] = 0; }
void EXIDiagState(u32 *o) { int k; for (k = 0; k < 10; k++) o[k] = 0; o[6] = exiDiagCmds; o[7] = exiDiagBlocked; }
static void exi_serve(void);
int EXIServiceStep(void)
{
	if (!EXILockTry()) { exiDiagBlocked++; return 0; }
	exi_serve();
	EXIUnlock();
	return 1;
}
static void exi_serve(void)
{
	u32 cmd = read32(EXI_CMD_0), chn, dev, len, mode, data;
	if (!(cmd & 0xFF000000)) return;
	exiDiagCmds++; exiDiagLastTick = read32(HW_TIMER);
	switch (cmd >> 24) {
	case 0x10:
		chn = cmd & 0xFF; dev = (cmd >> 8) & 0xFF;
		if (chn <= 2 && dev <= 2) { dev_sel[chn] = (u8)dev; }
		write32(EXI_CMD_1, (chn == 0 && dev == 2) || dev <= 1 ? 1u : 0u);
		if (chn == 0 && dev == 2 && vbbaActive) VBBAExiSelect(chn, dev);
		write32(EXI_CMD_0, 0);
		break;
	case 0x11:
		chn = (cmd >> 20) & 0xF; data = read32(EXI_CMD_1); len = cmd & 0xFFFF; mode = (cmd >> 16) & 0xF;
		if (chn == 0 && dev_sel[0] == 2 && vbbaActive) { u32 v = VBBAExiImm(data, len, mode); write32(EXI_CMD_1, v); write32(EXI_CMD_0, 0); }
		else { memcard_cmds++; write32(EXI_CMD_1, 0); write32(EXI_CMD_0, 0); }
		break;
	case 0x12:
		chn = (cmd >> 20) & 0xF; len = cmd & 0xFFFF; mode = (cmd >> 16) & 0xF;
		if (chn == 0 && dev_sel[0] == 2 && vbbaActive) { VBBAExiDma((u8 *)dma_ptr, len, mode); write32(EXI_CMD_0, 0); }
		else { memcard_cmds++; write32(EXI_CMD_0, 0); }
		break;
	default: write32(EXI_CMD_0, 0);
	}
}

/* ---- the PowerPC side: Nintendont's stubs (EXIImm.S with the 3-byte fix of the patch) ---- */
static volatile unsigned trips;
static u32 ppc_cmd(u32 cmd, u32 arg, int has_arg)
{
	if (has_arg) write32(EXI_CMD_1, arg);
	write32(EXI_CMD_0, cmd);
	while (read32(EXI_CMD_0) == cmd) { }
	trips++;
	return read32(EXI_CMD_1);
}
static int x_select(void *c) { (void)c; return ppc_cmd(0x10000000u | (5u << 16) | (2u << 8) | 0u, 0, 0) ? 0 : -1; }
static void x_deselect(void *c) { (void)c; }                 /* li r3,1; blr */
static void x_imm_write(void *c, sdnet_u32 d, int len) { (void)c; (void)ppc_cmd(0x11000000u | (1u << 16) | (u32)len, d, 1); }
static sdnet_u32 x_imm_read(void *c, int len)
{
	u32 v = ppc_cmd(0x11000000u | (0u << 16) | (u32)len, 0, 0);
	(void)c;
	switch (len) {                                          /* stb / sth / (patched) 3 x stb / stw, right-aligned */
	case 1: return (v & 0xFFu) << 24;
	case 2: return (v & 0xFFFFu) << 16;
	case 3: return (v & 0xFFFFFFu) << 8;
	default: return v;
	}
}
static void x_dma_write(void *c, const void *b, sdnet_u32 n) { (void)c; dma_ptr = (void *)b; (void)ppc_cmd(0x12000000u | (1u << 16) | n, 0, 0); }
static void x_dma_read(void *c, void *b, sdnet_u32 n) { (void)c; dma_ptr = b; (void)ppc_cmd(0x12000000u | (0u << 16) | n, 0, 0); }
static void x_delay(void *c, sdnet_u32 us) { (void)c; udelay((int)us); }
static const struct bba_exi_ops OPS = { x_select, x_deselect, x_imm_write, x_imm_read, x_dma_write, x_dma_read, 0, 0, x_delay };

static sdnet_u32 clk(void *c) { (void)c; return (sdnet_u32)(read32(HW_TIMER) / 1898u); }

/* ---- guest scenario ---- */
static struct bba B;
static struct sdnet S;
static unsigned log_base;
static volatile int phase, stop_guest, g_bba_rc, g_dhcp_ms, g_echo_ok, g_polls, g_link_rc, g_mac_ok, g_link_ms;
static int now_ms_guest(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static const int sizes[] = { 1, 18, 31, 32, 33, 64, 100, 255, 256, 257, 400, 500 };

static void run_ms(int ms) { int i; for (i = 0; i < ms && !stop_guest; i++) { udelay(1000); sdnet_poll(&S); g_polls++; } }

static void *guest(void *p)
{
	struct sdnet_nic nic;
	u8 pay[600], rx[700];
	int i, t, sock;
	(void)p;
	memset(&B, 0, sizeof B); B.auth_strict = 1;
	g_bba_rc = bba_init(&B, &OPS, 0, 0);
	if (g_bba_rc != BBA_OK) { phase = -1; return 0; }
	g_mac_ok = B.mac[0] == 0x00 && B.mac[1] == 0x09 && B.mac[2] == 0xBF;
	{ int t0 = now_ms_guest(); g_link_rc = bba_link_wait(&B, 10000); g_link_ms = now_ms_guest() - t0; }   /* the chip shows no link until IOS has an address (router link_up = lease known) */
	bba_make_nic(&B, &nic);
	sdnet_init(&S, &nic, clk, 0);
	sdnet_dhcp_start(&S);
	phase = 1;
	for (t = 0; t < 8000 && !stop_guest; t += 5) { run_ms(5); if (sdnet_dhcp_state(&S) == SDNET_DHCP_BOUND) break; }
	g_dhcp_ms = t;
	if (sdnet_dhcp_state(&S) != SDNET_DHCP_BOUND) { phase = -2; return 0; }
	phase = 2;
	for (i = 0; i < 600; i++) pay[i] = (u8)(i * 7 + 1);
	sock = sdnet_bind(&S, 7777, 0, 0);
	(void)sdnet_sendto(&S, sock, ECHO_LAN, 7, pay, 20);       /* ARP first (proxy ARP by the router) */
	run_ms(20);
	for (i = 0; i < 12 && !stop_guest; i++) {
		int got = 0, k;
		sdnet_u32 sip; sdnet_u16 sp;
		int n = sdnet_sendto(&S, sock, ECHO_LAN, 7, pay, (sdnet_u16)sizes[i]);
		if (n != sizes[i]) continue;
		for (k = 0; k < 300 && !got && !stop_guest; k++) {
			run_ms(1);
			while (sdnet_pending(&S, sock)) {
				n = sdnet_recvfrom(&S, sock, rx, sizeof rx, &sip, &sp);
				if (n == sizes[i] && !memcmp(rx, pay, (size_t)n) && sip == ECHO_LAN && sp == 7) got = 1;
			}
		}
		g_echo_ok += got;
	}
	phase = 3;
	while (!stop_guest) run_ms(1);                           /* idle: one RX poll per ms like the game's slice */
	return 0;
}

static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, 0); }
static int wait_phase(int want, int ms) { int t; for (t = 0; t < ms && phase != want && phase >= 0; t += 5) sleep_ms(5); return phase == want || phase > want; }

/* the Nintendont main loop stand-in: VBBAUpdate every ~0.5 ms (watchdog, handle opens), then one EXI service step (main.c of the patch) */
static volatile int stop_main;
static volatile unsigned main_passes;
static u32 main_gap_ms_max;
static void *mainloop(void *p)
{
	u32 last = 0;
	(void)p;
	while (!stop_main) {
		u32 t = read32(HW_TIMER) / 1898u;
		if (last && t - last > main_gap_ms_max) main_gap_ms_max = t - last;
		last = t;
		VBBAUpdate();
		(void)EXIServiceStep();                      /* the patched main.c: the main loop always serves EXI, serialised with the fast thread by the EXI lock (the only context in VBBA_SINGLE mode) */
		main_passes++;
		udelay((int)VBBAMainDelay(500));
	}
	return 0;
}

extern int kmock_open_kd, kmock_open_ip, kmock_kd_rc;
extern void kmock_stock_handles(int on);

int main(void)
{
	pthread_t gt, mt;
	int hang = getenv("GLUE_HANG") != 0, nosleep = getenv("GLUE_NOSLEEP") != 0, stall = getenv("GLUE_STALL") != 0;
	int bk = getenv("GLUE_BAKED") ? (strcmp(getenv("GLUE_BAKED"), "nomagic") ? atoi(getenv("GLUE_BAKED")) : -2) : -1;
	int kdslow = getenv("GLUE_KDSLOW") != 0, slowtx = getenv("GLUE_SLOWTX") != 0, latetx = getenv("GLUE_LATETX") != 0;
	u32 cmds0, cmds1, polls0, polls1;
	int i;
	km.verbose = getenv("VERBOSE") != 0;
#if defined(VBBA_ENGSTAT) && VBBA_ENGSTAT
	{ u32 w; eng_fake[(0x804F7880u - ENG_FAKE_BASE) / 4] = 0x53335354u; eng_fake[(0x804F7880u - ENG_FAKE_BASE) / 4 + 1] = 5; eng_fake[(0x804F7880u - ENG_FAKE_BASE) / 4 + 3] = 4242; eng_fake[(0x804F7880u - ENG_FAKE_BASE) / 4 + 5] = 4200;
	  w = (0x804F7880u + 0x740u - ENG_FAKE_BASE) / 4; eng_fake[w] = 0x53375354u; eng_fake[w + 3] = 77; }       /* 'S3ST' state 5 calls 4242 b0_calls 4200; 'S7ST' ping 77 */
#endif
#if defined(VBBA_SCENESTAT) && VBBA_SCENESTAT
	/* b22b: game words in the fake PowerPC memory: scene [major 2, pending 2, prev 0, minor 2], match frame 1234, scene frames 567, game-end id 0, controller block word AABBCCDD */
	eng_fake[(0x80479D30u - ENG_FAKE_BASE) / 4] = 0x02020002u; eng_fake[(0x8046B6C4u - ENG_FAKE_BASE) / 4] = 1234; eng_fake[(0x80479D58u - ENG_FAKE_BASE) / 4] = 567;
	eng_fake[(0x8046B6A8u - ENG_FAKE_BASE) / 4] = 0; eng_fake[(0x8046B6A0u - ENG_FAKE_BASE) / 4] = 0xAABBCCDDu;
#endif
	kmock_cfg_text = getenv("GLUE_CFG");                     /* the mock SD card's /vbba.cfg (unset: no file) */
	kmock_write_fails = getenv("GLUE_KWFAIL") != 0;
	if (getenv("GLUE_AUTO")) kmock_auto_info = (u32)strtoul(getenv("GLUE_AUTO"), 0, 16);       /* the auto app's sweep word: flags << 16 | position << 8 | 6 */
	{ const char *b = getenv("GLUE_BAKED"); kmock_baked = !b ? -1 : !strcmp(b, "nomagic") ? -2 : atoi(b); }   /* the preset the loader left for the kernel: unset = none baked, nomagic = an old loader */
	km.kd_out = getenv("GLUE_KDOUT") ? atoi(getenv("GLUE_KDOUT")) : 0; km.ncd_minus6 = getenv("GLUE_NCD6") ? 3 : 0; km.hostid_only = getenv("GLUE_HOSTID") != 0;
	km.ip = WII_IP; km.ip_after_ms = 300; km.echo_ip = ECHO_LAN; km.latency_ms = 1;
	printf("== vbba_glue host test (kernel/vbba_glue.c unmodified, fast EXI thread, IOS completion thread, real core + driver)\n");

	/* IOS handles: GLUE_OWN=1 = SOCKInit failed (no stock handles, own open with a retry after a -10); default = stock handles valid
	   and kd/request refusing a second open with -10, as IOS did on the first Wii run: the glue must not open it again */
#if defined(VBBA_IOSBUF) && VBBA_IOSBUF
	{ static u8 so_blk[0x8000] __attribute__((aligned(32))); extern int soHeap; soHeap = heap_create(so_blk, sizeof so_blk); so_lo = (uintptr_t)so_blk; }
#endif
	if (getenv("GLUE_OWN")) { kmock_stock_handles(0); kmock_kd_rc = 0; }
	else { kmock_stock_handles(1); kmock_kd_rc = -10; }

	VBBAConfigLoad();                                        /* kernel start: /vbba.cfg is read once, before any thread */
	VBBAConfigLog();                                         /* the first lines of the run block in /ndebug.log */
	if (getenv("GLUE_AUTO")) { CHECK(strstr(kmock_log, "VBBA: auto sweep: position") != NULL); }
	CHECK(strstr(kmock_log, "VBBA: duty ") != NULL);
	/* A: the main loop starts the thread once vbbaWanted is set; before that nothing runs */
	VBBAUpdate();
	CHECK(vbbaActive == 0);
	vbbaWanted = 1;
	VBBAUpdate();
	CHECK(vbbaActive == 1);
	if (hang) { km.hang_kd = 1; km.kd_late_ms = 2500; }        /* the first kd start-up call (variant 1) returns only after the 1.5 s timeout (test build): variant 0 must run next */
	if (kdslow) { km.hang_kd = 1; km.kd_late_ms = 3300; km.kd_serial = 1; }   /* the Wii of runs 6/7 scaled down: the first kd call takes longer than the timeout, the next one queues behind it and returns right after it */
	if (latetx) { km.late_first_n = 3; km.late_ms = 900; }     /* the first three sendto requests stall for 0.9 s (the test's soft limit is 0.6 s): counted as late, kept, never abandoned, the network stays up */
	if (slowtx) km.sendto_extra_ms = 30;                     /* sendto needs ~30 ms: latency counters and histogram bucket (10..50 ms) */
	if (nosleep) km.udelay_noop = 1;                         /* udelay returns at once on the fast thread: the watchdog must lower its priority */
	pthread_create(&mt, 0, mainloop, 0);
	if (getenv("GLUE_OWN")) CHECK(kmock_open_kd >= 1); else CHECK(kmock_open_kd == 0 && kmock_open_ip == 0);
	CHECK(status_blk[0] == 0x56424241u && status_blk[1] == 3);
	CHECK(kmock_q_entries > 0 && kmock_q_bytes >= 4 * kmock_q_entries);
	CHECK(kmock_queue_canary_ok());
	CHECK(kmock_stack_args_ok());                            /* thread_create gets the stack top: base + size (Wii freeze root cause) */
	{   /* the duty knobs in force (status words 129..134, 172) and the priority the fast thread was created with: the preset baked into the app by the loader is
		   the starting value, /vbba.cfg (GLUE_CFG, the mock SD card) overrides it */
		const char *c = getenv("GLUE_CFG");
		u32 st = 0, duty = bk >= 0 ? (u32)bk : 0, spin, idle, prio, adapt, mainus;
		if (bk >= 0) st |= 4;
		if ((kmock_auto_info & 0x20000) && (kmock_auto_info & 0xFF) && !getenv("GLUE_CFG")) { if (kmock_write_fails) duty = 0; else kmock_auto_info = (kmock_auto_info & ~0x20000u) | 0x80000u; }
		if (c) {
			st |= 1;
			if (!strncmp(c, "duty=", 5)) duty = (u32)atoi(c + 5);
			if (duty > 5) { duty = 0; st |= 2; }
		}
		spin = duty == 1 || duty == 4 || duty == 5 ? 50 : 200; idle = duty == 2 ? 1000 : 50; prio = duty == 3 ? 0x30 : 0x70; adapt = duty == 4 || duty == 5; mainus = duty == 5 ? 1000 : 0;
		if (c && !strcmp(c, "# test\nspin=0 idle=200,prio=20 main=300\n")) { spin = 0; idle = 200; prio = 0x20; mainus = 300; }
		if (c && !strcmp(c, "idle=300")) idle = 300;
		CHECK(status_blk[128] == 1 && status_blk[129] == st && status_blk[130] == duty && (int)status_blk[131] >= (int)spin - 5 && (int)status_blk[131] <= (int)spin + 1 && status_blk[132] == idle && status_blk[133] == prio && status_blk[134] == adapt && status_blk[172] == mainus);
		CHECK(kmock_fast_prio == (int)prio);
		CHECK(status_blk[173] == kmock_auto_info);                                       /* the sweep position and flags are in the status block (the probe page and report show them) */
		printf("   baked %d, /vbba.cfg %s: state %u duty %u spin %u us idle %u us priority %02x adaptive %u main %u (thread created with %02x)\n", bk, c ? c : "(none)", status_blk[129], status_blk[130], status_blk[131], status_blk[132], status_blk[133], status_blk[134], status_blk[172], kmock_fast_prio);
	}
	VBBAUpdate();                                            /* second call is harmless */

	pthread_create(&gt, 0, guest, 0);
	CHECK(wait_phase(1, 10000));
	CHECK(g_bba_rc == BBA_OK);                               /* strict challenge/response through the fast thread */
	CHECK(g_mac_ok);
	CHECK(g_link_rc == BBA_OK);
	CHECK(wait_phase(2, 12000));
	printf("   bba_init rc %d, link rc %d after %d ms, DHCP bound %d ms later (IOS gave the address after 300 ms), ip %u.%u.%u.%u\n", g_bba_rc, g_link_rc, g_link_ms, g_dhcp_ms,
	       S.ip >> 24, (S.ip >> 16) & 255, (S.ip >> 8) & 255, S.ip & 255);
	CHECK(S.ip == WII_IP);                                   /* MIRROR: the guest has the Wii's own address */
	CHECK(status_blk[153] >= 250);                           /* the glue saw the address no earlier than the mock IOS gives it (300 ms); the guest sees no link (so gets no DHCP answer) before that */
	CHECK(km.n_startup >= 1);
	if (nosleep) {
		sleep_ms(50);
		km.udelay_noop = 0;
		CHECK(status_blk[69] >= 16 && (status_blk[70] & 1) && km.prio_calls >= 1 && km.last_prio == 0x40);   /* sleep_fail, flags bit0, priority lowered below the main loop */
		printf("   NOSLEEP: sleep_fail %u, flags %x, reason %u, priority set to %02x (calls %u)\n", status_blk[69], status_blk[70], status_blk[76], km.last_prio, km.prio_calls);
	}
	if (hang || kdslow) {
		CHECK(status_blk[72] >= 1);                      /* ios_timeouts: the hung kd call was abandoned ... */
		CHECK(status_blk[17] >= 2);                      /* ... and the start-up was retried (startups) */
		CHECK(status_blk[39] == 0);                      /* ... with the next variant (variant 0: ncd + kd + 0x1F) */
		printf("   HANG: ios timeouts %u, startups %u, orphans now %u, net %u, variant now %u\n", status_blk[72], status_blk[17], status_blk[73], status_blk[15], status_blk[39]);
	}
	if (kdslow) CHECK(status_blk[72] == 1);              /* only the first call timed out: the retry queued behind it and came back in time */
	CHECK(status_blk[64] > 100 && main_passes > 20);     /* fast-thread heartbeat and main-loop passes both advance */
	/* start-up trace: every step is in the status block (words 28..63) and was printed by the main thread (VBBAUpdate) */
	VBBAUpdate();
	CHECK(status_blk[28] != 0 && status_blk[30] >= 4);       /* last step, sequence number */
	CHECK(status_blk[34] == (u32)km.kd_out && status_blk[35] == 0);   /* kd out word as the mock gave it, startinterface rc 0 */
	log_base = km.log_lines - kmock_tl_lines;          /* the start-up lines end here (net up); while the game runs only the periodic "VBBA: tl@" sets are written (checked before the shutdown) */
	printf("   start-up trace: variant %u ncd_rc %d kd_rc %d kd_out %d if_rc %d getif ip %08x hostid %08x | ncd calls %u kd %u startif %u hostid %u | log lines %u\n",
	       status_blk[39], (int)status_blk[32], (int)status_blk[33], (int)status_blk[34], (int)status_blk[35], status_blk[38], status_blk[36], km.n_ncd, km.n_startup, km.n_ifstart, km.n_hostid, km.log_lines);
	if (getenv("GLUE_EXPECT")) {      /* build check (tests/check_tweak_defaults.py): the defaults this build was compiled with are the ones IN FORCE at run time */
		unsigned ex_main = 0, ex_idle = 50, ex_prio = 0x70, ex_inflight = 0, ex_buffers = 0, d20, ok;
		const char *e = getenv("GLUE_EXPECT");
		sscanf(e, "main=%u idle=%u prio=%x inflight=%u buffers=%u", &ex_main, &ex_idle, &ex_prio, &ex_inflight, &ex_buffers);
		d20 = VBBAMainDelay(20);
		ok = status_blk[172] == ex_main && status_blk[132] == ex_idle && status_blk[133] == ex_prio && status_blk[156] == ex_inflight && status_blk[155] == ex_buffers && d20 == (ex_main ? ex_main : 20u) && (unsigned)kmock_fast_prio == ex_prio;
		printf("EFFECTIVE main-loop sleep %u us (delay for a 20 us request: %u), idle sleep %u us, priority %02x, sendto in flight at most %u, send buffers %u: %s\n",
		       status_blk[172] ? status_blk[172] : 20u, d20, status_blk[132], status_blk[133], status_blk[156], status_blk[155], ok ? "AS EXPECTED" : "MISMATCH");
		stop_guest = 1; pthread_join(gt, 0); stop_main = 1; pthread_join(mt, 0);
		VBBAShutdown(); kmock_stop_ios();
		return ok ? 0 : 1;
	}
	if (getenv("GLUE_NCD6")) { CHECK(hang && km.n_ncd >= 4); }      /* needs variant 0 (run with GLUE_HANG): three -6 answers, then the link is reported */

	if (stall) {          /* the fast thread blocks for 2 s in a call that does not return: the main-thread watchdog must take over */
		unsigned t0;
		km.fast_hang_ms = 2000;
		sleep_ms(700);
		CHECK(vbbaTakeover == 1 && (status_blk[70] & 4) && status_blk[76] == 3);
		t0 = trips; sleep_ms(200);
		CHECK(trips > t0 + 20);                           /* the PowerPC keeps being served, now by the main loop */
		printf("   STALL: takeover %u, flags %x, reason %u, PPC trips served by the main loop in 200 ms: %u, main gap max %u ms\n", vbbaTakeover, status_blk[70], status_blk[76], trips - t0, main_gap_ms_max);
		CHECK(main_gap_ms_max < 100);
		stop_guest = 1; pthread_join(gt, 0); stop_main = 1; pthread_join(mt, 0);
		VBBAShutdown(); CHECK(vbbaActive == 0 && vbbaTakeover == 0);
		kmock_stop_ios();
		printf("glue host test: %s (%d checks)\n", g_fail ? "FAIL" : "PASS", g_checks);
		return g_fail ? 1 : 0;
	}
	CHECK(wait_phase(3, 20000));
	printf("   UDP echoes through chip -> router -> IOS socket ops -> echo -> back: %d/12 (sendto %u, recvfrom %u, bind %u, protocol errors %u)\n",
	       g_echo_ok, km.n_sendto, km.n_recv, km.n_bind, km.protocol_errors);
	CHECK(latetx ? g_echo_ok >= 8 : g_echo_ok == 12);
	CHECK(km.protocol_errors == 0);
	CHECK(km.last_dst_ip == ECHO_LAN);
#if defined(VBBA_SYNCTX) && VBBA_SYNCTX
	{ extern unsigned kmock_sync_calls; printf("   synctx: %u blocking sendto calls by the sender thread\n", kmock_sync_calls); CHECK(kmock_sync_calls >= 12 && kmock_sync_calls >= km.n_sendto && kmock_sync_calls <= km.n_sendto + 1); }   /* one blocking call may still be in flight when the counters are read */
#endif
#if defined(VBBA_IOSBUF) && VBBA_IOSBUF
	printf("   iosbuf: last sendto data vector at offset %lx in the socket heap block\n", (unsigned long)(km.last_sendto_data - so_lo));
	CHECK(km.last_sendto_data >= so_lo && km.last_sendto_data < so_lo + 0x8000 && km.n_sendto >= 12);
#endif
#if defined(VBBA_TP_RXPOLL) && VBBA_TP_RXPOLL
	/* b18a: the socket was made non-blocking, receives only through poll -> recvfrom (never an empty recvfrom), never two requests at once */
	printf("   receive by poll: fcntl %u, poll %u (ready %u), recvfrom %u (without data %u), most sendto/poll/recvfrom requests pending at once %u\n", km.n_fcntl, km.n_poll, km.n_poll_ready, km.n_recv, km.n_recv_empty, km.net_pend_max);
	CHECK(km.n_fcntl >= 1 && km.n_poll >= km.n_poll_ready && km.n_poll_ready > 0 && km.n_recv_empty == 0 && km.net_pend_max == 1);
#endif

#ifdef GLUE_CC
	{   /* CCHECK (build_wii.sh NIN_MS_CCHECK block applied to this glue): the PowerPC side writes the mailbox, the kernel runs the check through the same router thread and IOS socket path, the mock echoes */
		extern volatile u32 cc_mem[32];
		int w, f0 = g_fail;                              /* (the older checks of this monolithic test before it, and its log/counter checks after it, are not about the Connection check: this mode ends here) */
		CHECK(cc_mem[0] == 0x4343484Bu && cc_mem[1] == ((1u << 16) | 28u) && cc_mem[3] == 0 && cc_mem[2] == 0);       /* feature present, nothing run */
		sleep_ms(1500);                                                                                               /* the guest's 12 datagrams were a burst: the check refuses while the guest sent more than 20 a second (a match) */
		cc_mem[26] = ECHO_LAN; cc_mem[27] = 7u | (10u << 16); cc_mem[24] = 1;                                         /* server, port 7 (the mock answers from there), 10 s; REQ last */
		for (w = 0; w < 400 && cc_mem[3] < 2; w++) sleep_ms(50);
		CHECK(cc_mem[2] == 1 && (cc_mem[3] == 2 || cc_mem[3] == 3) && (cc_mem[4] >> 24) == 4 && (cc_mem[5] & 1u) == 0);
		CHECK(cc_mem[6] >= 590 && cc_mem[6] <= 610 && cc_mem[7] >= cc_mem[6] * 95 / 100 && cc_mem[8] == cc_mem[6] - cc_mem[7]);
		CHECK((cc_mem[15] >> 16) > 0 && cc_mem[22] >> 16 == 0);                                                       /* sendto latency measured; no refused sends */
		printf("   CCHECK: verdict %u sent %u recv %u lost %u p50 %u p95 %u (0.1 ms) gaps %u/%u, sendto mean %u max %u (0.1 ms), run %u ds, state %08X\n", cc_mem[3], cc_mem[6], cc_mem[7], cc_mem[8], cc_mem[10] >> 16, cc_mem[10] & 0xFFFF,
		       cc_mem[13] >> 16, cc_mem[13] & 0xFFFF, cc_mem[15] >> 16, cc_mem[15] & 0xFFFF, cc_mem[17] & 0xFFFF, cc_mem[4]);
		cc_mem[27] = 7u | (30u << 16); cc_mem[24] = 2;                                                              /* a second request, cancelled after 2 s */
		sleep_ms(2000);
		CHECK(cc_mem[3] == 1 && cc_mem[2] == 2);
		cc_mem[25] = 9;
		for (w = 0; w < 100 && cc_mem[3] == 1; w++) sleep_ms(50);
		CHECK(cc_mem[3] == 0 && ((cc_mem[4] >> 16) & 0xFF) == 6 && (cc_mem[4] >> 24) == 4);                           /* not run, reason cancel */
		CHECK(status_blk[12] > 0);                                                                                    /* the game's own counters still run */
		stop_guest = 1; pthread_join(gt, 0); stop_main = 1; pthread_join(mt, 0);
		VBBAShutdown(); kmock_stop_ios();
		CHECK(vbbaActive == 0 && strstr(kmock_log, "connection check ready") != NULL);
		printf("CCHECK glue test: %s (%d checks of the mode)\n", g_fail > f0 ? "FAIL" : "PASS", g_checks);
		return g_fail > f0 ? 1 : 0;
	}
#endif
	/* B: memory-card hold: no EXI is served while the main loop writes the card image, service resumes afterwards */
	sleep_ms(30);
	cmds0 = status_blk[4];
	VBBAHoldBegin();
	polls0 = (u32)g_polls; cmds0 = status_blk[4];
	sleep_ms(80);
	cmds1 = status_blk[4]; polls1 = (u32)g_polls;
	VBBAHoldEnd();
	printf("   hold: EXI commands served during an 80 ms hold: %u (guest loop iterations %u)\n", cmds1 - cmds0, polls1 - polls0);
	CHECK(cmds1 == cmds0);
	(void)polls0; (void)polls1;                              /* the guest driver may keep looping without an EXI command (cached status): only service matters */
	sleep_ms(80);
	CHECK(status_blk[4] > cmds1 + 20);                       /* service resumed */

	{ unsigned l0 = km.log_lines - kmock_tl_lines - log_base; CHECK(l0 <= 8); }   /* no live log lines while the game runs (VBBA_LIVE_LOG 0): SD writes stall the main loop */
	/* D: shutdown */
	stop_guest = 1;
	pthread_join(gt, 0);
	CHECK(vbbaTakeover == 0 && (status_blk[71] == (nosleep ? 1u : 0u) || (status_blk[71] == 1 && status_blk[76] == 2 && !nosleep && printf("   (note: the host paused the fast thread for 20 ms: a watchdog trip of reason 2, not a fault of the glue)\n"))));      /* no watchdog trip in a healthy run (the NOSLEEP run has exactly the one)  */
	CHECK(main_gap_ms_max < 100);
	stop_main = 1; pthread_join(mt, 0);
	/* C (after the guest stopped, the test is the only PowerPC): a pending memory-card-style command while the thread runs is served too (other devices keep working) */
	{
		unsigned m0 = memcard_cmds;
		u32 r = ppc_cmd(0x10000000u | (4u << 16) | (0u << 8) | 0u, 0, 0);   /* select ch0 dev0 */
		CHECK(r == 1);
		(void)ppc_cmd(0x11000000u | (1u << 16) | 2u, 0, 1);
		CHECK(memcard_cmds == m0 + 1);
	}
	/* b17b: the timeline lines were also written during the run (built with VBBA_TL_PERIOD_MS=200), as a full set of 8 */
	CHECK(strstr(kmock_log, "VBBA: tl@") != NULL && strstr(kmock_log, " live: hb ") != NULL && strstr(kmock_log, " recvfrom errors (code x count): ") != NULL);
#if defined(VBBA_ENGSTAT) && VBBA_ENGSTAT
	/* b22a: the engine group follows the 9 timeline lines: found by its magic, values read from the (fake) PowerPC memory, the interrupt words and EXI counters of the ARM side */
	CHECK(strstr(kmock_log, " eng@804F7880 S3 magic 53335354 state 5 side 0 calls 4242 b0 4200 ") != NULL);
	CHECK(strstr(kmock_log, " eng S7 53375354: ack rx 0 tx 0 ping 77 ") != NULL);
	CHECK(strstr(kmock_log, " eng ARM: PPC int words RSW 00000000 DI 00000000 SI 00000000 EXI 00000010 | hw flag 00000000 mask 40000000") != NULL);
	CHECK(strstr(kmock_log, " eng since last set: +0 calls +0 b0, all status words unchanged") != NULL);
	printf("ENGSTAT lines checked\n");
#endif
#if defined(VBBA_SCENESTAT) && VBBA_SCENESTAT
	/* b22b: game words, DI registers, card state (the card stand-in says: unsaved 0x2000 bytes, 7 writes, 3 saves) */
	CHECK(strstr(kmock_log, " game: scene 02020002 frame 1234 scene_frames 567 end_id 00000000 ctl AABBCCDD") != NULL);
	CHECK(strstr(kmock_log, " game since last set: frame 0 scene_frames 0 | scene word unchanged | game words frozen") != NULL);
	CHECK(strstr(kmock_log, " di: DI_IRQ 0 hold 0 | last cmd A8000000 off 0048D158 len 00008000 dma 00500000 | ctl 00000000 st 00000000 | regs unchanged") != NULL);
	CHECK(strstr(kmock_log, " card: on 1 unsaved 1 (8192 B sys 0) dirty ") != NULL && strstr(kmock_log, " | writes 7 (-1 ms ago) | saves 3 (-1 ms ago)") != NULL);
	{ const char *w = strstr(kmock_log, "scene word CHANGED"); CHECK(w == NULL); }
	DI_IRQ = 1; VBBAUpdate(); sleep_ms(5); VBBAUpdate(); DI_IRQ = 0; VBBAUpdate();     /* one disc read of >= 5 ms seen by the pass loop */
	eng_fake[(0x80479D30u - ENG_FAKE_BASE) / 4] = 0x08082800u;                        /* the game left the match scene before the shutdown set */
	printf("SCENESTAT lines checked\n");
#endif
	{ const char *w = strstr(kmock_log, "VBBA: watchdog:"); CHECK(w == NULL); }          /* ... before any shutdown line */
	i = (int)status_blk[4];
	VBBAShutdown();
#if defined(VBBA_SCENESTAT) && VBBA_SCENESTAT
	CHECK(strstr(kmock_log, " di: reads seen 1 | busy now 0 ms | last ended ") != NULL);
	CHECK(strstr(kmock_log, " game: scene 08082800 frame 1234 ") != NULL);
	CHECK(strstr(kmock_log, "SCENE WORD CHANGED") != NULL);
	printf("SCENESTAT shutdown set checked\n");
#endif
	CHECK(vbbaActive == 0);
	CHECK(kmock_log_max < 200);                              /* every line printed in this run is shorter than 200 characters (the kernel buffer is 256) */
	CHECK(km.log_lines >= 8);                                /* the shutdown printed the whole trace and the watchdog line */
	VBBAShutdown();                                          /* idempotent */
	sleep_ms(20);
	CHECK(status_blk[4] == (u32)i);
	printf("   status block: commands %u selects %u imm %u dma %u (%u B) | tx frames %u rx frames %u | udp out %u in %u | net %u ip %08x | idle sleeps %u, longest sleep %u ticks, longest poll gap %u ticks, holds %u\n",
	       status_blk[4], status_blk[5], status_blk[6], status_blk[7], status_blk[9], status_blk[10], status_blk[11], status_blk[12], status_blk[13],
	       status_blk[15], status_blk[16], status_blk[21], status_blk[22], status_blk[24], status_blk[25]);
	CHECK(status_blk[15] == 3);                              /* VBBA_NET_UP */
	CHECK(status_blk[16] == WII_IP);
	CHECK(status_blk[10] > 10 && status_blk[11] > 10);       /* frames both ways */
	CHECK(status_blk[25] == 1);                              /* one hold */
	/* start-up order: variant 0 every time (ncd link status first as the gate, then kd, then so_startinterface) */
	{
		const char *p = kmock_log; int n0 = 0;
		while ((p = strstr(p, "variant rc 0 val 00000000")) != NULL) { n0++; p++; }
		CHECK(n0 >= 1 && strstr(kmock_log, "variant rc 0 val 00000001") == NULL && strstr(kmock_log, "variant rc 0 val 00000002") == NULL);
		if (hang || kdslow) CHECK(n0 >= 2);                  /* the retry is another variant 0 attempt */
		else CHECK(n0 == 1);                                 /* the first attempt gave the address */
		printf("   start-up attempts (variant 0 each): %d%s\n", n0, hang || kdslow ? " (a retry is expected here)" : "");
	}
	if (latetx) {
		CHECK(status_blk[170] >= 1 && status_blk[15] == 3 && !(status_blk[70] & 2) && status_blk[72] == 0 && status_blk[101] >= 900000u);   /* late sendto counted, no abandoned request, net up, nothing left in IOS */
		printf("   LATETX: sendto later than the soft limit %u, abandoned %u, flags %x, net %u\n", status_blk[170], status_blk[72], status_blk[70], status_blk[15]);
	}
	/* path counters (status block words 80..116) */
	CHECK(status_blk[80] == 1);
	if (!latetx) CHECK(status_blk[99] >= 12 && status_blk[99] <= status_blk[12] + 4);   /* sendto completions ~ datagrams sent */
	if (!latetx) CHECK(status_blk[101] >= 500 && status_blk[101] < 400000);             /* longest sendto: the mock needs ~1 ms (its clock has 1 ms granularity), scheduling adds a little */
	CHECK(status_blk[83] >= 1 && status_blk[83] <= 8 && status_blk[81] == 0 && status_blk[93] == 0 && status_blk[94] == 0 && status_blk[98] == 0);   /* peak in flight, no slot-full drops, no glue slot shortage, no ring overflow */
	CHECK(status_blk[110] >= (latetx ? 8u : 12u));                                          /* receives with data (the echoes) */
	CHECK(status_blk[117] >= 1 && status_blk[120] >= 1);   /* DHCP: DISCOVER seen and ACK sent (the guest sees no link before IOS has an address, so it no longer asks early) */
	{ unsigned h = 0, i2; for (i2 = 0; i2 < 8; i2++) h += status_blk[102 + i2]; CHECK(h == status_blk[99]); }   /* the histogram holds every completion */
	if (slowtx) CHECK(status_blk[104] + status_blk[105] >= 12 && status_blk[101] >= 29000);
	printf("   path counters: sendto n %u max %u us, buckets %u %u %u %u, peak in flight %u, slot-full drops %u, recv n %u dwell max %u us wait max %u ms, ext ver %u\n",
	       status_blk[99], status_blk[101], status_blk[102], status_blk[103], status_blk[104], status_blk[105], status_blk[83], status_blk[81], status_blk[110], status_blk[112], status_blk[114], status_blk[80]);
	CHECK(strstr(kmock_log, "VBBA: send: no-slot drops") != NULL && strstr(kmock_log, "VBBA: sendto latency") != NULL && strstr(kmock_log, "VBBA: recv drops") != NULL);
	/* ARM duty cycle (words 135..147), kd start-up timing (148..154), send queue (155..171) */
	CHECK(status_blk[135] > 949000u && status_blk[136] > 0 && status_blk[136] < status_blk[135] && status_blk[138] > 0 && status_blk[140] > 100 && status_blk[142] > 10);   /* wall > 1 s, asleep some of it, served commands, loops, sleeps */
	if (status_blk[135] > 3796000u) CHECK(status_blk[143] >= 1898437u && status_blk[147] > 20);   /* a full window: it closes at the first status refresh after one second; the main loop ran in it */
	CHECK((status_blk[154] & 0xB) == 0xB && status_blk[149] >= status_blk[148] && status_blk[153] >= status_blk[149]);    /* kd submitted, returned, address seen after it */
	if (hang || kdslow) CHECK((status_blk[154] & 4) && status_blk[152] >= 2000);                        /* the abandoned kd call came back late, 2.5 s / 3.3 s after its submit */
	CHECK(status_blk[155] == 24 && status_blk[156] == EXP_INFLIGHT && status_blk[165] >= (latetx ? 8u : 12u) && status_blk[166] == 0 && status_blk[159] == 0 && status_blk[160] == 0 && (latetx || status_blk[171] == 0));   /* 64 buffers, 32 in flight, nothing dropped, nothing left in IOS */
	CHECK(strstr(kmock_log, "VBBA: send queue:") != NULL && strstr(kmock_log, "VBBA: duty: fast thread awake") != NULL && strstr(kmock_log, "VBBA: kd start-up: submitted") != NULL && strstr(kmock_log, "VBBA: cfg: /vbba.cfg") != NULL);
	printf("   duty: awake %u of %u ms, asleep %u ms in %u sleeps (asked %u ms), serving %u ms, spins %u, last second %u ticks, main passes %u | kd submitted %u returned %u ms, address at %u ms | send queue peak %u wait max %u ms\n",
	       (status_blk[135] - status_blk[136]) / 1898u, status_blk[135] / 1898u, status_blk[136] / 1898u, status_blk[142], status_blk[137] / 1898u, status_blk[138] / 1898u, status_blk[141], status_blk[143], status_blk[147], status_blk[148], status_blk[149], status_blk[153], status_blk[157], status_blk[163]);
	CHECK(pub_blk[0] == 0x56424241u && pub_blk[126] == 0x56425854u && pub_blk[127] == (((u32)(uintptr_t)status_blk & 0x0FFFFFFFu) | 0xD0000000u) + 0x200u);   /* the published block carries the pointer to the extension */
	printf("   host stack use: fast thread %zu of %zu B, completion thread %zu of %zu B (ARM stacks are 8192 and 1024 B; the host test stacks are twice that, at least 16 KiB)\n", kmock_stack_used[2], kmock_stack_size[2], kmock_stack_used[1], kmock_stack_size[1]);
	CHECK(kmock_stack_used[2] > 0 && kmock_stack_used[2] <= 2 * 8192u * 3 / 4 && kmock_stack_used[1] <= 2 * 1024u * 3 / 4 + 4096u);   /* the fast thread loop stays under 75 % of a doubled stack */
	CHECK(status_blk[176] == 0);                                                                       /* the glue's own 0xA5 fill was never overrun */
	kmock_stop_ios();
	CHECK(kmock_queue_canary_ok());                          /* still untouched after the whole run */
	printf("glue host test: %s (%d checks)\n", g_fail ? "FAIL" : "PASS", g_checks);
	return g_fail ? 1 : 0;
}
