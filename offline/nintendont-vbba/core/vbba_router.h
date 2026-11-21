/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/*
 * vbba_router: a tiny virtual router that turns the guest's Ethernet frames into UDP datagrams on a vbba_transport
 * and back.  It is a vbba_frame_backend (vbba_router_backend()), so it sits directly below vbba_chip.
 *
 *   ARP      answers every request for an address other than the guest's own (proxy ARP: gateway and on-link peers
 *            all resolve to the router MAC); gratuitous / probe requests are ignored; rate limited.
 *   DHCP     server with one fixed lease: in MIRROR mode the transport's own address (the Wii's IP, so anything the
 *            guest puts in a payload about itself is right), otherwise cfg.guest_ip/mask/gw (private subnet).
 *            No answer until the lease address is known; the DISCOVER is kept (one, newest wins) and offered the moment the address is
 *            there (before, the guest only got its lease at its next retry: 2, 6, 14, 30 s after its start; 14.05 s and 30.06 s on the Wii).
 *   UDP out  allow-listed destinations only (default: nothing); source must be the leased address; one transport
 *            socket per guest source port (bound to the same port when the platform allows); conntrack entry.
 *   UDP in   datagrams from a conntrack peer (or, with OPEN_INBOUND, from an allow-listed one) become frames with
 *            IPv4/UDP headers and checksums addressed to the guest.
 *   ICMP     echo requests to the router address are answered (VBBA_RF_ICMP); everything else is dropped.
 *   TCP      VBBA_TCP=1 and a transport with stream sockets: outbound connections are spliced onto platform TCP sockets (vbba_tcp.h);
 *            otherwise dropped and counted (tx_tcp).
 *   IPv6, fragments, multicast, broadcast (except DHCP) are dropped and counted.
 * No heap; all state in struct vbba_router (sizes below).
 */
#ifndef VBBA_ROUTER_H
#define VBBA_ROUTER_H

#include "vbba_types.h"
#include "vbba_transport.h"
#include "vbba_tcp.h"

#ifndef VBBA_MAPS
#define VBBA_MAPS 4                    /* guest source ports with a transport socket */
#endif
#ifndef VBBA_CT
#define VBBA_CT 16                     /* conntrack entries (map, remote ip, remote port) */
#endif
#ifndef VBBA_ALLOW_MAX
#define VBBA_ALLOW_MAX 8
#endif
#ifndef VBBA_LOCALQ
#define VBBA_LOCALQ 4                  /* router-generated frames waiting for the ring (ARP, DHCP, ICMP replies) */
#endif
#ifndef VBBA_HOLD
#define VBBA_HOLD 1                    /* keep one datagram while its socket is still opening (0: drop it) */
#endif
#define VBBA_LOCAL_FRAME 384u          /* largest router-generated frame (a DHCP reply is 342 B) */
#define VBBA_DHCP_PEND 320u            /* largest DHCP message (UDP payload) kept while the transport has no address yet (our guest sends 300 B) */
#define VBBA_UDP_MAX 1472u             /* largest datagram payload either way (1500 MTU) */

#define VBBA_RF_MIRROR 0x01u           /* lease = the transport's own address */
#define VBBA_RF_ICMP 0x02u             /* answer echo requests to the router address */
#define VBBA_RF_OPEN_INBOUND 0x04u     /* accept datagrams from allow-listed peers before the guest sent to them */
#define VBBA_RF_CT_ANY_PORT 0x08u      /* conntrack matches the remote address only (peers behind NAT) */

struct vbba_allow_rule {
    vbba_u32 ip, mask;                 /* destination (ip & mask) == (rule.ip & rule.mask) */
    vbba_u16 port_lo, port_hi;         /* inclusive */
};

struct vbba_router_cfg {
    vbba_u32 flags;
    vbba_u32 guest_ip, mask, gw;       /* private mode lease; MIRROR: mask/gw used when the transport has none */
    vbba_u32 lease_s;                  /* lease time (default 86400) */
    vbba_u8 router_mac[6];
    vbba_u8 nallow;
    struct vbba_allow_rule allow[VBBA_ALLOW_MAX];
    vbba_u32 ct_ms;                    /* conntrack lifetime (default 120000) */
    vbba_u32 tx_per_s;                 /* guest datagrams per second (default 1000) */
    vbba_u32 arp_per_s;                /* ARP + DHCP + ICMP answers per second (default 50) */
    vbba_u32 dns;                      /* DHCP option 6 (resolver handed to the guest); 0 = none (no option 6 is sent) */
};

struct vbba_router_stats {
    vbba_u32 tx_frames, tx_udp, tx_denied, tx_bad, tx_bad_csum, tx_frag, tx_tcp, tx_proto, tx_ethertype, tx_spoof;
    vbba_u32 tx_bcast, tx_opening, tx_held, tx_hold_sent, tx_hold_drop, tx_nomap, tx_tp_busy, tx_tp_err, tx_rate;
    vbba_u32 tx_hold_busy;               /* datagram to an opening socket dropped: the one hold slot belonged to another socket */
    vbba_u32 arp_req, arp_replies, arp_ignored, local_rate, localq_drop;
    vbba_u32 dhcp_disc, dhcp_req, dhcp_offer, dhcp_ack, dhcp_nak, dhcp_wait_ip, dhcp_bad;
    vbba_u32 dhcp_deferred;              /* DISCOVERs answered late: kept until the transport had an address, then offered at once */
    vbba_u32 icmp_replies, icmp_drop;
    vbba_u32 rx_dgrams, rx_unsolicited, rx_trunc, rx_err, rx_no_lease;
    vbba_u32 unsol_ip, unsol_n;  vbba_u16 unsol_port;        /* the source of the newest datagram dropped as unsolicited (peer connection debugging,) */
    vbba_u32 maps_opened, maps_evicted, maps_failed;
    vbba_u32 tx_dns;                     /* guest UDP datagrams to port 53 (counted before any allow/deny decision; diagnostic) */
    vbba_u32 tx_f_arp, tx_f_udp, tx_f_ip_other, tx_f_nonip;   /* (diagnostic): every frame the guest transmitted, by type (before any validation) */
};

/* (diagnostic): the first VBBA_TXHDR distinct frame headers the guest transmitted (key: ethertype, ip protocol, destination ip, destination port) */
#define VBBA_TXHDR 8
struct vbba_txhdr { vbba_u16 eth, sport, dport, len; vbba_u8 proto, op; vbba_u32 dip; vbba_u32 n; };   /* n: how many frames carried this header (test builds print it) */
#define VBBA_RXSRC 6
struct vbba_rxsrc { vbba_u32 ip, n; vbba_u16 port; };                                  /* source address of delivered datagrams, with a count (test builds print it) */

struct vbba_map { vbba_u8 used; vbba_u16 gport; int h; vbba_u32 last; };
struct vbba_ct { vbba_u8 used, map; vbba_u16 port; vbba_u32 ip, last; };

struct vbba_router {
    struct vbba_router_cfg cfg;
    const struct vbba_transport *tp;
    struct vbba_frame_backend be;
    vbba_u8 guest_mac[6], have_mac;
    vbba_u32 lease_ip, router_ip, mask;    /* current view (refreshed from cfg / transport) */
    vbba_u32 leased;                       /* address the guest was ACKed (0 = none yet) */
    vbba_u16 ip_id;
    vbba_u8 rr;
    struct vbba_map map[VBBA_MAPS];
    struct vbba_ct ct[VBBA_CT];
    vbba_u32 win_ms, win_tx, win_local;
    vbba_u8 dh_pend;                       /* a DISCOVER is waiting for the transport's address (newest wins) */
    vbba_u16 dh_len;
    vbba_u32 dh_t;
    vbba_u8 dh_msg[VBBA_DHCP_PEND];
    vbba_u8 lq_head, lq_cnt;
    vbba_u16 lq_len[VBBA_LOCALQ];
    vbba_u8 lq[VBBA_LOCALQ][VBBA_LOCAL_FRAME];
#if VBBA_HOLD
    vbba_u8 hold_used, hold_map;
    vbba_u16 hold_port, hold_len;
    vbba_u32 hold_ip, hold_t;
    vbba_u8 hold[VBBA_UDP_MAX];
#endif
    struct vbba_txhdr txhdr[VBBA_TXHDR];
    vbba_u32 txhdr_n;
    struct vbba_rxsrc rxsrc[VBBA_RXSRC];
    vbba_u32 rxsrc_n;
    struct vbba_router_stats st;
#if VBBA_TCP
    struct vbba_tcp tcp;
#endif
};

void vbba_router_init(struct vbba_router *r, const struct vbba_router_cfg *cfg, const struct vbba_transport *tp);
const struct vbba_frame_backend *vbba_router_backend(struct vbba_router *r);
int vbba_router_allowed(const struct vbba_router *r, vbba_u32 ip, vbba_u16 port);

/* helpers shared with tests */
vbba_u16 vbba_csum(const vbba_u8 *p, vbba_u32 n, vbba_u32 sum);

#endif
