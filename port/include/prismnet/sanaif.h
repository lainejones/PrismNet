/*
 * PrismNet - lwIP network interface over a SANA-II device: Ethernet, or a
 * point-to-point link (SLIP, CSLIP, PPP, PLIP) that carries bare IP.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_SANAIF_H
#define PRISMNET_SANAIF_H

#include <exec/types.h>
#include <exec/ports.h>
#include "lwip/netif.h"
#include "prismnet/sana2defs.h"

#ifndef SANAIF_NUM_RX_IP
#ifdef PN_SMALL
#define SANAIF_NUM_RX_IP   8    /* PN_SMALL: a 6-segment window */
#else
#define SANAIF_NUM_RX_IP   32   /* a 24-segment window arrives as one burst */
#endif
#endif
#define SANAIF_NUM_RX_ARP  2
#define SANAIF_NUM_RX      (SANAIF_NUM_RX_IP + SANAIF_NUM_RX_ARP)
#ifdef PN_SMALL
#define SANAIF_NUM_TX      6
#define SANAIF_TXQ         8
#else
#define SANAIF_NUM_TX      16
#define SANAIF_TXQ         32   /* frames waiting for a free write request */
#endif

struct sanaif_req {
    struct IOSana2Req io;
    struct pbuf      *p;       /* rx: buffer being filled; tx: frame being sent */
    BOOL              busy;    /* request is with the driver */
    BOOL              lost;    /* the driver never gave it back (watchdog) */
    BOOL              defer;   /* rx: the read failed; requeue on a timed pass only */
    u32_t             seen;    /* tx: sys_now() the watchdog first saw it busy */
};

struct sanaif_stats {
    ULONG rx_packets, rx_bytes, rx_dropped, rx_errors;
    ULONG tx_packets, tx_bytes, tx_dropped, tx_errors;
    ULONG rx_starved;   /* polls that found every IP read completed (PROFILE=1) */
    ULONG resets;       /* watchdog resets of a stalled driver */
};

struct sanaif {
    struct netif        netif;
    char                devname[64];
    ULONG               unit;
    struct MsgPort     *port;
    struct IOSana2Req  *ctl;            /* the OpenDevice() request */
    BOOL                dev_open;
    BOOL                ctl_lost;         /* the driver kept ctl (reset_device): never reuse it */
    ULONG               mtu;
    ULONG               bps;
    UBYTE               mac[6];
    struct sanaif_req   rx[SANAIF_NUM_RX];
    struct sanaif_req   tx[SANAIF_NUM_TX];
    /* Frames lwIP sent while every write was still with the driver (a slow
     * card, e.g. WinUAE's NE2000): kept here, in order, instead of dropped -
     * a dropped ACK can leave the far end waiting for a retransmit timeout */
    struct pbuf        *txq[SANAIF_TXQ];
    UBYTE               txq_head, txq_n;
    struct sanaif_stats stats;
    ULONG               open_wire_error;  /* ios2_WireError if OpenDevice failed */
    ULONG               stall_ms;         /* watchdog: current stall limit */
    ULONG               stall_base;       /* ... its starting value (STALL= in the interface file, 0 = default) */
    LONG                mcast_err;        /* last S2_ADD/DELMULTICASTADDRESS error, 0 = fine */
    u32_t               tx_progress;      /* sys_now() of the last completed write */
    BOOL                no_s2mcast;       /* driver lacks S2_MULTICAST: use CMD_WRITE */
    BOOL                p2p;              /* point-to-point: bare IP, no ARP, no multicast */
    ULONG               hwtype;           /* Sana2DeviceQuery.HardwareType */
    UWORD               addrbits;         /* station address size in bits */
    struct serslip     *ser;              /* SLIP over serial.device (serslip.c), else NULL */
};

/*
 * Open the SANA-II device and add it to lwIP: an Ethernet netif, or for a
 * SLIP/PPP/PLIP driver a point-to-point one (s->p2p).
 * ip/netmask/gw may be NULL (all zero, e.g. for DHCP).
 * On a point-to-point link with no address given, a driver whose station
 * address is an IPv4 address (32 bits: rhslip, PPP after IPCP) supplies it.
 * Returns 0 on success, otherwise an OpenDevice/SANA-II error code
 * (negative = PrismNet-internal failure); `why` gets a short text.
 */
LONG  sanaif_open(struct sanaif *s, const char *devname, ULONG unit,
                  const ip4_addr_t *ip, const ip4_addr_t *netmask,
                  const ip4_addr_t *gw, const char **why);

/* Signal bit(s) to Wait() on for this interface. */
ULONG sanaif_sigmask(struct sanaif *s);

/* Process completed reads/writes; feeds received frames into lwIP.
 * Call from the lwIP task only. */
void  sanaif_poll(struct sanaif *s);

/* Call now and then (every few hundred ms) with sys_now(). A write the
 * driver has held for SANAIF_TX_STALL_MS means it has stalled (seen with
 * cnet.device on WinUAE's NE2000): abort all I/O and take the device offline
 * and online again. Returns TRUE if it did. Each reset doubles the limit (up
 * to SANAIF_TX_STALL_MAX_MS), so a driver that legitimately holds writes is
 * not cycled every 2 s; a write completing normally resets it. */
#define SANAIF_TX_STALL_MS     2000
#define SANAIF_TX_STALL_MAX_MS 8000   /* writes that keep completing never trip it */
BOOL  sanaif_watchdog(struct sanaif *s, u32_t now);

/* Abort all I/O, remove the netif and close the device. */
void  sanaif_close(struct sanaif *s);
BOOL  sanaif_requests_lost(struct sanaif *s);   /* after close: memory still in use? */

/* SLIP straight over a serial port (serial.device or compatible), no
 * SANA-II driver needed.
 * *s like sanaif_open; sanaif_poll/watchdog/close then hand over to it. */
LONG  serslip_open(struct sanaif *s, const char *devname, ULONG unit, ULONG baud,
                   BOOL rtscts, ULONG mtu, const ip4_addr_t *ip,
                   const ip4_addr_t *netmask, const ip4_addr_t *gw, const char **why);
void  serslip_poll(struct sanaif *s);
void  serslip_close(struct sanaif *s);

#endif
