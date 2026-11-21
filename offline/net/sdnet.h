/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SDNET_H
#define SDNET_H

/*
 * sdnet: tiny project-owned Ethernet/ARP/IPv4/ICMP/UDP/DHCP stack.
 * Portable C89, no dynamic allocation, no libc beyond nothing (own helpers),
 * explicit big-endian byte access (safe on unaligned data, any host
 * endianness).  IPv4 addresses in the API are host-order 32-bit values
 * (192.168.1.2 == 0xC0A80102).  Single-threaded: call sdnet_poll from one
 * context; no function here is reentrant or interrupt safe.
 */

typedef unsigned char sdnet_u8;
typedef unsigned short sdnet_u16;
typedef unsigned int sdnet_u32;
typedef int sdnet_s32;

/* ---- compile-time limits (override with -D) ---- */
#ifndef SDNET_MAX_SOCKETS
#define SDNET_MAX_SOCKETS 4
#endif
#ifndef SDNET_SOCK_SLOTS          /* datagrams queued per socket */
#define SDNET_SOCK_SLOTS 4
#endif
#ifndef SDNET_SOCK_PAYLOAD        /* max queued datagram payload bytes */
#define SDNET_SOCK_PAYLOAD 512
#endif
#ifndef SDNET_ARP_ENTRIES
#define SDNET_ARP_ENTRIES 8
#endif
#ifndef SDNET_ARP_TTL_MS
#define SDNET_ARP_TTL_MS 60000u
#endif
/* Neighbour entries never expire under traffic ( fix): an entry is refreshed (a) by ANY received
   frame from its MAC, (b) ahead of time by a unicast request once it is SDNET_ARP_REFRESH_MS old and was used within the last TTL (falling back
   to broadcast after two unanswered tries), and it stays USABLE until SDNET_ARP_STALE_MS (stale-while-revalidate), so a datagram is never
   dropped because the TTL elapsed.  The first datagram to a not-yet-resolved neighbour is held in a one-slot queue (SDNET_PEND_MAX bytes) and
   sent when the reply arrives; anything that cannot be held is counted in stats.pend_drop, never silently lost. */
#ifndef SDNET_ARP_REFRESH_MS
#define SDNET_ARP_REFRESH_MS (SDNET_ARP_TTL_MS / 10u * 8u)
#endif
#ifndef SDNET_ARP_STALE_MS
#define SDNET_ARP_STALE_MS (SDNET_ARP_TTL_MS * 3u)
#endif
#ifndef SDNET_PEND_MAX
#define SDNET_PEND_MAX 512
#endif
#ifndef SDNET_ARP_PENDING_MS      /* min gap between ARP requests per target */
#define SDNET_ARP_PENDING_MS 250u
#endif
/* Console builds (the S3/S4 game images) define SDNET_CONSOLE: sdnet_poll then runs with interrupts off inside the game, so the
   defaults trade features for bounded time under a flood ( N6):
     SDNET_RX_BUDGET  frames read from the NIC per sdnet_poll   4 (host/test default 16); each 1500 B frame costs ~0.4 ms of EXI
     SDNET_ENABLE_ICMP echo replies (ping)                       0 (host/test default 1); a ping flood would also cost a 1514 B TX each
   Either can still be overridden with -DSDNET_RX_BUDGET=n / -DSDNET_ENABLE_ICMP=1.  Other tunables: SDNET_MAX_SOCKETS,
   SDNET_SOCK_SLOTS, SDNET_SOCK_PAYLOAD, SDNET_ARP_*, SDNET_DHCP_*. */
#ifndef SDNET_RX_BUDGET           /* frames handled per sdnet_poll */
#ifdef SDNET_CONSOLE
#define SDNET_RX_BUDGET 4
#else
#define SDNET_RX_BUDGET 16
#endif
#endif
#ifndef SDNET_ENABLE_ICMP
#ifdef SDNET_CONSOLE
#define SDNET_ENABLE_ICMP 0
#else
#define SDNET_ENABLE_ICMP 1
#endif
#endif
#ifndef SDNET_DHCP_INIT_TIMEOUT_MS
#define SDNET_DHCP_INIT_TIMEOUT_MS 2000u
#endif
#ifndef SDNET_DHCP_MAX_TIMEOUT_MS
#define SDNET_DHCP_MAX_TIMEOUT_MS 16000u
#endif
#ifndef SDNET_DHCP_MAX_TRIES      /* transmissions per stage before FAILED */
#define SDNET_DHCP_MAX_TRIES 5
#endif
#ifndef SDNET_DHCP_RENEW_RETRY_MS
#define SDNET_DHCP_RENEW_RETRY_MS 10000u
#endif

/* TCP (sdtcp.c): off by default so the UDP-only builds stay byte-identical.  With -DSDNET_ENABLE_TCP=1, ip_rx hands protocol 6 to the tcp_rx hook
   and sdnet_poll calls the tcp_poll hook (both set by sdtcp_init, which also stores its context in s->tcp). */
#ifndef SDNET_ENABLE_TCP
#define SDNET_ENABLE_TCP 0
#endif

#define SDNET_FRAME_MAX 1518      /* max Ethernet frame (no FCS) we accept: 1514 */
#define SDNET_BUF_SIZE 1536
#define SDNET_UDP_MAX 1472        /* largest UDP payload for sendto */

/* ---- error codes ---- */
#define SDNET_OK 0
#define SDNET_E_PARAM (-1)
#define SDNET_E_NOIP (-2)         /* no address configured yet */
#define SDNET_E_TOOBIG (-3)
#define SDNET_E_ARP (-4)          /* next hop unresolved; ARP request sent, retry */
#define SDNET_E_NIC (-5)
#define SDNET_E_EMPTY (-6)        /* nothing queued */
#define SDNET_E_NOSOCK (-7)
#define SDNET_E_INUSE (-8)
#define SDNET_E_NOROUTE (-9)

/* ---- NIC abstraction ---- */
struct sdnet_nic {
    /* returns 0 on success, <0 on failure; frame has no FCS, len 14..1514 */
    int (*send)(void *ctx, const sdnet_u8 *frame, sdnet_u16 len);
    /* returns frame length (>0), 0 when nothing pending, <0 on error;
       must never write more than max bytes */
    int (*recv)(void *ctx, sdnet_u8 *buf, sdnet_u16 max);
    void *ctx;
    sdnet_u8 mac[6];
};

typedef sdnet_u32 (*sdnet_clock_fn)(void *ctx);   /* milliseconds, wraps ok */

/* Called from sdnet_poll after a datagram was queued on the socket. */
typedef void (*sdnet_udp_cb)(int sock, void *user);

/* ---- DHCP states ---- */
#define SDNET_DHCP_IDLE 0
#define SDNET_DHCP_SELECTING 1
#define SDNET_DHCP_REQUESTING 2
#define SDNET_DHCP_BOUND 3
#define SDNET_DHCP_RENEWING 4
#define SDNET_DHCP_REBINDING 5
#define SDNET_DHCP_FAILED 6

struct sdnet_dgram {
    sdnet_u32 src_ip;
    sdnet_u16 src_port;
    sdnet_u16 len;
    sdnet_u8 data[SDNET_SOCK_PAYLOAD];
};

struct sdnet_socket {
    sdnet_u8 used;
    sdnet_u8 head;
    sdnet_u8 count;
    sdnet_u16 port;
    sdnet_udp_cb cb;
    void *user;
    sdnet_u32 dropped;
    struct sdnet_dgram q[SDNET_SOCK_SLOTS];
};

struct sdnet_arp_entry {
    sdnet_u32 ip;
    sdnet_u32 stamp;              /* last time the neighbour was heard (valid) / last request (pending) */
    sdnet_u32 last_use;           /* last time a datagram was routed through this entry */
    sdnet_u32 req_stamp;          /* last refresh request */
    sdnet_u8 tries;               /* unanswered refresh requests */
    sdnet_u8 state;               /* 0 free, 1 pending, 2 valid */
    sdnet_u8 mac[6];
};

struct sdnet_stats {
    sdnet_u32 rx_frames, tx_frames;
    sdnet_u32 rx_bad;             /* malformed / failed checksum */
    sdnet_u32 rx_frag;            /* fragments dropped */
    sdnet_u32 rx_nomatch;         /* not for us / no socket / no handler */
    sdnet_u32 sock_drop;          /* socket ring full or datagram too large */
    sdnet_u32 arp_refresh;        /* refresh-ahead requests sent */
    sdnet_u32 arp_touch;          /* entries kept alive by a received frame */
    sdnet_u32 pend_sent;          /* held datagrams sent after resolution */
    sdnet_u32 pend_drop;          /* held datagram lost (timeout, too big) */
    sdnet_u32 pend_replaced;      /* held datagram replaced by a newer one */
};

struct sdnet {
    struct sdnet_nic nic;
    sdnet_clock_fn clock;
    void *clock_ctx;
    sdnet_u32 now;
    sdnet_u32 ip, mask, gw, dns;
    sdnet_u16 ip_id;
    sdnet_u32 rng;
    struct sdnet_arp_entry arp[SDNET_ARP_ENTRIES];
    struct sdnet_socket sock[SDNET_MAX_SOCKETS];
    struct {
        sdnet_u8 state;
        sdnet_u8 tries;
        sdnet_u8 server_mac[6];
        sdnet_u8 have_server_mac;
        sdnet_u8 infinite;
        sdnet_u32 xid, server_ip, offered_ip;
        sdnet_u32 deadline;       /* next retransmit (ms clock) */
        sdnet_u32 t_bound;        /* ms clock at last ACK */
        sdnet_u32 lease_s, t1_s, t2_s;
    } dhcp;
    struct sdnet_stats stats;
    struct {
        sdnet_u8 used;
        sdnet_u16 src_port, dst_port, len;
        sdnet_u32 dst_ip, hop, t;
        sdnet_u8 data[SDNET_PEND_MAX];
    } pend;
    sdnet_u8 announce;            /* send a gratuitous ARP at the next poll */
#if SDNET_ENABLE_TCP
    /* Hooks, all NULL until sdtcp_init (or an overlay that carries sdtcp.c) fills them: the engine's sdnet needs no link dependency on sdtcp,
       and setting tcp_rx to NULL again (match start) makes protocol 6 cost one branch. */
    void *tcp;
    void (*tcp_rx)(void *tcp, sdnet_u32 src, sdnet_u32 dst, const sdnet_u8 *seg, sdnet_u32 len);
    void (*tcp_poll)(void *tcp);
#endif
    sdnet_u8 rxbuf[SDNET_BUF_SIZE];
    sdnet_u8 txbuf[SDNET_BUF_SIZE];
};

/* ---- setup ---- */
void sdnet_init(struct sdnet *s, const struct sdnet_nic *nic,
                sdnet_clock_fn clock, void *clock_ctx);
void sdnet_set_static(struct sdnet *s, sdnet_u32 ip, sdnet_u32 mask,
                      sdnet_u32 gw);
void sdnet_dhcp_start(struct sdnet *s);   /* (re)start; drops current config */
int sdnet_dhcp_state(const struct sdnet *s);
int sdnet_configured(const struct sdnet *s);   /* nonzero if ip != 0 */

/* Drive the stack: receive up to SDNET_RX_BUDGET frames, run timers. */
void sdnet_poll(struct sdnet *s);

/* ---- UDP sockets ---- */
/* returns socket id >= 0, or SDNET_E_INUSE / SDNET_E_NOSOCK / SDNET_E_PARAM.
   Port 0 and port 68 (DHCP client) are rejected. cb may be NULL. */
int sdnet_bind(struct sdnet *s, sdnet_u16 port, sdnet_udp_cb cb, void *user);
void sdnet_close(struct sdnet *s, int sock);
/* returns payload length sent or a negative SDNET_E_* */
int sdnet_sendto(struct sdnet *s, int sock, sdnet_u32 dst_ip,
                 sdnet_u16 dst_port, const void *data, sdnet_u16 len);
/* returns bytes copied (truncated to max), or SDNET_E_EMPTY */
int sdnet_recvfrom(struct sdnet *s, int sock, void *buf, sdnet_u16 max,
                   sdnet_u32 *src_ip, sdnet_u16 *src_port);
int sdnet_pending(const struct sdnet *s, int sock);

#if SDNET_ENABLE_TCP
/* Unicast IPv4 transmit for other protocols: the L4 payload (plen bytes) is already at s->txbuf + SDNET_L4_OFF.  Resolves the
   next hop like sdnet_sendto (SDNET_E_ARP: request sent, retry later).  Returns SDNET_OK or a negative SDNET_E_*. */
#define SDNET_L4_OFF 34
int sdnet_tx_ip(struct sdnet *s, sdnet_u8 proto, sdnet_u32 dst_ip, sdnet_u16 plen);
#endif

/* ---- ARP ---- */
int sdnet_arp_lookup(struct sdnet *s, sdnet_u32 ip, sdnet_u8 mac[6]); /* 1/0 */
int sdnet_arp_resolve(struct sdnet *s, sdnet_u32 ip, sdnet_u8 mac[6]);
/* resolve: 1 if known now; else sends a (rate limited) request, returns 0 */

/* ---- helpers (exported for tests) ---- */
sdnet_u32 sdnet_csum_add(sdnet_u32 sum, const sdnet_u8 *p, sdnet_u32 len);
sdnet_u16 sdnet_csum_fold(sdnet_u32 sum);     /* one's complement of folded sum */
sdnet_u16 sdnet_checksum(const sdnet_u8 *p, sdnet_u32 len);

#endif
