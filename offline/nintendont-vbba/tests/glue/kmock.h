/* SPDX-License-Identifier: GPL-2.0-only (host test) */
#ifndef KMOCK_H
#define KMOCK_H
#include "kshim.h"
typedef uint64_t u64;

struct kmock_cfg {
	int verbose;
	u32 ip, ip_after_ms, echo_ip, latency_ms;
	unsigned submitted, n_startup, n_bind, n_sendto, n_recv, n_close, protocol_errors;
	uintptr_t last_sendto_data;   /* b19: address of the datagram vector of the last sendto */
	unsigned n_fcntl, n_poll, n_poll_ready, n_recv_empty, net_pend_max;   /* b18a: fcntl / poll requests, polls with data, non-blocking recvfrom without data, most sendto+poll+recvfrom requests pending at once */
	unsigned sent_bytes; u32 last_dst_ip; u16 last_dst_port;
	int kd_out;                    /* word the kd startup ioctl writes to its output (-15 on the Wii) */
	int ncd_minus6;                /* ncd GetLinkStatus answers -6 this many times first */
	int hostid_only;               /* getinterfaceopt(0x4003) keeps answering 0; only SO_GETHOSTID (0x10) gives the address */
	unsigned n_ncd, n_hostid, n_ifstart, log_lines;
	int hang_kd;                   /* the first N kd startup calls never complete on their own ... */
	u32 kd_late_ms;                /* ... unless this is set: they then complete this long after the submit (a late return) */
	volatile int fast_hang_ms;     /* the vBBA thread's next udelay blocks this long (a thread stuck in a blocking call) */
	volatile int udelay_noop;      /* the vBBA thread's udelay returns at once (timer/queue creation failing in the real udelay) */
	unsigned prio_calls; int last_prio;
	int kd_serial;                 /* the IOS kd service handles one request at a time: a start-up call submitted while another is in flight completes 1 ms after it (what runs 3-7 on the Wii showed) */
	int late_first_n; u32 late_ms;   /* the first N sendto requests take late_ms longer (a Wi-Fi stall: counted as late, not abandoned) */
	u32 sendto_extra_ms;           /* sendto completes this long after its submit (a slow IOS sendto), on top of latency_ms */
};
/* the text of /vbba.cfg the mock SD card holds (NULL: no such file); SDisInit (kernel/vsprintf.c) is set by the test */
extern const char *kmock_cfg_text;
extern u32 kmock_auto_info;
extern char kmock_written[16];
extern int kmock_write_fails;
extern int kmock_baked;                           /* the duty preset the loader left for the kernel (-1: none, the loader wrote the 'no preset' value) */
extern int kmock_fast_prio;                       /* the priority the glue gave its fast thread */
/* everything the glue passed to dbgprintf, in order (NUL-terminated, capped) */
extern char kmock_log[65536];
extern unsigned kmock_log_max, kmock_tl_lines;
extern struct kmock_cfg km;
void kmock_stop_ios(void);
extern size_t kmock_stack_used[8], kmock_stack_size[8];   /* host bytes used / available per mock thread (1 completion, 2 fast), valid after thread_cancel */
#endif

/* host check of the IOS queue memory: entries, heap bytes of the block, and whether the bytes after the array are still untouched */
extern int kmock_q_entries, kmock_q_bytes;
int kmock_queue_canary_ok(void);
int kmock_stack_args_ok(void);      /* thread stacks were passed as base + size (the kernel takes the top) */
