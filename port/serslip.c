/*
 * PrismNet: SLIP straight over serial.device, without a SANA-II driver.
 *
 * A serial cable to a SLIP peer (a PC, a Pi, an ESP32 bridge) is all it needs.
 *
 * It fills in the same struct sanaif as a SANA-II interface (netif, name,
 * statistics, p2p), so the core treats both alike; sanaif.c hands poll,
 * close and the watchdog to the functions here when s->ser is set.
 *
 * serial.device runs in EOF mode with SLIP's END byte (0xC0) as the
 * terminator: one CMD_READ takes one packet and completes on its END, and
 * whatever arrives before the next read waits in serial.device's own 8 KB
 * buffer. (Reading one byte and then SDCMD_QUERY plus a bulk read lost
 * bytes to overruns.) EOF mode ends writes at an END too, so each packet is
 * written as escaped data followed by END, no leading END. A read that
 * reports an error (overrun) still hands on what it got; a damaged packet
 * fails its IP or TCP checksum. RFC 1055 framing.
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/serial.h>
#include <proto/exec.h>

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip.h"
#include "prismnet/sanaif.h"
#include "prismnet/sana2defs.h"
#include "prismnet/pnmem.h"

void core_log(const char *fmt, ...);

#define SLIP_END      0xC0
#define SLIP_ESC      0xDB
#define SLIP_ESC_END  0xDC
#define SLIP_ESC_ESC  0xDD

#define SER_MTU       1006          /* the traditional SLIP MTU */
#ifdef PN_SMALL
#define SER_NWR       4
#else
#define SER_NWR       8
#endif
#define SER_RBUF      8192          /* serial.device's own receive buffer */
#define SER_RDLEN     (2 * SER_MTU + 2)   /* longest escaped packet */
#define SER_NRD       3             /* reads kept queued, so one always waits */

struct serslip {
    struct IOExtSer *rd[SER_NRD];   /* queued packet reads, completed in order */
    struct IOExtSer *ctl;           /* open and parameters */
    struct IOExtSer *wr[SER_NWR];
    UBYTE           *wbuf[SER_NWR]; /* escaped packet being written, or NULL */
    BOOL             rd_busy[SER_NRD];
    BOOL             esc, overflow;
    ULONG            flen;
    UBYTE            rbuf[SER_NRD][SER_RDLEN];
    UBYTE            frame[SER_MTU + 4];
};

/* ---- receive ------------------------------------------------------------ */

static void deliver(struct sanaif *s, struct serslip *z)
{
    struct pbuf *p;

    if (z->flen < 20 || (z->frame[0] >> 4) != 4) {      /* not an IPv4 packet */
        if (s->stats.rx_errors++ < 5)
            core_log("%s: dropped a %lu-byte frame that is not IPv4", s->devname,
                     (unsigned long)z->flen);
        return;
    }
    if (!(p = pbuf_alloc(PBUF_RAW, (u16_t)z->flen, PBUF_POOL))) {
        s->stats.rx_dropped++;
        return;
    }
    pbuf_take(p, z->frame, (u16_t)z->flen);
    s->stats.rx_packets++;
    s->stats.rx_bytes += z->flen;
    if (s->netif.input(p, &s->netif) != ERR_OK)
        pbuf_free(p);
}

static void feed(struct sanaif *s, struct serslip *z, const UBYTE *b, ULONG n)
{
    while (n--) {
        UBYTE c = *b++;
        if (c == SLIP_END) {
            if (z->flen && !z->overflow)
                deliver(s, z);
            else if (z->overflow)
                s->stats.rx_errors++;
            z->flen = 0;
            z->esc = z->overflow = FALSE;
            continue;
        }
        if (c == SLIP_ESC) {
            z->esc = TRUE;
            continue;
        }
        if (z->esc) {
            c = c == SLIP_ESC_END ? SLIP_END : c == SLIP_ESC_ESC ? SLIP_ESC : c;
            z->esc = FALSE;
        }
        if (z->flen < SER_MTU)
            z->frame[z->flen++] = c;
        else
            z->overflow = TRUE;
    }
}

/* Several reads stay queued: when one completes on an END, the next one is
 * already there for the bytes that follow, so the interrupt code stores them
 * straight into a request instead of serial.device's ring buffer (whose
 * later copy-out cost overruns). serial.device completes them in order. */
static void start_read(struct serslip *z, int i)
{
    z->rd[i]->IOSer.io_Command = CMD_READ;
    z->rd[i]->IOSer.io_Length  = SER_RDLEN;
    z->rd[i]->IOSer.io_Data    = z->rbuf[i];
    z->rd_busy[i] = TRUE;
    SendIO((struct IORequest *)z->rd[i]);
}

static void read_done(struct sanaif *s, struct serslip *z, int i)
{
    z->rd_busy[i] = FALSE;
    if (z->rd[i]->IOSer.io_Error && s->stats.rx_errors++ < 5)
        core_log("%s: serial read error %ld", s->devname, (long)z->rd[i]->IOSer.io_Error);
    feed(s, z, z->rbuf[i], z->rd[i]->IOSer.io_Actual);
    start_read(z, i);
}

/* ---- transmit ----------------------------------------------------------- */

static err_t serslip_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    struct sanaif *s = (struct sanaif *)netif->state;
    struct serslip *z = s->ser;
    struct pbuf *q;
    UBYTE *buf, *o;
    int i;

    (void)ipaddr;
    for (i = 0; i < SER_NWR && z->wbuf[i]; i++)
        ;
    if (i == SER_NWR || !(buf = pn_mem_malloc(2 * p->tot_len + 1))) {
        s->stats.tx_dropped++;
        return ERR_MEM;
    }
    o = buf;
    for (q = p; q; q = q->next) {
        const UBYTE *d = q->payload;
        u16_t n = q->len;
        while (n--) {
            UBYTE c = *d++;
            if (c == SLIP_END)      { *o++ = SLIP_ESC; *o++ = SLIP_ESC_END; }
            else if (c == SLIP_ESC) { *o++ = SLIP_ESC; *o++ = SLIP_ESC_ESC; }
            else                    *o++ = c;
        }
    }
    *o++ = SLIP_END;                    /* the only END: EOF mode stops the write there */
    z->wbuf[i] = buf;
    z->wr[i]->IOSer.io_Command = CMD_WRITE;
    z->wr[i]->IOSer.io_Length  = o - buf;
    z->wr[i]->IOSer.io_Data    = buf;
    SendIO((struct IORequest *)z->wr[i]);
    s->stats.tx_packets++;
    s->stats.tx_bytes += p->tot_len;
    return ERR_OK;
}

static void write_done(struct sanaif *s, struct serslip *z, int i)
{
    if (z->wr[i]->IOSer.io_Error)
        s->stats.tx_errors++;
    pn_mem_free(z->wbuf[i]);
    z->wbuf[i] = NULL;
}

/* ---- netif and lifetime ------------------------------------------------- */

static err_t serslip_netif_init(struct netif *netif)
{
    struct sanaif *s = (struct sanaif *)netif->state;
    netif->name[0]    = 's';
    netif->name[1]    = 'l';
    netif->hwaddr_len = 0;
    netif->mtu        = (u16_t)(s->mtu ? s->mtu : SER_MTU);
    netif->flags      = 0;              /* no broadcast, ARP or multicast */
    netif->output     = serslip_output;
    return ERR_OK;
}

static struct IOExtSer *copy_req(struct serslip *z, struct MsgPort *port)
{
    struct IOExtSer *r = (struct IOExtSer *)CreateIORequest(port, sizeof(struct IOExtSer));
    if (r) {
        CopyMem(z->ctl, r, sizeof(struct IOExtSer));
        r->IOSer.io_Message.mn_ReplyPort = port;
    }
    return r;
}

LONG serslip_open(struct sanaif *s, const char *devname, ULONG unit, ULONG baud,
                  BOOL rtscts, ULONG mtu, const ip4_addr_t *ip,
                  const ip4_addr_t *netmask, const ip4_addr_t *gw, const char **why)
{
    struct serslip *z;
    LONG err;
    int i;

    memset(s, 0, sizeof(*s));
    strncpy(s->devname, devname, sizeof(s->devname) - 1);
    s->unit   = unit;
    s->p2p    = TRUE;
    s->hwtype = S2WireType_SLIP;
    s->bps    = baud ? baud : 19200;
    s->mtu    = mtu && mtu < SER_MTU ? mtu : SER_MTU;

    if (!(z = AllocVec(sizeof(*z), MEMF_PUBLIC | MEMF_CLEAR))) {
        *why = "no memory";
        return -1;
    }
    s->ser = z;
    if (!(s->port = CreateMsgPort()) ||
        !(z->ctl = (struct IOExtSer *)CreateIORequest(s->port, sizeof(struct IOExtSer)))) {
        *why = "no memory for port";
        serslip_close(s);
        return -1;
    }
    z->ctl->io_SerFlags = SERF_SHARED;
    if ((err = OpenDevice((CONST_STRPTR)devname, unit, (struct IORequest *)z->ctl, 0)) != 0) {
        *why = "OpenDevice failed (serial port in use?)";
        DeleteIORequest((struct IORequest *)z->ctl);
        z->ctl = NULL;
        serslip_close(s);
        return err;
    }
    s->dev_open = TRUE;

    /* 8N1, no XON/XOFF (SLIP is binary), optional RTS/CTS, a big buffer,
     * reads and writes ending at SLIP's END byte */
    z->ctl->IOSer.io_Command = SDCMD_SETPARAMS;
    z->ctl->io_Baud      = s->bps;
    z->ctl->io_ReadLen   = 8;
    z->ctl->io_WriteLen  = 8;
    z->ctl->io_StopBits  = 1;
    z->ctl->io_RBufLen   = SER_RBUF;
    z->ctl->io_TermArray.TermArray0 = 0xC0C0C0C0UL;
    z->ctl->io_TermArray.TermArray1 = 0xC0C0C0C0UL;
    z->ctl->io_SerFlags  = SERF_SHARED | SERF_XDISABLED | SERF_EOFMODE |
                           (rtscts ? SERF_7WIRE : 0);
    if (DoIO((struct IORequest *)z->ctl) != 0) {
        *why = "the serial port refused the settings";
        serslip_close(s);
        return -4;
    }

    for (i = 0; i < SER_NRD; i++)
        if (!(z->rd[i] = copy_req(z, s->port))) {
            *why = "no memory for requests";
            serslip_close(s);
            return -1;
        }
    for (i = 0; i < SER_NWR; i++)
        if (!(z->wr[i] = copy_req(z, s->port))) {
            *why = "no memory for requests";
            serslip_close(s);
            return -1;
        }

    if (!netif_add(&s->netif, ip, netmask, gw, s, serslip_netif_init, ip_input)) {
        *why = "netif_add failed";
        serslip_close(s);
        return -3;
    }
    for (i = 0; i < SER_NRD; i++)
        start_read(z, i);
    netif_set_link_up(&s->netif);
    *why = "ok";
    return 0;
}

void serslip_poll(struct sanaif *s)
{
    struct serslip *z = s->ser;
    struct Message *msg;
    int i;

    while ((msg = GetMsg(s->port)) != NULL) {
        for (i = 0; i < SER_NRD; i++)
            if (msg == (struct Message *)z->rd[i])
                break;
        if (i < SER_NRD) {
            read_done(s, z, i);
            continue;
        }
        for (i = 0; i < SER_NWR; i++)
            if (msg == (struct Message *)z->wr[i]) {
                write_done(s, z, i);
                break;
            }
    }
}

void serslip_close(struct sanaif *s)
{
    struct serslip *z = s->ser;
    int i;

    if (s->netif.state == s) {
        netif_set_down(&s->netif);
        netif_remove(&s->netif);
        s->netif.state = NULL;
    }
    if (z) {
        for (i = 0; i < SER_NRD; i++)
            if (z->rd[i] && z->rd_busy[i]) {
                AbortIO((struct IORequest *)z->rd[i]);
                WaitIO((struct IORequest *)z->rd[i]);
            }
        for (i = 0; i < SER_NWR; i++) {
            if (z->wr[i] && z->wbuf[i]) {
                AbortIO((struct IORequest *)z->wr[i]);
                WaitIO((struct IORequest *)z->wr[i]);
                pn_mem_free(z->wbuf[i]);
            }
            if (z->wr[i])
                DeleteIORequest((struct IORequest *)z->wr[i]);
        }
        for (i = 0; i < SER_NRD; i++)
            if (z->rd[i])
                DeleteIORequest((struct IORequest *)z->rd[i]);
        if (z->ctl) {
            if (s->dev_open)
                CloseDevice((struct IORequest *)z->ctl);
            DeleteIORequest((struct IORequest *)z->ctl);
        }
        FreeVec(z);
        s->ser = NULL;
    }
    s->dev_open = FALSE;
    if (s->port) {
        DeleteMsgPort(s->port);
        s->port = NULL;
    }
}
