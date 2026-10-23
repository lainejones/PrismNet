/*
 * PrismNet - lwIP configuration.
 * GPL-3.0-only, see LICENSE.
 *
 * lwIP runs NO_SYS=1 inside a single task (see docs/DESIGN.md); PrismNet
 * implements BSD-socket semantics itself, so lwIP's netconn/socket layers
 * are disabled.
 */
#ifndef PRISMNET_LWIPOPTS_H
#define PRISMNET_LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_TIMERS                 1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define SYS_LIGHTWEIGHT_PROT        0

/* ---- memory -------------------------------------------------------- */
/* Two profiles. The default suits accelerated machines (~500 KB in use with
 * the stack). PN_SMALL (make small -> out/small/PrismNet) is for a 68000
 * with 1 MB: a 6-segment window, 20 packet buffers, a 20 KB heap and smaller
 * tables, about half the memory. A 7 MHz CPU cannot fill a bigger window. */
#define MEM_ALIGNMENT               4
#define ETH_PAD_SIZE                2   /* IP header longword-aligned (see sanaif.c) */
/* Nothing is reserved: the heap and every pool element come from pnmem.c at
 * run time, within a budget (ENV:PrismNet/startup MEMORY=<KB>). The counts
 * below remain the limits; MEM_SIZE is no longer used for the heap. */
#define MEM_CUSTOM_ALLOCATOR        1
#define MEM_CUSTOM_MALLOC           pn_mem_malloc
#define MEM_CUSTOM_FREE             pn_mem_free
#define MEM_CUSTOM_CALLOC           pn_mem_calloc
#define MEMP_MEM_MALLOC             1
#define MEMP_STATS                  1   /* lwIP turns it off in malloc mode; the limits need it */
#include "prismnet/pnmem.h"
#ifdef PN_SMALL
#define MEM_SIZE                    (20 * 1024)
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_UDP_PCB            10
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_PCB_LISTEN     6
#define MEMP_NUM_TCP_SEG            24
#else
#define MEM_SIZE                    (96 * 1024)
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_RAW_PCB            8
#define MEMP_NUM_UDP_PCB            16
#define MEMP_NUM_TCP_PCB            64   /* a limit only: allocated when used */
#define MEMP_NUM_TCP_PCB_LISTEN     16
#define MEMP_NUM_TCP_SEG            64
#endif
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 16)   /* + mDNS probes */
/* One pool pbuf holds a complete Ethernet frame (14 + 1500 + slack), so a
 * SANA-II read can copy straight into a single pbuf. Every queued driver
 * read holds one, so the pool must exceed SANAIF_NUM_RX plus the window. */
#ifndef PBUF_POOL_SIZE
#ifdef PN_SMALL
#define PBUF_POOL_SIZE              20   /* 10 driver reads + received data */
#else
#define PBUF_POOL_SIZE              96   /* 34 driver reads + received data */
#endif
#endif
#define PBUF_POOL_BUFSIZE           1536

/* ---- protocols ----------------------------------------------------- */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#ifdef PN_SMALL
#define LWIP_IGMP                   0   /* no mDNS in the small build: see below */
#else
#define LWIP_IGMP                   1   /* multicast, for mDNS */
#endif
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             3
#ifdef PN_SMALL
#define DNS_TABLE_SIZE              6
#define DNS_MAX_REQUESTS            6
#else
#define DNS_TABLE_SIZE              16  /* one per resolver slot, else ERR_MEM retries */
#define DNS_MAX_REQUESTS            16  /* concurrent lookups (browsers fan out) */
#endif
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define IP_REASSEMBLY               1
#define IP_FRAG                     1

/* ---- TCP ----------------------------------------------------------- */
#define TCP_MSS                     1460
#ifndef PN_WND_MSS
#ifdef PN_SMALL
#define PN_WND_MSS 6
#else
#define PN_WND_MSS 24
#endif
#endif
#ifndef PN_ACK_EVERY
#define PN_ACK_EVERY 8      /* ACK every 8 full segments, or at once on PSH (tcp_in.c) */
#endif
#define TCP_WND                     (PN_WND_MSS * TCP_MSS)   /* per socket */
/* Send buffer: 24 segments took the A4000's upload from 952 to 1254 KB/s
 * (8 -> 16 gave most of it, 32 no more); the receive window stays at 24
 * (44 cost 20 % of download). See docs/PERFORMANCE.md. */
#ifndef PN_SND_MSS
#ifdef PN_SMALL
#define PN_SND_MSS 4
#else
#define PN_SND_MSS 24
#endif
#endif
#define TCP_SND_BUF                 (PN_SND_MSS * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
/* lwIP's oversize optimisation corrupts memory after retransmissions (its
 * spare-room count is only resynced when TCP_OVERSIZE_DBGCHECK is on, which
 * needs LWIP_DEBUG). Slow or lossy links (SLIP, PPP) retransmit all the time
 * and crashed within seconds; segments are filled by chaining instead. */
#define TCP_OVERSIZE                0
#define LWIP_TCP_KEEPALIVE          1
/* A SYN past a listener's backlog is dropped (the client sends it again), as
 * on BSD, instead of the connection being reset. Connections waiting in
 * PrismNet's own accept queue count too (tcp_backlog_delayed in csock.c). */
#define TCP_LISTEN_BACKLOG          1
#define SO_REUSE                    1   /* SO_REUSEADDR */

/* ---- loopback (127.0.0.1) --------------------------------------------- */
#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_LOOPBACK         1
#ifdef PN_SMALL
#define LWIP_LOOPBACK_MAX_PBUFS     8
#else
#define LWIP_LOOPBACK_MAX_PBUFS     16
#endif

/* ---- name services: the computer name on the LAN ------------------- */
/* NetBIOS (Windows: ping NAME, \\NAME) and mDNS (name.local). */
#define LWIP_NETBIOS_RESPOND_NAME_QUERY 1   /* also nbtstat -A node status */
#ifdef PN_SMALL
#define LWIP_MDNS_RESPONDER         0   /* 17 KB of code and IGMP: the 1 MB machines keep NetBIOS only */
#else
#define LWIP_MDNS_RESPONDER         1
#endif
#define LWIP_MDNS_SEARCH            0
#define MDNS_MAX_SERVICES           1
#define LWIP_NUM_NETIF_CLIENT_DATA  1       /* mDNS keeps its state per netif */
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1    /* mDNS re-announces on DHCP changes */

/* ---- time from the network (SNTP): ENV:PrismNet/time, PrismNet SETCLOCK -- */
#define LWIP_DHCP_GET_NTP_SRV       1       /* DHCP option 42 */
#define LWIP_DHCP_MAX_NTP_SERVERS   1
#define SNTP_MAX_SERVERS            2       /* [0] from DHCP, [1] the configured name */
#define SNTP_SERVER_DNS             1
#define SNTP_STARTUP_DELAY          0       /* SETCLOCK wants the request at once */
#define SNTP_CHECK_RESPONSE         1       /* the answer must come from the server asked */
#define SNTP_SET_SYSTEM_TIME_US(sec, us) pn_sntp_set(sec, us)
void pn_sntp_set(unsigned long sec, unsigned long us);   /* core/core.c */

/* ---- hooks ----------------------------------------------------------- */
/* Unpredictable initial sequence numbers (lwIP's default is a plain counter). */
#define LWIP_HOOK_FILENAME          "prismnet/lwiphooks.h"
#define LWIP_HOOK_TCP_ISN(lip, lport, rip, rport) prismnet_tcp_isn(lip, lport, rip, rport)
/* Static routes from ENV:PrismNet/routes (ROUTE lines). */
#define LWIP_HOOK_IP4_ROUTE(dest)               pn_route_netif(dest)
#define LWIP_HOOK_ETHARP_GET_GW(netif, dest)    pn_route_gw(netif, dest)

/* bsdsocket code uses the NDK's htons/htonl; keep lwIP's names prefixed */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1

/* ---- checksums: in software, the sum itself in 68000 asm (port/chksum.S) ---- */
unsigned short pn_chksum(const void *dataptr, int len);
#ifdef PN_PROFILE
unsigned short pn_chksum_prof(const void *dataptr, int len);
#define LWIP_CHKSUM                 pn_chksum_prof
#else
#define LWIP_CHKSUM                 pn_chksum
#endif

/* ---- stats / debug ------------------------------------------------- */
#define LWIP_STATS                  1
#define LWIP_STATS_LARGE            1   /* 32-bit counters (Roadshow statistics) */
#define MIB2_STATS                  1   /* the counters GetNetworkStatistics reports */
#define LWIP_STATS_DISPLAY          0
#ifdef PRISMNET_DEBUG
#define LWIP_DEBUG                  1
#define ETHARP_DEBUG                LWIP_DBG_ON
#define DHCP_DEBUG                  LWIP_DBG_ON
#define DNS_DEBUG                   LWIP_DBG_ON
#endif

#ifdef PN_MEMCHECK                     /* test build: catch buffer overruns */
#define MEMP_OVERFLOW_CHECK         2
#define MEM_OVERFLOW_CHECK          2
#define MEMP_SANITY_CHECK           1
#define MEM_SANITY_CHECK            1
#endif

#endif /* PRISMNET_LWIPOPTS_H */
