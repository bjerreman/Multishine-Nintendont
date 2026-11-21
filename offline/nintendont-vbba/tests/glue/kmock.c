/* SPDX-License-Identifier: GPL-2.0-only (host test, mock of the Nintendont kernel API and of IOS for kernel/vbba_glue.c) */
/*
 * kmock: what the Nintendont kernel and IOS would do for kernel/vbba_glue.c, on the host with pthreads:
 *   - EXI_CMD_0 / EXI_CMD_1 words (the PowerPC and the ARM thread share them), HW_TIMER at 1.898 MHz from the host clock
 *   - threads (thread_create/continue/cancel), IOS message queues, a bump heap
 *   - IOS: /dev/net/kd/request and /dev/net/ip/top decoded exactly as the glue encodes them (the layouts of
 *     kernel/sock.c as read by the glue's author: this checks the glue against our reading, NOT against a real IOS):
 *     ioctl 6 / 0x1F startup, ioctlv 0x1C own address, 0x0F socket, 0x02 bind, 0x0D sendto, 0x0C recvfrom, 0x03 close.
 *     A tiny network: datagrams sent to echo_ip (any port) come back from echo_ip:7 after latency_ms.
 * Words are handled in the value domain (the glue writes `(8<<24)|(2<<16)|port` for the sockaddr head).
 */
#include "kshim.h"
#include "kmock.h"
#include "ff_utf8.h"
#include <pthread.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

struct kmock_cfg km;
int sockFd = -1, nwcFd = -1;                       /* kernel/sock.c's handles (the glue reuses them) */
int kmock_open_kd, kmock_open_ip;                 /* IOS_Open calls seen */

static volatile u32 exi_mem[8];

static u64 ns_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
static u32 ms_now(void) { return (u32)(ns_now() / 1000000ull); }

int kmock_baked = -1;
u32 kmock_auto_info;                              /* the auto app's sweep word the loader left at 0x12FFFFD0 */                              /* the duty preset the loader left at 0x12FFFFC8/CC (-1: none) */
volatile u32 cc_mem[32];                          /* CCHECK: the mailbox at 0x13026600 (28 words) */
void glf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); if (km.verbose) { printf("   [gamelog] "); vprintf(fmt, ap); } va_end(ap); }
u32 read32(u32 addr)
{
	if (addr >= 0x13026600u && addr < 0x13026680u) { u32 v = cc_mem[(addr - 0x13026600u) / 4]; __sync_synchronize(); return v; }
	if (addr == 0x31A0u) return 0x40302010u;
	if (addr == 0x12FFFFC8u) return kmock_baked >= -1 ? 0x56424455u : 0;
	if (addr == 0x12FFFFD0u) return kmock_auto_info;
	if (addr == 0x12FFFFCCu) return kmock_baked >= 0 ? (u32)kmock_baked : 0xFFFFFFFFu;
	if (addr >= DI_BASE && addr < DI_BASE + 0x40) { u32 o = addr - DI_BASE; return o == 0x08 ? 0xA8000000u : o == 0x0C ? 0x00123456u : o == 0x10 ? 0x8000u : o == 0x14 ? 0x00500000u : 0; }     /* b22b: DI registers as the PowerPC wrote them */
	if (addr >= INT_BASE && addr < INT_BASE + 0x80) return (addr - INT_BASE) == 0x60 ? 0x10u : 0;     /* b22a: EXI interrupt pending, others clear */
	if (addr == HW_PPCIRQFLAG || addr == HW_PPCIRQMASK) return addr == HW_PPCIRQMASK ? 0x40000000u : 0;
	if (addr == HW_TIMER) return (u32)((ns_now() * 243ull) / 128000ull);
	if (addr >= EXI_BASE && addr < EXI_BASE + 0x20) { u32 v = exi_mem[(addr - EXI_BASE) / 4]; __sync_synchronize(); return v; }
	fprintf(stderr, "kmock: read32(%08x) not modelled\n", addr); abort();
}
void write32(u32 addr, u32 v)
{
	if (addr >= 0x13026600u && addr < 0x13026680u) { __sync_synchronize(); cc_mem[(addr - 0x13026600u) / 4] = v; __sync_synchronize(); return; }
	if (addr >= EXI_BASE && addr < EXI_BASE + 0x20) { __sync_synchronize(); exi_mem[(addr - EXI_BASE) / 4] = v; __sync_synchronize(); return; }
	fprintf(stderr, "kmock: write32(%08x) not modelled\n", addr); abort();
}
void sync_before_read(void *p, int len) { (void)p; (void)len; __sync_synchronize(); }
void sync_after_write(void *p, int len) { (void)p; (void)len; __sync_synchronize(); }
static int is_fast(void);
static void ns_sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, 0); }
void udelay(int us)
{
	struct timespec ts = { 0, (long)(us < 1 ? 1 : us) * 1000L };
	if (is_fast()) {
		if (km.udelay_noop) return;
		if (km.fast_hang_ms) { int h = km.fast_hang_ms; km.fast_hang_ms = 0; ns_sleep_ms(h); return; }
	}
	nanosleep(&ts, 0);
}
int thread_set_priority(int id, int prio) { (void)id; km.prio_calls++; km.last_prio = prio; return 0; }
char kmock_log[65536];
unsigned kmock_log_max;          /* longest line passed to dbgprintf (the kernel formats into a 256-byte stack buffer) */
unsigned kmock_tl_lines;         /* of them the periodic timeline lines "VBBA: tl@" */
int dbgprintf(const char *fmt, ...)
{
	va_list ap, ap2; va_start(ap, fmt);
	va_copy(ap2, ap);
	km.log_lines++;
	if (!strncmp(fmt, "VBBA: tl@", 9)) kmock_tl_lines++;
	{ char one[1024]; va_list ap3; int len; va_copy(ap3, ap); len = vsnprintf(one, sizeof one, fmt, ap3); va_end(ap3); if (len > 0 && (unsigned)len > kmock_log_max) kmock_log_max = (unsigned)len; }
	{ size_t n = strlen(kmock_log); if (n < sizeof kmock_log - 512) vsnprintf(kmock_log + n, sizeof kmock_log - n, fmt, ap2); }
	va_end(ap2);
	if (km.verbose) { printf("   [kernel] "); vprintf(fmt, ap); }
	va_end(ap);
	return 0;
}

/* ---- threads ---- */
#define MAXTH 8
static struct kthread { pthread_t t; u32 (*proc)(void *); void *arg; int started; unsigned char *stk; size_t stk_bytes; } th[MAXTH];
/* host stack actually used by each mock thread, measured at cancel: the thread runs on a stack of max(16 KiB, 2 x the ARM stack), filled with 0xA5 */
size_t kmock_stack_used[MAXTH], kmock_stack_size[MAXTH];
static int nth = 1;
static int fast_idx = 2;           /* the fast EXI thread: the last thread the glue creates */
static int is_fast(void) { return th[fast_idx].started && pthread_equal(pthread_self(), th[fast_idx].t); }
static void *th_main(void *p) { struct kthread *k = p; k->proc(k->arg); return 0; }
int kmock_fast_prio;
/* The kernel's thread_create takes the stack TOP (kernel.ld: __di_stack_addr = base + size; RealDI/HID/main.c pass those), not the base: the ARM stack grows down from it.
   Passing the base of a heap block (the glue did until round 8) makes the thread grow down into the block allocated before it (the IOS queue): the Wii freeze root cause. */
struct kmock_blk { void *p; int size; };
extern struct kmock_blk kmock_blocks[32]; extern int kmock_nblocks;
static struct { u32 *stack; u32 words; } th_args[8];
int kmock_stack_args_ok(void)       /* every thread's stack pointer lies in [block end - 16, block end] of a heap block of exactly the stack size */
{
	int i, k, ok = 1, n = 0;
	for (i = 0; i < 8; i++) {
		int found = 0;
		if (!th_args[i].stack) continue;
		n++;
		for (k = 0; k < kmock_nblocks; k++) {
			long off = (long)((u8 *)th_args[i].stack - (u8 *)kmock_blocks[k].p);
			if (kmock_blocks[k].size == (int)th_args[i].words * 4 && off >= kmock_blocks[k].size - 16 && off <= kmock_blocks[k].size) found = 1;
		}
		ok &= found;
	}
	return ok && n >= 2;
}
u32 thread_create(u32 (*proc)(void *arg), void *arg, u32 *stack, u32 stacksize, u8 priority, bool detach)
{
	(void)detach;
	if (nth < 8) { th_args[nth].stack = stack; th_args[nth].words = stacksize; }
	th[nth].stk_bytes = stacksize * 8u < 16384u ? 16384u : stacksize * 8u;   /* stacksize is in words: 2 x the ARM bytes */
	kmock_fast_prio = priority; fast_idx = nth;       /* the fast EXI thread is the last one the glue creates (after the completion, network and sender threads) */
	th[nth].proc = proc; th[nth].arg = arg;
	return (u32)nth++;
}
int thread_continue(int id)
{
	pthread_attr_t at;
	void *mem = 0;
	th[id].started = 1;
	pthread_attr_init(&at);
	mem = mmap(0, th[id].stk_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);   /* (ASan refuses a heap block as a thread stack) */
	if (mem != MAP_FAILED) { memset(mem, 0xA5, th[id].stk_bytes); th[id].stk = mem; pthread_attr_setstack(&at, mem, th[id].stk_bytes); kmock_stack_size[id] = th[id].stk_bytes; }
	pthread_create(&th[id].t, &at, th_main, &th[id]);
	return 0;
}
int thread_cancel(int id, int when)
{
	(void)when;
	if (!th[id].started) return 0;
	pthread_cancel(th[id].t); pthread_join(th[id].t, 0); th[id].started = 0;
	if (th[id].stk) { size_t i = 0; while (i < th[id].stk_bytes && th[id].stk[i] == 0xA5) i++; kmock_stack_used[id] = th[id].stk_bytes - i; }
	return 0;
}

/* ---- the SD card's /vbba.cfg ---- */
vu32 SDisInit = 1;
const char *kmock_cfg_text;
static const char *cfg_pos;
FRESULT f_open_char(FIL *fp, const char *path, unsigned char mode)
{
	(void)fp; (void)mode;
	if (!strcmp(path, "/vbba_sweep.txt")) return kmock_write_fails ? 1 : FR_OK;
	if (strcmp(path, "/vbba.cfg") || !kmock_cfg_text) return 4;     /* FR_NO_FILE */
	cfg_pos = kmock_cfg_text;
	return FR_OK;
}
FRESULT f_read(FIL *fp, void *buff, u32 btr, u32 *br)
{
	u32 n = (u32)strlen(cfg_pos);
	(void)fp;
	if (n > btr) n = btr;
	memcpy(buff, cfg_pos, n); cfg_pos += n; *br = n;
	return FR_OK;
}
char kmock_written[16]; int kmock_write_fails;
FRESULT f_write(FIL *fp, const void *buff, u32 btw, u32 *bw) { (void)fp; if (kmock_write_fails) { *bw = 0; return 1; } memcpy(kmock_written, buff, btw < 15 ? btw : 15); *bw = btw; return FR_OK; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }

/* ---- message queues ---- */
#define MAXQ 4
static struct mq { pthread_mutex_t m; pthread_cond_t c; struct ipcmessage *q[256]; int head, tail; } mqs[MAXQ];
static int nmq;
/* queue memory accounting: the glue passes a heap block of B bytes for an N-entry queue; real IOS needs 4 x N bytes (and, on the Wii, evidently more: zero bytes of
   slack after the array froze or stalled the console).  The bytes after 4 x N are filled with a canary; kmock_queue_canary_ok() fails when anything wrote there. */
struct kmock_blk kmock_blocks[32]; int kmock_nblocks;
int kmock_q_entries, kmock_q_bytes; static u32 *q_canary; static int q_canary_n;
int kmock_queue_canary_ok(void) { int i; for (i = 0; i < q_canary_n; i++) if (q_canary[i] != 0xC5C5C5C5u) return 0; return 1; }
int mqueue_create(void *ptr, int n)
{
	struct mq *q = &mqs[nmq]; int i;
	for (i = 0; i < kmock_nblocks; i++) if (kmock_blocks[i].p == ptr && nmq == 0) {
		kmock_q_entries = n; kmock_q_bytes = kmock_blocks[i].size;
		q_canary = (u32 *)((u8 *)ptr + 4 * n); q_canary_n = (kmock_q_bytes - 4 * n) / 4; if (q_canary_n < 0) q_canary_n = 0;
		for (int k = 0; k < q_canary_n; k++) q_canary[k] = 0xC5C5C5C5u;
	}
	pthread_mutex_init(&q->m, 0); pthread_cond_init(&q->c, 0);
	return nmq++;
}
static void mq_post(int qi, struct ipcmessage *m)
{
	struct mq *q = &mqs[qi];
	pthread_mutex_lock(&q->m); q->q[q->tail++ & 255] = m; pthread_cond_signal(&q->c); pthread_mutex_unlock(&q->m);
}
static void unlock_cb(void *p) { pthread_mutex_unlock((pthread_mutex_t *)p); }
int mqueue_recv(int qi, struct ipcmessage **m, int flags)
{
	struct mq *q = &mqs[qi]; (void)flags;
	pthread_mutex_lock(&q->m);
	pthread_cleanup_push(unlock_cb, &q->m);
	while (q->head == q->tail) pthread_cond_wait(&q->c, &q->m);
	*m = q->q[q->head++ & 255];
	pthread_cleanup_pop(1);
	return 0;
}
void mqueue_ack(struct ipcmessage *m, int retval) { (void)m; (void)retval; }
int mqueue_send_now(int qi, struct ipcmessage *m, int flags) { (void)flags; mq_post(qi, m); return 0; }

/* ---- heap: bump allocator inside the region the glue passes ---- */
static struct { u8 *base; int size, used; } hp[2];
static int nhp;
int soHeap = -1;                                   /* kernel/sock.c's heap (b19 build); the test creates it when VBBA_IOSBUF */
int heap_create(void *base, int size) { hp[nhp].base = base; hp[nhp].size = size; hp[nhp].used = 0; return nhp++; }
void heap_destroy(int h) { (void)h; }
void *heap_alloc_aligned(int h, int size, int align)
{
	uintptr_t a = (uintptr_t)hp[h].base + (uintptr_t)hp[h].used;
	a = (a + (uintptr_t)align - 1) & ~((uintptr_t)align - 1);
	if ((int)(a - (uintptr_t)hp[h].base) + size > hp[h].size) return 0;
	hp[h].used = (int)(a - (uintptr_t)hp[h].base) + size;
	if (kmock_nblocks < 32) { kmock_blocks[kmock_nblocks].p = (void *)a; kmock_blocks[kmock_nblocks++].size = size; }
	return (void *)a;
}
void heap_free(int h, void *p) { (void)h; (void)p; }

/* ---- IOS ---- */
#define FD_IP 100
#define FD_KD 101
#define FD_NCD 102
#define MAXREQ 128
struct req { int used; u32 due; int kind; int fd, request; void *in, *io; ioctlv *vec; int qi; struct ipcmessage *msg; };
struct dg { u32 ip; u16 port, len; u8 d[2048]; u32 due; };
struct sk { int used, nb; u16 port; struct dg q[64]; int cnt; };
static struct req rq[MAXREQ];
static struct sk sks[8];
static pthread_mutex_t iom = PTHREAD_MUTEX_INITIALIZER;
static pthread_t io_thread; static int io_started, io_stop;
static void ensure_io(void);
static u32 t_open_ms;

static void complete(struct req *r, int result) { r->msg->result = (unsigned)result; mq_post(r->qi, r->msg); r->used = 0; }

static int service_one(struct req *r)    /* returns 1 when completed */
{
	u32 now = ms_now();
	u32 *in = r->in, *io = r->io;
	if (r->fd == FD_KD && r->request == 6) { io[0] = (u32)km.kd_out; complete(r, 0); km.n_startup++; return 1; }
	if (r->fd == FD_NCD && r->request == 7) {                /* ioctlv ":d": no input, one 32-byte output */
		if (r->vec[0].len != 32 || ((uintptr_t)r->vec[0].data & 31)) km.protocol_errors++;
		km.n_ncd++;
		if (km.ncd_minus6 > 0) { km.ncd_minus6--; complete(r, -6); } else { memset(r->vec[0].data, 0, 32); complete(r, 0); }
		return 1; }
	if (r->fd != FD_IP) { complete(r, -1); return 1; }
	switch (r->request) {
	case 0x1F: km.n_ifstart++; complete(r, 0); return 1;
	case 0x10: km.n_hostid++; complete(r, (now - t_open_ms >= km.ip_after_ms) ? (int)km.ip : 0); return 1;
	case 0x1C: {
		u32 *a = r->vec[0].data;
		if (r->vec[0].len != 8 || a[0] != 0xFFFE || a[1] != 0x4003 || r->vec[1].len != 4 || r->vec[2].len != 4) { km.protocol_errors++; complete(r, -1); return 1; }
		*(u32 *)r->vec[1].data = (!km.hostid_only && now - t_open_ms >= km.ip_after_ms) ? km.ip : 0;
		complete(r, 0); return 1; }
	case 0x0F: {
		int i;
		if (!in || in[0] != 2 || in[1] != 2 || in[2] != 0) km.protocol_errors++;
		for (i = 0; i < 8; i++) if (!sks[i].used) { memset(&sks[i], 0, sizeof sks[i]); sks[i].used = 1; complete(r, 200 + i); return 1; }
		complete(r, -1); return 1; }
	case 0x02: {
		int i = (int)in[0] - 200;
		if (i < 0 || i >= 8 || !sks[i].used || in[1] != 1 || (in[2] >> 24) != 8 || ((in[2] >> 16) & 0xFF) != 2) { km.protocol_errors++; complete(r, -1); return 1; }
		sks[i].port = (u16)(in[2] & 0xFFFF); km.n_bind++;
		complete(r, 0); return 1; }
	case 0x05: {                                              /* fcntl(fd, F_SETFL 4, O_NONBLOCK 4), the IOS values */
		int i = (int)in[0] - 200;
		if (i < 0 || i >= 8 || !sks[i].used || in[1] != 4 || in[2] != 4) { km.protocol_errors++; complete(r, -1); return 1; }
		sks[i].nb = 1; km.n_fcntl++;
		complete(r, 0); return 1; }
	case 0x0B: {                                              /* poll: in = u64 timeout (0), out = one pollsd {fd, events, revents} */
		int i = (int)io[0] - 200, k, rdy = 0;
		if (i < 0 || i >= 8 || !sks[i].used || in[0] != 0 || in[1] != 0 || io[1] != 1) { km.protocol_errors++; complete(r, -1); return 1; }
		for (k = 0; k < sks[i].cnt; k++) if ((int)(now - sks[i].q[k].due) >= 0) { rdy = 1; break; }
		km.n_poll++; if (rdy) km.n_poll_ready++;
		io[2] = rdy ? 1u : 0u;
		complete(r, rdy); return 1; }
	case 0x0D: {
		u32 *p = r->vec[1].data;
		int i = (int)p[0] - 200;
		u32 dip = p[4]; u16 dport = (u16)(p[3] & 0xFFFF);
		if (i < 0 || i >= 8 || !sks[i].used || r->vec[1].len != 40 || p[2] != 1 || (p[3] >> 24) != 8 || ((p[3] >> 16) & 0xFF) != 2) { km.protocol_errors++; complete(r, -1); return 1; }
		km.last_sendto_data = (uintptr_t)r->vec[0].data;
		km.n_sendto++; km.sent_bytes += r->vec[0].len; km.last_dst_ip = dip; km.last_dst_port = dport;
		if (dip == km.echo_ip && sks[i].cnt < 64 && r->vec[0].len <= 2048) {
			struct dg *d = &sks[i].q[sks[i].cnt++];
			d->ip = dip; d->port = 7; d->len = (u16)r->vec[0].len; memcpy(d->d, r->vec[0].data, r->vec[0].len); d->due = now + km.latency_ms;
			if (d->len >= 32 && !memcmp(d->d, "MSEC", 4)) d->d[5] = 1;      /* CCHECK: the echo server turns a request into a reply */
		}
		complete(r, (int)r->vec[0].len); return 1; }
	case 0x0C: {
		u32 *a = r->vec[0].data;
		int i = (int)a[0] - 200, k;
		if (i < 0 || i >= 8 || !sks[i].used) { complete(r, -1); return 1; }
		if (r->vec[2].len != 8 || r->vec[1].len < 1) km.protocol_errors++;
		for (k = 0; k < sks[i].cnt; k++) if ((int)(now - sks[i].q[k].due) >= 0) {
			struct dg *d = &sks[i].q[k]; u32 *sa = r->vec[2].data; u32 n = d->len > r->vec[1].len ? r->vec[1].len : d->len;
			memcpy(r->vec[1].data, d->d, n);
			sa[0] = (8u << 24) | (2u << 16) | d->port; sa[1] = d->ip;
			km.n_recv++;
			memmove(&sks[i].q[k], &sks[i].q[k + 1], (size_t)(sks[i].cnt - k - 1) * sizeof(struct dg)); sks[i].cnt--;
			complete(r, (int)n); return 1;
		}
		if (sks[i].nb) { km.n_recv_empty++; complete(r, -11); return 1; }
		return 0; }
	case 0x03: {
		int i = (int)in[0] - 200;
		if (i >= 0 && i < 8) sks[i].used = 0;
		km.n_close++;
		complete(r, 0); return 1; }
	}
	km.protocol_errors++; complete(r, -1); return 1;
}

static void *io_main(void *p)
{
	(void)p;
	while (!io_stop) {
		int i;
		pthread_mutex_lock(&iom);
		for (i = 0; i < MAXREQ; i++) if (rq[i].used && (int)(ms_now() - rq[i].due) >= 0) (void)service_one(&rq[i]);
		pthread_mutex_unlock(&iom);
		udelay(200);
	}
	return 0;
}

static u32 kd_busy_until;                          /* kd_serial: when the kd call in flight finishes */
static int submit(int fd, int request, void *in, void *io, ioctlv *vec, int qi, struct ipcmessage *msg)
{
	int i;
	pthread_mutex_lock(&iom);
	for (i = 0; i < MAXREQ; i++) if (!rq[i].used) {
		rq[i].used = 1; rq[i].due = ms_now() + (request == 0x0C ? 0 : km.latency_ms) + (fd == FD_IP && request == 0x0D ? km.sendto_extra_ms : 0); rq[i].fd = fd; rq[i].request = request;
		if (fd == FD_IP && request == 0x0D && km.late_first_n > 0) { km.late_first_n--; rq[i].due += km.late_ms; }
		if (fd == FD_KD && request == 6 && km.hang_kd > 0) { km.hang_kd--; rq[i].due = ms_now() + (km.kd_late_ms ? km.kd_late_ms : 0x3FFFFFFFu); }
		if (fd == FD_KD && request == 6 && km.kd_serial) {
			if (kd_busy_until && (int)(rq[i].due - kd_busy_until) <= 0 && (int)(kd_busy_until - ms_now()) > 0) rq[i].due = kd_busy_until + 1;     /* queued behind the call in flight */
			kd_busy_until = rq[i].due;
		}
		rq[i].in = in; rq[i].io = io; rq[i].vec = vec; rq[i].qi = qi; rq[i].msg = msg;
		km.submitted++;
		{ int k; unsigned c = 0; for (k = 0; k < MAXREQ; k++) if (rq[k].used && rq[k].fd == FD_IP && (rq[k].request == 0x0D || rq[k].request == 0x0B || rq[k].request == 0x0C)) c++; if (c > km.net_pend_max) km.net_pend_max = c; }
		pthread_mutex_unlock(&iom);
		return 0;
	}
	pthread_mutex_unlock(&iom);
	return -1;
}
static void ensure_io(void) { if (!io_started) { io_started = 1; io_stop = 0; t_open_ms = ms_now(); pthread_create(&io_thread, 0, io_main, 0); } }
int kmock_kd_rc;                                  /* nonzero: IOS_Open of kd/request fails with this value */
void kmock_stock_handles(int on) { sockFd = on ? FD_IP : -1; nwcFd = on ? FD_KD : -1; if (on) ensure_io(); }
int IOS_Open(const char *device, int mode)
{
	(void)mode;
	ensure_io();
	if (!strcmp(device, "/dev/net/ip/top")) { kmock_open_ip++; return FD_IP; }
	if (!strcmp(device, "/dev/net/kd/request")) { kmock_open_kd++; return kmock_kd_rc ? kmock_kd_rc : FD_KD; }   /* real IOS: -10 while another handle is open */
	if (!strcmp(device, "/dev/net/ncd/manage")) return FD_NCD;
	return -6;
}
void IOS_Close(int fd) { (void)fd; }
int IOS_IoctlAsync(int fd, int request, void *in, int lin, void *io, int lio, int queue, struct ipcmessage *message)
{
	(void)lin; (void)lio;
	return submit(fd, request, in, io, 0, queue, message);
}
int IOS_IoctlvAsync(int fd, int request, int nin, int nio, void *vec, int queue, void *message)
{
	(void)nin; (void)nio;
	return submit(fd, request, 0, 0, vec, queue, message);
}
/* b20: the blocking call of the sender thread (one caller): a private queue slot, MAXQ-1, is the reply path */
unsigned kmock_sync_calls;
int IOS_Ioctlv(int fd, int request, int nin, int nio, void *vec)
{
	static int init; struct ipcmessage m, *mm = 0;
	(void)nin; (void)nio;
	if (!init) { init = 1; pthread_mutex_init(&mqs[MAXQ - 1].m, 0); pthread_cond_init(&mqs[MAXQ - 1].c, 0); }
	memset(&m, 0, sizeof m);
	if (submit(fd, request, 0, 0, vec, MAXQ - 1, &m) < 0) return -1;
	kmock_sync_calls++;
	mqueue_recv(MAXQ - 1, &mm, 0);
	return (int)m.result;
}
void kmock_stop_ios(void) { if (io_started) { io_stop = 1; pthread_join(io_thread, 0); io_started = 0; } }
