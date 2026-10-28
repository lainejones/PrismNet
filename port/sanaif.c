/*
 * PrismNet - lwIP network interface over a SANA-II device.
 * GPL-3.0-only, see LICENSE.
 *
 * Point-to-point drivers (SLIP, CSLIP, PPP, PLIP) carry bare IP packets:
 * no ARP, no link addresses. They get a netif without NETIF_FLAG_ETHARP
 * whose output hands IP packets straight to the driver. Every pbuf still
 * keeps LINK_ROOM bytes in front of the packet so the buffer hooks below are
 * the same for both kinds.
 *
 * The device is used in cooked (non-raw) mode, which every SANA-II driver
 * supports: on transmit the Ethernet header lwIP built is translated into
 * ios2_DstAddr/ios2_PacketType, on receive it is rebuilt from
 * ios2_SrcAddr/ios2_DstAddr/ios2_PacketType. Reads are queued per ethertype.
 *
 * Buffer management: the driver calls our S2_CopyToBuff/S2_CopyFromBuff
 * hooks, possibly from its own task or an interrupt. The hooks only copy
 * bytes; every lwIP call happens on the lwIP task in sanaif_poll() and
 * sanaif_linkoutput().
 */
#include <string.h>

#include <exec/types.h>
#include <exec/errors.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#if LWIP_IGMP
#include "lwip/igmp.h"
#endif
#include "lwip/def.h"
#include "lwip/ip.h"
#include "netif/ethernet.h"

#include "prismnet/sanaif.h"

#ifdef PN_PROFILE
#include <devices/timer.h>
#include <proto/timer.h>
ULONG sprof_in, sprof_inn, sprof_rq, sprof_rqn, sprof_tx, sprof_txn;
static ULONG sprof_now(void) { struct EClockVal ev; ReadEClock(&ev); return ev.ev_lo; }
#define SPROF_T(v) ULONG v = sprof_now()
ULONG sprof_ck, sprof_ckn, sprof_ckb;
unsigned short pn_chksum(const void *dataptr, int len);
unsigned short pn_chksum_prof(const void *dataptr, int len)
{
    ULONG t0 = sprof_now();
    unsigned short r = pn_chksum(dataptr, len);
    sprof_ck += sprof_now() - t0; sprof_ckn++; sprof_ckb += len;
    return r;
}
#define SPROF_ADD(acc, cnt, t0) do { acc += sprof_now() - (t0); cnt++; } while (0)
#else
#define SPROF_T(v)
#define SPROF_ADD(acc, cnt, t0)
#endif

#ifdef PN_PKTLOG
/* Diagnostic build only (make pktlog): one log line per frame. */
void core_log(const char *fmt, ...);
static int pktlog_n;
static void pktlog(const char *dir, const UBYTE *f, ULONG len)
{
    UWORD type;
    if (pktlog_n >= 120 || len < 14) return;
    pktlog_n++;
    type = (f[12] << 8) | f[13];
    if (type == 0x0806 && len >= 42) {
        core_log("%s ARP op %u %u.%u.%u.%u -> who %u.%u.%u.%u", dir, (unsigned)((f[20] << 8) | f[21]),
                 f[28], f[29], f[30], f[31], f[38], f[39], f[40], f[41]);
    } else if (type == 0x0800 && len >= 34) {
        const UBYTE *ip = f + 14, *l4 = ip + (ip[0] & 15) * 4;
        if (ip[9] == 6 && len >= (ULONG)(l4 - f) + 14)
            core_log("%s TCP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u flags %02x len %lu", dir,
                     ip[12], ip[13], ip[14], ip[15], (unsigned)((l4[0] << 8) | l4[1]),
                     ip[16], ip[17], ip[18], ip[19], (unsigned)((l4[2] << 8) | l4[3]),
                     l4[13], (unsigned long)len);
        else
            core_log("%s IP proto %u %u.%u.%u.%u -> %u.%u.%u.%u len %lu", dir, ip[9],
                     ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19], (unsigned long)len);
    } else {
        core_log("%s type %04x len %lu dst %02x:%02x:%02x:%02x:%02x:%02x", dir, type, (unsigned long)len,
                 f[0], f[1], f[2], f[3], f[4], f[5]);
    }
}
#define PKTLOG(d, f, l) pktlog(d, f, l)
#else
#define PKTLOG(d, f, l)
#endif

#define ETHTYPE_IP_   0x0800
#define ETHTYPE_ARP_  0x0806
#define ETH_HDR_LEN   14
/* Room in front of every packet: the Ethernet header plus lwIP's ETH_PAD_SIZE
 * (2), which puts the IP header, TCP header and data on longword boundaries -
 * a misaligned longword costs a 68020/030 two bus cycles. */
#define LINK_ROOM     (ETH_HDR_LEN + ETH_PAD_SIZE)

/* ---- buffer-management hooks (called by the driver) ----------------- */

/* Driver -> us: received packet data (cooked: no Ethernet header).
 * `to` is the ios2_Data cookie, i.e. the pbuf we queued with the read. */
static ULONG copy_to_buff(register APTR to __asm("a0"),
                          register APTR from __asm("a1"),
                          register ULONG n __asm("d0"))
{
    struct pbuf *p = (struct pbuf *)to;

    if (n > (ULONG)(p->len - LINK_ROOM))
        return FALSE;
    memcpy((UBYTE *)p->payload + LINK_ROOM, from, n);
    return TRUE;
}

/* Us -> driver: packet data to send (cooked: skip our Ethernet header).
 * `from` is the ios2_Data cookie, i.e. the pbuf chain being sent. */
static ULONG copy_from_buff(register APTR to __asm("a0"),
                            register APTR from __asm("a1"),
                            register ULONG n __asm("d0"))
{
    struct pbuf *p = (struct pbuf *)from;

    return pbuf_copy_partial(p, to, (u16_t)n, LINK_ROOM) == n;
}

/* SANA-II R3 direct buffers: the driver reads or writes the packet in place
 * and skips its own copy. Driver -> us: a receive pbuf is one pool buffer, and
 * copy_to_buff above writes at payload + LINK_ROOM, which is longword aligned;
 * the room there (PBUF_POOL_BUFSIZE - LINK_ROOM) holds any Ethernet frame. */
static APTR dma_to_buff(register APTR cookie __asm("a0"))
{
    struct pbuf *p = (struct pbuf *)cookie;

    if (p->len < LINK_ROOM + 1514 || ((ULONG)p->payload + LINK_ROOM) & 3)
        return NULL;
    return (UBYTE *)p->payload + LINK_ROOM;
}

/* Us -> driver: only when the packet is one contiguous, longword-aligned
 * block; otherwise NULL and the driver uses copy_from_buff. */
static APTR dma_from_buff(register APTR cookie __asm("a0"))
{
    struct pbuf *p = (struct pbuf *)cookie;

    if (p->next != NULL || p->len < LINK_ROOM || ((ULONG)p->payload + LINK_ROOM) & 3)
        return NULL;
    return (UBYTE *)p->payload + LINK_ROOM;
}

static struct TagItem buffer_tags[] = {
    { S2_CopyToBuff,   (ULONG)copy_to_buff },
    { S2_CopyFromBuff, (ULONG)copy_from_buff },
    { S2_DMACopyToBuff32,   (ULONG)dma_to_buff },
    { S2_DMACopyFromBuff32, (ULONG)dma_from_buff },
    { TAG_DONE,        0 }
};

/* ---- helpers --------------------------------------------------------- */

static struct sanaif_req *free_tx(struct sanaif *s)
{
    int i;
    for (i = 0; i < SANAIF_NUM_TX; i++)
        if (!s->tx[i].busy && !s->tx[i].lost)
            return &s->tx[i];
    return NULL;
}

static ULONG rx_type(struct sanaif *s, struct sanaif_req *r)
{
    return (r - s->rx) < SANAIF_NUM_RX_IP ? ETHTYPE_IP_ : ETHTYPE_ARP_;
}

static void queue_read(struct sanaif *s, struct sanaif_req *r)
{
    if (!r->p) {
        r->p = pbuf_alloc(PBUF_RAW, PBUF_POOL_BUFSIZE, PBUF_POOL);
        if (!r->p)
            return;     /* out of pbufs; retried on the next poll */
    }
    r->io.ios2_Req.io_Command = CMD_READ;
    r->io.ios2_Req.io_Flags   = 0;
    r->io.ios2_PacketType     = s->p2p ? ETHTYPE_IP_ : rx_type(s, r);
    r->io.ios2_Data           = r->p;
    r->busy = TRUE;
    {
        SPROF_T(t0);
        SendIO((struct IORequest *)&r->io);
        SPROF_ADD(sprof_rq, sprof_rqn, t0);
    }
}

static void rx_done(struct sanaif *s, struct sanaif_req *r)
{
    struct IOSana2Req *io = &r->io;
    struct pbuf *p;
    UBYTE *h;
    ULONG len;

    r->busy = FALSE;
    if (io->ios2_Req.io_Error != 0) {
        if (io->ios2_Req.io_Error != IOERR_ABORTED) {
            /* A unit that is offline fails reads at once; requeuing now
             * would spin the core (priority 3) until it comes back. */
            s->stats.rx_errors++;
            r->defer = TRUE;
        }
        return;         /* keep r->p, requeue */
    }

    p    = r->p;
    r->p = NULL;
    len  = io->ios2_DataLength;
    if (len > (ULONG)(p->len - LINK_ROOM)) {
        s->stats.rx_dropped++;
        pbuf_free(p);
        return;
    }

    if (s->p2p) {                       /* bare IP: drop the spare header room */
        pbuf_realloc(p, (u16_t)(LINK_ROOM + len));
        pbuf_remove_header(p, LINK_ROOM);
        s->stats.rx_packets++;
        s->stats.rx_bytes += len;
        if (s->netif.input(p, &s->netif) != ERR_OK)
            pbuf_free(p);
        return;
    }
    h = (UBYTE *)p->payload + ETH_PAD_SIZE;
    if (io->ios2_Req.io_Flags & SANA2IOF_BCAST)
        memset(h, 0xff, 6);
    else if (io->ios2_Req.io_Flags & SANA2IOF_MCAST)
        memcpy(h, io->ios2_DstAddr, 6);
    else
        memcpy(h, s->mac, 6);
    memcpy(h + 6, io->ios2_SrcAddr, 6);
    h[12] = (UBYTE)(io->ios2_PacketType >> 8);
    h[13] = (UBYTE)io->ios2_PacketType;
    pbuf_realloc(p, (u16_t)(LINK_ROOM + len));

    s->stats.rx_packets++;
    s->stats.rx_bytes += len;
    PKTLOG("rx", (const UBYTE *)p->payload + ETH_PAD_SIZE, p->tot_len - ETH_PAD_SIZE);
    {
        SPROF_T(t0);
        if (s->netif.input(p, &s->netif) != ERR_OK)
            pbuf_free(p);
        SPROF_ADD(sprof_in, sprof_inn, t0);
    }
}

static void tx_done(struct sanaif *s, struct sanaif_req *r)
{
    s->tx_progress = sys_now();         /* the driver is moving, even if this one failed */
    if (r->io.ios2_Req.io_Error != 0) {
        /* A driver without S2_MULTICAST answers IOERR_NOCMD, or (scsidayna 1.5)
         * S2ERR_NOT_SUPPORTED: from then on multicast goes out as a plain write,
         * which every Ethernet driver sends to whatever address it is given. */
        if (r->io.ios2_Req.io_Command == S2_MULTICAST &&
            (r->io.ios2_Req.io_Error == IOERR_NOCMD || r->io.ios2_Req.io_Error == S2ERR_NOT_SUPPORTED))
            s->no_s2mcast = TRUE;
        s->stats.tx_errors++;
    } else {
        s->stall_ms = s->stall_base ? s->stall_base : SANAIF_TX_STALL_MS;
        s->stats.tx_packets++;
        s->stats.tx_bytes += r->io.ios2_DataLength;
    }
    pbuf_free(r->p);
    r->p    = NULL;
    r->busy = FALSE;
    r->lost = FALSE;                    /* a write the driver kept past a reset came back */
}

static void drain_txq(struct sanaif *s);

/* ---- lwIP netif callbacks --------------------------------------------- */

/* hand one Ethernet frame to a free write request (it takes its own ref) */
static void start_tx(struct sanaif *s, struct sanaif_req *r, struct pbuf *p)
{
    const UBYTE *h = (const UBYTE *)p->payload + ETH_PAD_SIZE;

    r->io.ios2_Req.io_Flags = 0;
    if ((h[0] & h[1] & h[2] & h[3] & h[4] & h[5]) == 0xff)
        r->io.ios2_Req.io_Command = S2_BROADCAST;
    else if ((h[0] & 1) && !s->no_s2mcast)      /* group address: mDNS, IGMP */
        r->io.ios2_Req.io_Command = S2_MULTICAST;
    else
        r->io.ios2_Req.io_Command = CMD_WRITE;
    memcpy(r->io.ios2_DstAddr, h, 6);
    r->io.ios2_PacketType = ((ULONG)h[12] << 8) | h[13];
    r->io.ios2_DataLength = p->tot_len - LINK_ROOM;
    r->io.ios2_Data       = p;

    PKTLOG("tx", (const UBYTE *)p->payload + ETH_PAD_SIZE, p->tot_len - ETH_PAD_SIZE);
    pbuf_ref(p);        /* held until the driver has sent it (tx_done) */
    r->p    = p;
    r->busy = TRUE;
    r->seen = 0;
    {
        SPROF_T(t0);
        SendIO((struct IORequest *)&r->io);
        SPROF_ADD(sprof_tx, sprof_txn, t0);
    }
}

static void p2p_start_tx(struct sanaif *s, struct sanaif_req *r, struct pbuf *q);

/* send queued frames while there are free write requests */
static void drain_txq(struct sanaif *s)
{
    struct sanaif_req *r;

    while (s->txq_n && (r = free_tx(s)) != NULL) {
        struct pbuf *p = s->txq[s->txq_head];
        s->txq[s->txq_head] = NULL;
        s->txq_head = (UBYTE)((s->txq_head + 1) % SANAIF_TXQ);
        s->txq_n--;
        if (s->p2p) {
            p2p_start_tx(s, r, p);      /* our private copy: the write owns it now */
        } else {
            start_tx(s, r, p);
            pbuf_free(p);               /* the queue's reference */
        }
    }
}

static err_t sanaif_linkoutput(struct netif *netif, struct pbuf *p)
{
    struct sanaif *s = (struct sanaif *)netif->state;
    struct sanaif_req *r;

    if (p->len < LINK_ROOM)
        return ERR_ARG;
    if (!s->txq_n && (r = free_tx(s)) != NULL) {
        start_tx(s, r, p);
        return ERR_OK;
    }
    if (s->txq_n < SANAIF_TXQ) {        /* behind the others, keeps the order */
        pbuf_ref(p);
        s->txq[(s->txq_head + s->txq_n) % SANAIF_TXQ] = p;
        s->txq_n++;
        drain_txq(s);
        return ERR_OK;
    }
    s->stats.tx_dropped++;
    return ERR_MEM;
}

/* Point-to-point output: lwIP hands us an IP packet; the next hop does not
 * matter, everything goes to the other end of the link. The packet is sent
 * with 14 bytes of header room in front, like an Ethernet frame, so
 * copy_from_buff works unchanged. */
static err_t sanaif_p2p_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    struct sanaif *s = (struct sanaif *)netif->state;
    struct sanaif_req *r;
    struct pbuf *q;

    (void)ipaddr;
    /* Behind the others when frames are already waiting (keeps the order),
     * and while the queue has room: a slow serial link takes a TCP burst
     * there instead of losing it (every lost segment cost a retransmit). */
    if ((s->txq_n || !(r = free_tx(s))) && s->txq_n >= SANAIF_TXQ) {
        s->stats.tx_dropped++;
        return ERR_MEM;
    }
    /* Always a private copy: lwIP's pbuf stays lwIP's (TCP may retransmit or
     * free it while the slow link still holds the write). */
    q = pbuf_alloc(PBUF_RAW, (u16_t)(p->tot_len + LINK_ROOM), PBUF_RAM);
    if (!q) {
        s->stats.tx_dropped++;
        return ERR_MEM;
    }
    pbuf_copy_partial(p, (UBYTE *)q->payload + LINK_ROOM, p->tot_len, 0);
    if (s->txq_n || !(r = free_tx(s))) {
        s->txq[(s->txq_head + s->txq_n) % SANAIF_TXQ] = q;
        s->txq_n++;
        return ERR_OK;
    }
    p2p_start_tx(s, r, q);
    return ERR_OK;
}

static void p2p_start_tx(struct sanaif *s, struct sanaif_req *r, struct pbuf *q)
{
    r->io.ios2_Req.io_Command = CMD_WRITE;
    r->io.ios2_Req.io_Flags   = 0;
    memset(r->io.ios2_DstAddr, 0, sizeof(r->io.ios2_DstAddr));
    r->io.ios2_PacketType = ETHTYPE_IP_;
    r->io.ios2_DataLength = q->tot_len - LINK_ROOM;
    r->io.ios2_Data       = q;
    r->p    = q;
    r->busy = TRUE;
    r->seen = 0;
    SendIO((struct IORequest *)&r->io);
}

static LONG ctl_cmd(struct sanaif *s, UWORD cmd);

#if LWIP_IGMP
/* lwIP joins/leaves an IPv4 multicast group (IGMP, mDNS): tell the driver
 * to pass the matching Ethernet address 01:00:5e:xx:xx:xx up to us. */
static err_t sanaif_igmp_filter(struct netif *netif, const ip4_addr_t *group,
                                enum netif_mac_filter_action action)
{
    struct sanaif *s = (struct sanaif *)netif->state;
    ULONG a = lwip_ntohl(ip4_addr_get_u32(group));
    LONG err;

    if (!s->dev_open || s->ctl_lost)
        return ERR_IF;
    memset(s->ctl->ios2_SrcAddr, 0, sizeof(s->ctl->ios2_SrcAddr));
    s->ctl->ios2_SrcAddr[0] = 0x01;
    s->ctl->ios2_SrcAddr[1] = 0x00;
    s->ctl->ios2_SrcAddr[2] = 0x5e;
    s->ctl->ios2_SrcAddr[3] = (UBYTE)((a >> 16) & 0x7f);
    s->ctl->ios2_SrcAddr[4] = (UBYTE)(a >> 8);
    s->ctl->ios2_SrcAddr[5] = (UBYTE)a;
    err = ctl_cmd(s, action == NETIF_ADD_MAC_FILTER ? S2_ADDMULTICASTADDRESS : S2_DELMULTICASTADDRESS);
    if (err) {
        s->mcast_err = err;
        return ERR_IF;
    }
    return ERR_OK;
}
#endif

static err_t sanaif_netif_init(struct netif *netif)
{
    struct sanaif *s = (struct sanaif *)netif->state;

    if (s->p2p) {
        netif->name[0]    = 's';
        netif->name[1]    = 'l';
        netif->hwaddr_len = 0;
        netif->mtu        = (u16_t)(s->mtu && s->mtu < 1500 ? s->mtu : 1500);
        netif->flags      = 0;          /* no broadcast, ARP or multicast on the link */
        netif->output     = sanaif_p2p_output;
        return ERR_OK;
    }
    netif->name[0]    = 'e';
    netif->name[1]    = 'n';
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, s->mac, 6);
    netif->mtu        = (u16_t)(s->mtu && s->mtu < 1500 ? s->mtu : 1500);
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output     = etharp_output;
    netif->linkoutput = sanaif_linkoutput;
#if LWIP_IGMP
    netif->flags     |= NETIF_FLAG_IGMP;
    netif_set_igmp_mac_filter(netif, sanaif_igmp_filter);
#endif
    return ERR_OK;
}

/* ---- public API ------------------------------------------------------- */

static LONG ctl_cmd(struct sanaif *s, UWORD cmd)
{
    s->ctl->ios2_Req.io_Command = cmd;
    s->ctl->ios2_Req.io_Flags   = 0;
    return DoIO((struct IORequest *)s->ctl);
}

LONG sanaif_open(struct sanaif *s, const char *devname, ULONG unit,
                 const ip4_addr_t *ip, const ip4_addr_t *netmask,
                 const ip4_addr_t *gw, const char **why)
{
    struct Sana2DeviceQuery dq;
    LONG err;
    int i;

    memset(s, 0, sizeof(*s));
    strncpy(s->devname, devname, sizeof(s->devname) - 1);
    s->unit = unit;

    s->port = CreateMsgPort();
    if (!s->port) { *why = "no memory for port"; return -1; }
    s->ctl = (struct IOSana2Req *)CreateIORequest(s->port, sizeof(struct IOSana2Req));
    if (!s->ctl) { *why = "no memory for request"; sanaif_close(s); return -1; }

    /* ramlib only searches DEVS:, but SANA-II drivers live in DEVS:Networks/,
     * so a bare name is tried there first. */
    s->ctl->ios2_BufferManagement = buffer_tags;
    err = -1;
    if (!strchr(devname, ':') && !strchr(devname, '/')) {
        char path[96];
        strcpy(path, "DEVS:Networks/");
        strncat(path, devname, sizeof(path) - strlen(path) - 1);
        err = OpenDevice((CONST_STRPTR)path, unit, (struct IORequest *)s->ctl, 0);
    }
    if (err != 0) {
        s->ctl->ios2_BufferManagement = buffer_tags;
        err = OpenDevice((CONST_STRPTR)devname, unit, (struct IORequest *)s->ctl, 0);
    }
    if (err != 0) {
        *why = "OpenDevice failed";
        s->open_wire_error = s->ctl->ios2_WireError;
        sanaif_close(s);
        return err;
    }
    s->dev_open = TRUE;

    memset(&dq, 0, sizeof(dq));
    dq.SizeAvailable = sizeof(dq);
    s->ctl->ios2_StatData = &dq;
    if (ctl_cmd(s, S2_DEVICEQUERY) == 0) {
        s->hwtype   = dq.HardwareType;
        s->addrbits = dq.AddrFieldSize;
        if (dq.HardwareType == S2WireType_PPP || dq.HardwareType == S2WireType_SLIP ||
            dq.HardwareType == S2WireType_CSLIP || dq.HardwareType == S2WireType_PLIP) {
            s->p2p = TRUE;
        } else if ((dq.HardwareType != S2WireType_Ethernet &&
                    dq.HardwareType != S2WireType_IEEE802) || dq.AddrFieldSize != 48) {
            *why = "neither Ethernet nor a point-to-point link (SLIP/PPP/PLIP)";
            sanaif_close(s);
            return -2;
        }
        s->mtu = dq.MTU;
        s->bps = dq.BPS;
    }

    /* Configure with the factory address unless already configured
     * (then S2_CONFIGINTERFACE fails with S2ERR_BAD_STATE, which is fine). */
    if (ctl_cmd(s, S2_GETSTATIONADDRESS) == 0) {
        memcpy(s->ctl->ios2_SrcAddr, s->ctl->ios2_DstAddr, 6);
        ctl_cmd(s, S2_CONFIGINTERFACE);
    }
    ctl_cmd(s, S2_ONLINE);
    if (ctl_cmd(s, S2_GETSTATIONADDRESS) == 0) {
        memcpy(s->mac, s->ctl->ios2_SrcAddr, 6);
    } else if (!s->p2p) {               /* a link without addresses may refuse it */
        *why = "S2_GETSTATIONADDRESS failed";
        sanaif_close(s);
        return s->ctl->ios2_Req.io_Error;
    }
    if (s->p2p && s->addrbits == 32 && (!ip || ip4_addr_isany(ip)) &&
        (s->mac[0] | s->mac[1] | s->mac[2] | s->mac[3])) {
        /* the driver's station address is our IPv4 address */
        static ip4_addr_t drv_ip;
        IP4_ADDR(&drv_ip, s->mac[0], s->mac[1], s->mac[2], s->mac[3]);
        ip = &drv_ip;
    }
    if (s->p2p && (!netmask || ip4_addr_isany(netmask))) {
        /* a link to one peer: without a netmask its "subnet" 0.0.0.0/0
         * would claim every destination (vpn.device, 2026-10-28) */
        static ip4_addr_t host_mask;
        IP4_ADDR(&host_mask, 255, 255, 255, 255);
        netmask = &host_mask;
    }

    for (i = 0; i < SANAIF_NUM_RX; i++)
        CopyMem(s->ctl, &s->rx[i].io, sizeof(struct IOSana2Req));
    for (i = 0; i < SANAIF_NUM_TX; i++)
        CopyMem(s->ctl, &s->tx[i].io, sizeof(struct IOSana2Req));

    if (!netif_add(&s->netif, ip, netmask, gw, s, sanaif_netif_init,
                   s->p2p ? ip_input : ethernet_input)) {
        *why = "netif_add failed";
        sanaif_close(s);
        return -3;
    }

    for (i = 0; i < SANAIF_NUM_RX; i++)
        queue_read(s, &s->rx[i]);

    s->stall_ms = SANAIF_TX_STALL_MS;
    netif_set_link_up(&s->netif);
    *why = "ok";
    return 0;
}

ULONG sanaif_sigmask(struct sanaif *s)
{
    return 1UL << s->port->mp_SigBit;
}

void sanaif_poll(struct sanaif *s)
{
    struct Message *msg;
    int i;

    if (s->ser) {
        serslip_poll(s);
        return;
    }
#ifdef PN_PROFILE
    for (i = 0; i < SANAIF_NUM_RX_IP && !s->rx[i].busy; i++)
        ;
    if (i == SANAIF_NUM_RX_IP)
        s->stats.rx_starved++;
#endif

    while ((msg = GetMsg(s->port)) != NULL) {
        struct sanaif_req *r = (struct sanaif_req *)msg;

        if (r >= s->rx && r < s->rx + SANAIF_NUM_RX) {
            r->lost = FALSE;            /* a read kept past a reset came back */
            rx_done(s, r);
            if (!r->defer)
                queue_read(s, r);
        } else if (r >= s->tx && r < s->tx + SANAIF_NUM_TX) {
            tx_done(s, r);
        }
    }
    if (s->txq_n)
        drain_txq(s);

    /* Requeue reads that could not get a pbuf earlier. */
    for (i = 0; i < SANAIF_NUM_RX; i++)
        if (!s->rx[i].busy && !s->rx[i].lost && !s->rx[i].defer)
            queue_read(s, &s->rx[i]);
}

/* AbortIO() with A3 = io_Unit. exec passes only A1 and A6, but drivers built
 * from Commodore's SANA-II example (Rhialto's slip.device 37.4) take the unit
 * from A3: with whatever A3 happened to hold they searched a garbage list,
 * aborted nothing, and our WaitIO() blocked until a packet arrived for every
 * read. The old core lived on after STOP and its reads ate the new one's
 * packets (SLIP dead for ~a minute after a restart). Other drivers ignore A3. */
static void abort_io(struct IORequest *io)
{
    register struct IORequest *a1 __asm("a1") = io;
    register struct Unit *a3 __asm("a3") = io->io_Unit;
    register struct Device *a6 __asm("a6") = io->io_Device;

    __asm volatile ("jsr a6@(-36)"      /* DEV_ABORTIO */
                    : "+r" (a1)
                    : "r" (a3), "r" (a6)
                    : "d0", "d1", "a0", "cc", "memory");
}

/* Wait for a sent `io` for at most ~1 s. FALSE if the driver kept it. */
static BOOL io_bounded(struct IORequest *io)
{
    int n;

    for (n = 0; n < 50; n++) {
        if (CheckIO(io)) {
            WaitIO(io);
            return TRUE;
        }
        Delay(1);
    }
    return FALSE;
}

/* Collect aborted requests, ~1 s for all of them together; the ones the
 * driver keeps are marked lost (never reused, their pbufs stay with them). */
static void collect_aborted(struct sanaif *s, struct sanaif_req **all, int n)
{
    int i, left = n, ticks = 0;

#ifdef SANAIF_TEST_LOST
    ticks = 1000;                       /* test build: a driver that ignores AbortIO */
#endif
    while (left > 0 && ticks <= 50) {
        for (i = 0; i < n; i++) {
            struct sanaif_req *r = all[i];
            if (!r || !CheckIO((struct IORequest *)&r->io))
                continue;
            WaitIO((struct IORequest *)&r->io);
            r->busy = FALSE;
            if (r >= s->tx && r < s->tx + SANAIF_NUM_TX) {
                s->stats.tx_errors++;
                pbuf_free(r->p);
                r->p = NULL;
            }
            all[i] = NULL;
            left--;
        }
        if (left == 0 || ++ticks > 50)
            break;
        Delay(1);
    }
    for (i = 0; i < n; i++)
        if (all[i])
            all[i]->lost = TRUE;
}

static void reset_device(struct sanaif *s)
{
    struct sanaif_req *all[SANAIF_NUM_RX + SANAIF_NUM_TX];
    int i, n = 0;

    for (i = 0; i < SANAIF_NUM_RX; i++)
        if (s->rx[i].busy) all[n++] = &s->rx[i];
    for (i = 0; i < SANAIF_NUM_TX; i++)
        if (s->tx[i].busy) all[n++] = &s->tx[i];
    for (i = 0; i < n; i++)
        abort_io((struct IORequest *)&all[i]->io);
    collect_aborted(s, all, n);
    s->stats.resets++;
    if (s->ctl_lost)
        return;                         /* ctl is still with the driver: cannot cycle it */
    /* Restart the hardware: S2_OFFLINE, S2_ONLINE (bounded: it is stuck). */
    s->ctl->ios2_Req.io_Command = S2_OFFLINE;
    s->ctl->ios2_Req.io_Flags   = 0;
    SendIO((struct IORequest *)s->ctl);
    if (!io_bounded((struct IORequest *)s->ctl)) {
        s->ctl_lost = TRUE;             /* never SendIO or free it again */
        return;
    }
    s->ctl->ios2_Req.io_Command = S2_ONLINE;
    s->ctl->ios2_Req.io_Flags   = 0;
    SendIO((struct IORequest *)s->ctl);
    if (!io_bounded((struct IORequest *)s->ctl))
        s->ctl_lost = TRUE;
}

BOOL sanaif_watchdog(struct sanaif *s, u32_t now)
{
    if (s->ser)
        return FALSE;               /* serial.device does not stall like that */
    int i;

    /* Reads that failed (unit offline) are retried from here, i.e. at most
     * every 250 ms, instead of the instant they come back. */
    for (i = 0; i < SANAIF_NUM_RX; i++) {
        struct sanaif_req *r = &s->rx[i];
        if (r->defer) {
            r->defer = FALSE;
            if (!r->busy && !r->lost)
                queue_read(s, r);
        }
    }
#ifdef SANAIF_TEST_RESET
    static int calls;
    if (++calls % SANAIF_TEST_RESET == 0) {   /* test build: reset regularly */
        reset_device(s);
        sanaif_poll(s);
        return TRUE;
    }
#endif

    for (i = 0; i < SANAIF_NUM_TX; i++) {
        struct sanaif_req *r = &s->tx[i];
        if (!r->busy || r->lost)
            continue;
        if (!r->seen) {
            r->seen = now | 1;          /* 0 means "not seen yet" */
        } else if (now - r->seen >= s->stall_ms && now - s->tx_progress >= s->stall_ms) {
            /* a write held this long AND nothing completed meanwhile: on a
             * slow link (serial, PLIP) writes queue up but keep completing */
            if (s->stall_ms < SANAIF_TX_STALL_MAX_MS || s->stall_ms < 4 * s->stall_base)
                s->stall_ms *= 2;
            reset_device(s);
            sanaif_poll(s);             /* requeue the reads, send what is queued */
            return TRUE;
        }
    }
    return FALSE;
}

static void free_req(struct sanaif_req *r)
{
    if (r->lost)
        return;                         /* the driver still has it */
    if (r->p) {
        pbuf_free(r->p);
        r->p = NULL;
    }
}

/* After sanaif_close(): TRUE if the driver kept requests that live in *s,
 * so its memory must not be freed. */
BOOL sanaif_requests_lost(struct sanaif *s)
{
    int i;
    BOOL lost = s->ctl_lost;
    if (s->ser)
        return FALSE;
    for (i = 0; i < SANAIF_NUM_RX; i++) lost |= s->rx[i].lost;
    for (i = 0; i < SANAIF_NUM_TX; i++) lost |= s->tx[i].lost;
    return lost;
}

void sanaif_close(struct sanaif *s)
{
    struct sanaif_req *all[SANAIF_NUM_RX + SANAIF_NUM_TX];
    int i, n = 0;
    BOOL lost = s->ctl_lost;

    if (s->ser) {
        serslip_close(s);
        return;
    }
    if (s->netif.state == s) {
        netif_set_down(&s->netif);
        netif_remove(&s->netif);
        s->netif.state = NULL;
    }
    /* Abort everything, then wait ~1 s at most: a request the driver does
     * not give back is marked lost rather than blocking the exit forever. */
    for (i = 0; i < SANAIF_NUM_RX; i++)
        if (s->rx[i].busy && !s->rx[i].lost) all[n++] = &s->rx[i];
    for (i = 0; i < SANAIF_NUM_TX; i++)
        if (s->tx[i].busy && !s->tx[i].lost) all[n++] = &s->tx[i];
    for (i = 0; i < n; i++)
        abort_io((struct IORequest *)&all[i]->io);
    collect_aborted(s, all, n);
    for (i = 0; i < SANAIF_NUM_RX; i++) {
        free_req(&s->rx[i]);
        lost |= s->rx[i].lost;
    }
    for (i = 0; i < SANAIF_NUM_TX; i++) {
        free_req(&s->tx[i]);
        lost |= s->tx[i].lost;
    }
    while (s->txq_n) {
        pbuf_free(s->txq[s->txq_head]);
        s->txq[s->txq_head] = NULL;
        s->txq_head = (UBYTE)((s->txq_head + 1) % SANAIF_TXQ);
        s->txq_n--;
    }
    if (lost) {
        /* The driver still holds requests that point at this port and these
         * IORequests: freeing them would hand it dangling memory when it
         * finally replies. Leave the device open and the memory allocated. */
        s->port = NULL;
        s->ctl  = NULL;
        return;
    }
    if (s->dev_open) {
        CloseDevice((struct IORequest *)s->ctl);
        s->dev_open = FALSE;
    }
    if (s->ctl) {
        DeleteIORequest((struct IORequest *)s->ctl);
        s->ctl = NULL;
    }
    if (s->port) {
        DeleteMsgPort(s->port);
        s->port = NULL;
    }
}
