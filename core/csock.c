/*
 * PrismNet core task: BSD socket semantics on top of the lwIP raw API.
 * GPL-3.0-only, see LICENSE.
 *
 * lwIP callbacks only record state (queue data, set flags, store errors).
 * Every operation is written so it can be re-run: the core calls it when
 * the request arrives and again after each event while it is pending, and
 * it either replies (returns TRUE) or asks to stay pending (FALSE).
 * r->progress carries partial state (bytes done, "connect started").
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/memp.h"
#include "lwip/stats.h"

#include "core.h"

#define PN_FIONBIO     0x8004667eUL
#define PN_FIOASYNC    0x8004667dUL
#define PN_FIONREAD    0x4004667fUL
#define PN_SIOCATMARK  0x40047307UL

#define SO_DEBUG_      0x0001
#define SO_ACCEPTCONN_ 0x0002
#define SO_REUSEADDR_  0x0004
#define SO_KEEPALIVE_  0x0008
#define SO_DONTROUTE_  0x0010
#define SO_BROADCAST_  0x0020
#define SO_LINGER_     0x0080
#define SO_OOBINLINE_  0x0100
#define SO_REUSEPORT_  0x0200
#define SO_SNDBUF_     0x1001
#define SO_RCVBUF_     0x1002
#define SO_SNDLOWAT_   0x1003
#define SO_RCVLOWAT_   0x1004
#define SO_SNDTIMEO_   0x1005
#define SO_RCVTIMEO_   0x1006
#define SO_ERROR_      0x1007
#define SO_TYPE_       0x1008
#define SO_EVENTMASK_  0x2001
#define TCP_NODELAY_   0x01
#define IP_HDRINCL_    2
#define IP_TOS_        3
#define IP_TTL_        4

/* GetSocketEvents() event bits */
#define FD_ACCEPT_     0x01
#define FD_CONNECT_    0x02
#define FD_OOB_        0x04
#define FD_READ_       0x08
#define FD_WRITE_      0x10
#define FD_ERROR_      0x20
#define FD_CLOSE_      0x40

struct PnRelease {
    struct MinNode  node;
    struct PnSock  *s;
    LONG            id;
};
static struct MinList released;

/* ---------------------------------------------------------------------- */
/* helpers                                                                  */
/* ---------------------------------------------------------------------- */

LONG err2errno(err_t e)
{
    switch (e) {
    case ERR_OK:         return 0;
    case ERR_MEM:
    case ERR_BUF:        return PN_ENOBUFS;
    case ERR_TIMEOUT:    return PN_ETIMEDOUT;
    case ERR_RTE:        return PN_EHOSTUNREACH;
    case ERR_INPROGRESS: return PN_EINPROGRESS;
    case ERR_WOULDBLOCK: return PN_EWOULDBLOCK;
    case ERR_USE:        return PN_EADDRINUSE;
    case ERR_ALREADY:    return PN_EALREADY;
    case ERR_ISCONN:     return PN_EISCONN;
    case ERR_CONN:       return PN_ENOTCONN;
    case ERR_IF:         return PN_ENETUNREACH;
    case ERR_ABRT:       return PN_ECONNABORTED;
    case ERR_RST:        return PN_ECONNRESET;
    case ERR_CLSD:       return PN_ENOTCONN;
    default:             return PN_EINVAL;
    }
}

BOOL sa_get(const UBYTE *sa, LONG len, ip_addr_t *ip, u16_t *port, BOOL *oldstyle)
{
    if (!sa || len < 8)
        return FALSE;
    if (sa[1] != PN_AF_INET)
        return FALSE;
    *oldstyle = (sa[0] == 0);     /* 4.3BSD: UWORD family, high byte 0 */
    *port = (u16_t)((sa[2] << 8) | sa[3]);
    IP_ADDR4(ip, sa[4], sa[5], sa[6], sa[7]);
    return TRUE;
}

void sa_put(UBYTE *sa, LONG *lenp, const ip_addr_t *ip, u16_t port, BOOL oldstyle)
{
    UBYTE tmp[16];
    LONG n;
    u32_t a = ip ? lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(ip))) : 0;

    if (!sa)
        return;
    memset(tmp, 0, sizeof(tmp));
    tmp[0] = oldstyle ? 0 : 16;
    tmp[1] = PN_AF_INET;
    tmp[2] = (UBYTE)(port >> 8);  tmp[3] = (UBYTE)port;
    tmp[4] = (UBYTE)(a >> 24);    tmp[5] = (UBYTE)(a >> 16);
    tmp[6] = (UBYTE)(a >> 8);     tmp[7] = (UBYTE)a;
    n = lenp ? *lenp : 16;
    if (n > 16) n = 16;
    if (n > 0)  memcpy(sa, tmp, n);
    if (lenp)   *lenp = 16;
}

static BOOL deadline_passed(struct PnReq *r, ULONG timeout_ms)
{
    if (!timeout_ms)
        return FALSE;
    if (!r->has_deadline) {
        core_set_deadline(r, timeout_ms);
        return FALSE;
    }
    return (LONG)(sys_now() - r->deadline) >= 0;
}

static LONG take_error(struct PnSock *s)
{
    LONG e = s->so_error;
    s->so_error = 0;
    return e;
}

/* ---------------------------------------------------------------------- */
/* lwIP callbacks                                                           */
/* ---------------------------------------------------------------------- */

/*
 * Unread TCP data stays in the pbufs it arrived in. Those are pool pbufs, the
 * same ones the SANA-II reads are queued with: sockets whose programs read
 * slowly could hold the whole pool and stop all reception. When the pool runs
 * low, received data is moved to the heap instead (a copy, but only then).
 */
#ifndef RX_POOL_RESERVE
#define RX_POOL_RESERVE 16
#endif

static struct pbuf *keep_rx(struct pbuf *p)
{
    struct pbuf *q;

    if (lwip_stats.memp[MEMP_PBUF_POOL]->used + RX_POOL_RESERVE < PBUF_POOL_SIZE ||
        !pbuf_match_allocsrc(p, PBUF_POOL))
        return p;
    q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!q)
        return p;
    pbuf_copy(q, p);
    pbuf_free(p);
    return q;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct PnSock *s = (struct PnSock *)arg;

    LWIP_UNUSED_ARG(err);
    if (!s) {
        if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); }
        return ERR_OK;
    }
    if (!p) {
        s->flags |= SF_RD_EOF;
        /* Peer FIN after our own shutdown(SHUT_WR): both directions are
         * done. lwIP finishes the handshake alone and frees the pcb from
         * TIME_WAIT after 2*MSL *without any callback*, so drop it now or
         * a later CloseSocket/getsockname would touch freed memory. */
        if ((s->flags & SF_WR_SHUT) && (pcb->state == TIME_WAIT || pcb->state == CLOSING)) {
            tcp_arg(pcb, NULL);
            tcp_recv(pcb, NULL);
            tcp_err(pcb, NULL);
            s->pcb.tcp = NULL;
        }
        return ERR_OK;
    }
    if (s->flags & SF_RD_SHUT) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }
    if (s->rxq)
        pbuf_cat(s->rxq, keep_rx(p));
    else
        s->rxq = keep_rx(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    struct PnSock *s = (struct PnSock *)arg;

    if (!s)
        return;
    s->pcb.tcp = NULL;          /* already freed by lwIP */
    if (s->flags & SF_CONNECTING)
        s->so_error = err == ERR_RST ? PN_ECONNREFUSED :
                      err == ERR_ABRT ? PN_ETIMEDOUT : err2errno(err);
    else if (err != ERR_CLSD)
        s->so_error = err == ERR_RST ? PN_ECONNRESET : err2errno(err);
    s->flags &= ~SF_CONNECTING;
    s->flags |= SF_RD_EOF;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct PnSock *s = (struct PnSock *)arg;

    LWIP_UNUSED_ARG(pcb);
    if (s) {
        s->flags &= ~SF_CONNECTING;
        if (err == ERR_OK)
            s->flags |= SF_CONNECTED;
        else
            s->so_error = err2errno(err);
    }
    return ERR_OK;
}

static void tcp_setup(struct PnSock *s, struct tcp_pcb *pcb)
{
    s->pcb.tcp = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_err(pcb, tcp_err_cb);
}

static LONG ev_sockets;                       /* sockets with a non-zero eventmask */

/* SO_SNDBUF: lwIP keeps the free send space per pcb (snd_buf, starting at
 * TCP_SND_BUF), so a new size moves it by the difference. Bounded by what
 * the segment queue can carry; the memory still comes out of the budget. */
#define SNDBUF_MIN  (2 * TCP_MSS)
#define SNDBUF_MAX  (TCP_SND_QUEUELEN * TCP_MSS > 0xffff ? 0xffff : TCP_SND_QUEUELEN * TCP_MSS)

static ULONG sock_sndbuf(const struct PnSock *s)
{
    return s->sndbuf ? s->sndbuf : TCP_SND_BUF;
}

static void set_sndbuf(struct PnSock *s, struct tcp_pcb *pcb, LONG v)
{
    LONG old = (LONG)sock_sndbuf(s), nb;

    if (v < SNDBUF_MIN) v = SNDBUF_MIN;
    if (v > SNDBUF_MAX) v = SNDBUF_MAX;
    s->sndbuf = (UWORD)v;
    if (pcb) {
        nb = (LONG)pcb->snd_buf + v - old;
        pcb->snd_buf = (tcpwnd_size_t)(nb < 0 ? 0 : nb);
    }
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    struct PnSock *ls = (struct PnSock *)arg, *ns;

    if (!ls || err != ERR_OK || !newpcb || ls->acc_count >= ls->backlog)
        return ERR_MEM;         /* lwIP aborts the new connection */
    ns = pn_alloc(sizeof(*ns), MEMF_CLEAR);
    if (!ns)
        return ERR_MEM;
    ns->type    = PN_SOCK_STREAM;
    ns->proto   = PN_IPPROTO_TCP;
    ns->backlog = 1;
    AddTail((struct List *)&core.sockets, (struct Node *)&ns->node);
    tcp_setup(ns, newpcb);
    ns->flags |= SF_CONNECTED | (ls->flags & (SF_NBIO | SF_OLDSOCKADDR));
    ns->keepalive = ls->keepalive;
    ns->nodelay   = ls->nodelay;
    ns->rcvtimeo  = ls->rcvtimeo;       /* inherited from the listener, as on BSD */
    ns->sndtimeo  = ls->sndtimeo;
    ns->linger_abort = ls->linger_abort;
    ns->linger_sec   = ls->linger_sec;
    if (ls->eventmask) {                /* Roadshow: the event mask carries over */
        ns->eventmask = ls->eventmask;
        ev_sockets++;
    }
    if (ls->pcb.tcp) {                  /* lwIP copies so_options only, not TTL/TOS */
        newpcb->ttl = ls->pcb.tcp->ttl;
        newpcb->tos = ls->pcb.tcp->tos;
    }
    if (ns->keepalive) ip_set_option(newpcb, SOF_KEEPALIVE);
    if (ns->nodelay)   tcp_nagle_disable(newpcb);
    if (ls->sndbuf)    set_sndbuf(ns, newpcb, ls->sndbuf);
    ls->accq[ls->acc_count++] = ns;
    return ERR_OK;
}

static void dgram_queue(struct PnSock *s, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct PnDgram *d;

    if (s->dg_count >= PN_MAX_DGRAMS || !(d = pn_alloc(sizeof(*d), MEMF_CLEAR))) {
        pbuf_free(p);
        return;
    }
    d->p = p;
    ip_addr_copy(d->addr, *addr);
    d->port = port;
    if (s->dg_tail)
        s->dg_tail->next = d;
    else
        s->dg_head = d;
    s->dg_tail = d;
    s->dg_count++;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    LWIP_UNUSED_ARG(pcb);
    if (arg)
        dgram_queue((struct PnSock *)arg, p, addr, port);
    else
        pbuf_free(p);
}

static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    struct pbuf *q;

    LWIP_UNUSED_ARG(pcb);
    /* BSD raw sockets see the whole IP datagram; lwIP keeps processing it
     * (e.g. answering echo requests), so queue a copy and don't eat it. */
    if (arg && (q = pbuf_clone(PBUF_RAW, PBUF_RAM, p)) != NULL)
        dgram_queue((struct PnSock *)arg, q, addr, 0);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* socket objects                                                           */
/* ---------------------------------------------------------------------- */

struct PnSock *sock_new(struct PnClient *owner, LONG type, LONG proto, LONG *err)
{
    struct PnSock *s;

    switch (type) {
    case PN_SOCK_STREAM:
        if (proto && proto != PN_IPPROTO_TCP) { *err = PN_EPROTONOSUPPORT; return NULL; }
        proto = PN_IPPROTO_TCP;
        break;
    case PN_SOCK_DGRAM:
        if (proto && proto != PN_IPPROTO_UDP) { *err = PN_EPROTONOSUPPORT; return NULL; }
        proto = PN_IPPROTO_UDP;
        break;
    case PN_SOCK_RAW:
        if (proto <= 0 || proto > 255) { *err = PN_EPROTONOSUPPORT; return NULL; }
        break;
    default:
        *err = PN_ESOCKTNOSUPPORT;
        return NULL;
    }

    s = pn_alloc(sizeof(*s), MEMF_CLEAR);
    if (!s) { *err = PN_ENOMEM; return NULL; }
    s->owner = owner;
    s->type  = (UBYTE)type;
    s->proto = (UBYTE)proto;
    s->backlog = 1;

    switch (type) {
    case PN_SOCK_STREAM: {
        struct tcp_pcb *pcb = tcp_new();
        if (pcb) tcp_setup(s, pcb);
        break;
    }
    case PN_SOCK_DGRAM:
        if ((s->pcb.udp = udp_new()) != NULL)
            udp_recv(s->pcb.udp, udp_recv_cb, s);
        break;
    case PN_SOCK_RAW:
        if ((s->pcb.raw = raw_new((u8_t)proto)) != NULL)
            raw_recv(s->pcb.raw, raw_recv_cb, s);
        break;
    }
    if (!s->pcb.any) {
        pn_free(s);
        *err = PN_ENOBUFS;
        return NULL;
    }
    AddTail((struct List *)&core.sockets, (struct Node *)&s->node);
    return s;
}

/* lwIP may free a pcb, and hand the memory to the next connection, without
 * a callback in a few corner cases. Every pcb of ours carries its socket as
 * callback_arg, so a pcb that points elsewhere is stale: forget it rather
 * than write into someone else's connection. */
static void sock_check_pcb(struct PnSock *s)
{
    if (s->type == PN_SOCK_STREAM && s->pcb.tcp && s->pcb.tcp->callback_arg != s) {
        core_log("stale TCP pcb dropped (socket flags %lx)", (unsigned long)s->flags);
        s->pcb.tcp = NULL;
        if (!s->so_error && !(s->flags & SF_LISTENING))
            s->so_error = PN_ECONNRESET;
        s->flags |= SF_RD_EOF;
    }
}

static void sock_destroy(struct PnSock *s)
{
    struct PnDgram *d;
    int i;

    if (s->eventmask)
        ev_sockets--;
    sock_check_pcb(s);
    switch (s->type) {
    case PN_SOCK_STREAM:
        if (s->pcb.tcp) {
            if (s->flags & SF_LISTENING) {
                tcp_arg(s->pcb.tcp, NULL);
                tcp_accept(s->pcb.tcp, NULL);
                tcp_close(s->pcb.tcp);
            } else {
                tcp_arg(s->pcb.tcp, NULL);
                tcp_recv(s->pcb.tcp, NULL);     /* lwIP's null handler drains */
                tcp_err(s->pcb.tcp, NULL);
                if (s->linger_abort)        /* SO_LINGER {on, 0}: reset, no FIN */
                    tcp_abort(s->pcb.tcp);
                else if (tcp_close(s->pcb.tcp) != ERR_OK)
                    tcp_abort(s->pcb.tcp);
            }
        }
        for (i = 0; i < s->acc_count; i++)
            sock_destroy(s->accq[i]);
        break;
    case PN_SOCK_DGRAM:
        if (s->pcb.udp) udp_remove(s->pcb.udp);
        break;
    case PN_SOCK_RAW:
        if (s->pcb.raw) raw_remove(s->pcb.raw);
        break;
    }
    if (s->rxq)
        pbuf_free(s->rxq);
    while ((d = s->dg_head) != NULL) {
        s->dg_head = d->next;
        pbuf_free(d->p);
        pn_free(d);
    }
    Remove((struct Node *)&s->node);
    pn_free(s);
}

void sock_unref(struct PnSock *s)
{
    if (--s->refs <= 0)
        sock_destroy(s);
}

void sock_init(void)
{
    pn_newlist(&released);
}

void sock_free_all(void)
{
    struct PnRelease *rel;
    while ((rel = (struct PnRelease *)RemHead((struct List *)&released)) != NULL) {
            sock_unref(rel->s);
            pn_free(rel);
        }
    while (core.sockets.mlh_Head->mln_Succ)
        sock_destroy((struct PnSock *)core.sockets.mlh_Head);
}

void sock_abort_pending(struct PnReq *r)
{
    LWIP_UNUSED_ARG(r);     /* nothing to undo: lwIP state is per socket */
}

/* ---------------------------------------------------------------------- */
/* readiness                                                                */
/* ---------------------------------------------------------------------- */

static BOOL readable(struct PnSock *s)
{
    return s->rxq || (s->flags & (SF_RD_EOF | SF_RD_SHUT)) || s->so_error || s->acc_count > 0 ||
           s->dg_head;
}

static BOOL writable(struct PnSock *s)
{
    if (s->type != PN_SOCK_STREAM)
        return TRUE;
    if (s->so_error || (s->flags & SF_WR_SHUT) || !s->pcb.tcp)
        return (s->flags & (SF_CONNECTED | SF_CONNECTING | SF_RD_EOF)) != 0 || s->so_error;
    return (s->flags & SF_CONNECTED) && tcp_sndbuf(s->pcb.tcp) > 0;
}

/* ---------------------------------------------------------------------- */
/* operations                                                               */
/* ---------------------------------------------------------------------- */

#define REPLY(res, err) do { core_reply(r, (res), (err)); return TRUE; } while (0)

static BOOL op_socket(struct PnClient *c, struct PnReq *r)
{
    struct PnSock *s;
    LONG err, fd;

    if (r->a[0] != PN_AF_INET)
        REPLY(-1, PN_EAFNOSUPPORT);
    s = sock_new(c, (LONG)r->a[1], (LONG)r->a[2], &err);
    if (!s)
        REPLY(-1, err);
    fd = core_alloc_fd(c, s, 0);
    if (fd < 0) {
        sock_destroy(s);
        REPLY(-1, PN_EMFILE);
    }
    if (c->oldsockaddr)
        s->flags |= SF_OLDSOCKADDR;
    REPLY(fd, 0);
}

static BOOL op_connect(struct PnClient *c, struct PnSock *s, struct PnReq *r)
{
    ip_addr_t ip;
    u16_t port;
    BOOL old;
    err_t e;

    if (r->progress == 0) {
        if (!sa_get((UBYTE *)r->a[0], (LONG)r->a[1], &ip, &port, &old))
            REPLY(-1, r->a[0] ? PN_EAFNOSUPPORT : PN_EFAULT);
        if (old) { s->flags |= SF_OLDSOCKADDR; c->oldsockaddr = TRUE; }

        if (s->type == PN_SOCK_DGRAM) {
            e = udp_connect(s->pcb.udp, &ip, port);
            REPLY(e ? -1 : 0, err2errno(e));
        }
        if (s->type == PN_SOCK_RAW) {
            e = raw_connect(s->pcb.raw, &ip);
            REPLY(e ? -1 : 0, err2errno(e));
        }
        if (s->flags & SF_CONNECTED)  REPLY(-1, PN_EISCONN);
        if (s->flags & SF_LISTENING)  REPLY(-1, PN_EOPNOTSUPP);
        if (s->flags & SF_CONNECTING) {
            if (s->flags & SF_NBIO)
                REPLY(-1, PN_EALREADY);
        } else {
            if (!s->pcb.tcp)
                REPLY(-1, s->so_error ? take_error(s) : PN_EINVAL);
            if (s->keepalive) ip_set_option(s->pcb.tcp, SOF_KEEPALIVE);
            if (!core_have_route(ip4_addr_get_u32(ip_2_ip4(&ip))))
                REPLY(-1, PN_EHOSTUNREACH);
            e = tcp_connect(s->pcb.tcp, &ip, port, tcp_connected_cb);
            if (e != ERR_OK)
                REPLY(-1, err2errno(e));
            s->flags |= SF_CONNECTING;
            if (s->flags & SF_NBIO)
                REPLY(-1, PN_EINPROGRESS);
        }
        r->progress = 1;
        return FALSE;
    }
    if (s->flags & SF_CONNECTED)
        REPLY(0, 0);
    if (s->so_error)
        REPLY(-1, take_error(s));
    if (!(s->flags & SF_CONNECTING))
        REPLY(-1, PN_ECONNABORTED);
    return FALSE;
}

static BOOL op_bind(struct PnClient *c, struct PnSock *s, struct PnReq *r)
{
    ip_addr_t ip;
    u16_t port;
    BOOL old;
    err_t e = ERR_VAL;

    if (!sa_get((UBYTE *)r->a[0], (LONG)r->a[1], &ip, &port, &old))
        REPLY(-1, PN_EAFNOSUPPORT);
    if (old) { s->flags |= SF_OLDSOCKADDR; c->oldsockaddr = TRUE; }
    switch (s->type) {
    case PN_SOCK_STREAM:
        if (!s->pcb.tcp || (s->flags & (SF_LISTENING | SF_CONNECTED)))
            REPLY(-1, PN_EINVAL);
        if (s->reuseaddr) ip_set_option(s->pcb.tcp, SOF_REUSEADDR);
        e = tcp_bind(s->pcb.tcp, &ip, port);
        break;
    case PN_SOCK_DGRAM:
        if (s->reuseaddr) ip_set_option(s->pcb.udp, SOF_REUSEADDR);
        e = udp_bind(s->pcb.udp, &ip, port);
        break;
    case PN_SOCK_RAW:
        e = raw_bind(s->pcb.raw, &ip);
        break;
    }
    if (e != ERR_OK)
        REPLY(-1, e == ERR_USE ? PN_EADDRINUSE : err2errno(e));
    s->flags |= SF_BOUND;
    REPLY(0, 0);
}

static BOOL op_listen(struct PnSock *s, struct PnReq *r)
{
    struct tcp_pcb *lpcb;
    LONG backlog = (LONG)r->a[0];
    err_t e;

    if (s->type != PN_SOCK_STREAM)
        REPLY(-1, PN_EOPNOTSUPP);
    if (backlog < 1) backlog = 1;
    if (backlog > PN_MAX_BACKLOG) backlog = PN_MAX_BACKLOG;
    s->backlog = backlog;
    if (s->flags & SF_LISTENING)
        REPLY(0, 0);
    if (!s->pcb.tcp || (s->flags & (SF_CONNECTED | SF_CONNECTING)))
        REPLY(-1, PN_EINVAL);
    if (!(s->flags & SF_BOUND)) {
        e = tcp_bind(s->pcb.tcp, IP_ANY_TYPE, 0);
        if (e != ERR_OK)
            REPLY(-1, err2errno(e));
        s->flags |= SF_BOUND;
    }
    lpcb = tcp_listen_with_backlog_and_err(s->pcb.tcp, (u8_t)backlog, &e);
    if (!lpcb)
        REPLY(-1, err2errno(e));
    s->pcb.tcp = lpcb;          /* the old pcb was freed by tcp_listen */
    tcp_arg(lpcb, s);
    tcp_accept(lpcb, tcp_accept_cb);
    s->flags |= SF_LISTENING;
    REPLY(0, 0);
}

static BOOL op_accept(struct PnClient *c, struct PnSock *s, struct PnReq *r)
{
    struct PnSock *ns;
    LONG fd;
    int i;

    if (!(s->flags & SF_LISTENING))
        REPLY(-1, PN_EINVAL);
    if (s->acc_count == 0) {
        if (!s->pcb.tcp)
            REPLY(-1, s->so_error ? take_error(s) : PN_ECONNABORTED);
        if (s->flags & SF_NBIO)
            REPLY(-1, PN_EWOULDBLOCK);
        return FALSE;
    }
    ns = s->accq[0];
    for (i = 1; i < s->acc_count; i++)
        s->accq[i - 1] = s->accq[i];
    s->acc_count--;

    ns->owner = c;
    fd = core_alloc_fd(c, ns, 0);
    if (fd < 0) {
        sock_destroy(ns);
        REPLY(-1, PN_EMFILE);
    }
    if (ns->pcb.tcp)
        sa_put((UBYTE *)r->a[0], (LONG *)r->a[1], &ns->pcb.tcp->remote_ip,
               ns->pcb.tcp->remote_port, (ns->flags & SF_OLDSOCKADDR) != 0);
    else
        sa_put((UBYTE *)r->a[0], (LONG *)r->a[1], NULL, 0, (ns->flags & SF_OLDSOCKADDR) != 0);
    REPLY(fd, 0);
}

static BOOL op_send(struct PnClient *c, struct PnSock *s, struct PnReq *r)
{
    const UBYTE *buf = (const UBYTE *)r->a[0];
    ULONG len = r->a[1];
    BOOL dontwait = (s->flags & SF_NBIO) || (r->a[2] & PN_MSG_DONTWAIT);
    ip_addr_t ip;
    u16_t port;
    BOOL old, wrote = FALSE;
    struct pbuf *p;
    err_t e;

    if ((LONG)len < 0)                        /* send(fd, buf, -1, 0) is not 4 GB */
        REPLY(-1, PN_EINVAL);
    if (!buf && len)
        REPLY(-1, PN_EFAULT);

    if (s->type != PN_SOCK_STREAM) {
        BOOL have_dest = r->a[3] != 0;
        if (have_dest) {
            if (!sa_get((UBYTE *)r->a[3], (LONG)r->a[4], &ip, &port, &old))
                REPLY(-1, PN_EAFNOSUPPORT);
            if (!core_have_route(ip4_addr_get_u32(ip_2_ip4(&ip))))
                REPLY(-1, PN_EHOSTUNREACH);
            if (old) { s->flags |= SF_OLDSOCKADDR; c->oldsockaddr = TRUE; }
        }
        if (len > 65000)
            REPLY(-1, PN_EMSGSIZE);
        p = pbuf_alloc(s->type == PN_SOCK_DGRAM ? PBUF_TRANSPORT : PBUF_IP, (u16_t)len, PBUF_RAM);
        if (!p)
            REPLY(-1, PN_ENOBUFS);
        if (len)
            pbuf_take(p, buf, (u16_t)len);
        if (s->type == PN_SOCK_DGRAM) {
            if (have_dest)
                e = udp_sendto(s->pcb.udp, p, &ip, port);
            else if (s->pcb.udp->flags & UDP_FLAGS_CONNECTED)
                e = udp_send(s->pcb.udp, p);
            else
                e = ERR_CONN;
        } else {
            e = have_dest ? raw_sendto(s->pcb.raw, p, &ip) : raw_send(s->pcb.raw, p);
        }
        pbuf_free(p);
        if (e == ERR_CONN)
            REPLY(-1, PN_EDESTADDRREQ);
        REPLY(e ? -1 : (LONG)len, err2errno(e));
    }

    /* TCP */
    if (s->flags & SF_WR_SHUT)
        REPLY(-1, PN_EPIPE);
    if (!s->pcb.tcp) {
        if (r->progress) REPLY((LONG)r->progress, 0);
        REPLY(-1, s->so_error ? take_error(s) : PN_EPIPE);
    }
    if (!(s->flags & SF_CONNECTED))
        REPLY(-1, (s->flags & SF_CONNECTING) && dontwait ? PN_EWOULDBLOCK : PN_ENOTCONN);

    while (r->progress < len) {
        u32_t room  = len - r->progress;
        u16_t avail = tcp_sndbuf(s->pcb.tcp);
        u16_t n;

        if (avail == 0)
            break;
        n = room < avail ? (u16_t)room : avail;
        e = tcp_write(s->pcb.tcp, buf + r->progress, n,
                      TCP_WRITE_FLAG_COPY | (room > n ? TCP_WRITE_FLAG_MORE : 0));
        if (e == ERR_MEM)
            break;              /* queue full: wait for acks */
        if (e != ERR_OK) {
            if (r->progress) REPLY((LONG)r->progress, 0);
            REPLY(-1, err2errno(e));
        }
        r->progress += n;
        wrote = TRUE;
    }
    if (wrote)
        tcp_output(s->pcb.tcp);
    if (r->progress >= len)
        REPLY((LONG)len, 0);
    if (dontwait || deadline_passed(r, s->sndtimeo)) {
        if (r->progress) REPLY((LONG)r->progress, 0);
        REPLY(-1, PN_EWOULDBLOCK);
    }
    return FALSE;
}

static BOOL op_recv(struct PnSock *s, struct PnReq *r)
{
    UBYTE *buf = (UBYTE *)r->a[0];
    ULONG len = r->a[1], flags = r->a[2];
    BOOL dontwait = (s->flags & SF_NBIO) || (flags & PN_MSG_DONTWAIT);
    BOOL old = (s->flags & SF_OLDSOCKADDR) != 0;

    if ((LONG)len < 0)
        REPLY(-1, PN_EINVAL);
    if (!buf && len)
        REPLY(-1, PN_EFAULT);

    if (len == 0)
        REPLY(0, 0);

    if (s->type != PN_SOCK_STREAM) {
        struct PnDgram *d = s->dg_head;
        if (d) {
            u16_t n = d->p->tot_len < len ? d->p->tot_len : (u16_t)len;
            pbuf_copy_partial(d->p, buf, n, 0);
            sa_put((UBYTE *)r->a[3], (LONG *)r->a[4], &d->addr, d->port, old);
            if (!(flags & PN_MSG_PEEK)) {
                s->dg_head = d->next;
                if (!s->dg_head) s->dg_tail = NULL;
                s->dg_count--;
                pbuf_free(d->p);
                pn_free(d);
            }
            REPLY(n, 0);
        }
        if (s->flags & SF_RD_SHUT)
            REPLY(0, 0);
        if (dontwait || deadline_passed(r, s->rcvtimeo))
            REPLY(-1, PN_EWOULDBLOCK);
        return FALSE;
    }

    /* TCP */
    if (s->flags & SF_LISTENING)
        REPLY(-1, PN_ENOTCONN);
    while (s->rxq && r->progress < len) {
        u32_t want = len - r->progress;
        u16_t n = s->rxq->tot_len < want ? s->rxq->tot_len : (u16_t)(want > 0xffff ? 0xffff : want);

        if (flags & PN_MSG_PEEK) {
            pbuf_copy_partial(s->rxq, buf + r->progress, n, 0);
            r->progress += n;
            break;
        }
        pbuf_copy_partial(s->rxq, buf + r->progress, n, 0);
        s->rxq = pbuf_free_header(s->rxq, n);
        if (s->pcb.tcp)
            tcp_recved(s->pcb.tcp, n);
        r->progress += n;
    }
    if (r->progress > 0 &&
        (r->progress >= len || !(flags & PN_MSG_WAITALL) || (flags & PN_MSG_PEEK) ||
         (s->flags & SF_RD_EOF) || dontwait)) {
        if (r->a[3] && s->pcb.tcp)
            sa_put((UBYTE *)r->a[3], (LONG *)r->a[4], &s->pcb.tcp->remote_ip,
                   s->pcb.tcp->remote_port, old);
        REPLY((LONG)r->progress, 0);
    }
    if (s->flags & (SF_RD_EOF | SF_RD_SHUT)) {
        if (s->so_error && r->progress == 0)
            REPLY(-1, take_error(s));
        REPLY((LONG)r->progress, 0);
    }
    if (!(s->flags & (SF_CONNECTED | SF_CONNECTING)))
        REPLY(-1, PN_ENOTCONN);
    if (dontwait || deadline_passed(r, s->rcvtimeo)) {
        if (r->progress) REPLY((LONG)r->progress, 0);
        REPLY(-1, PN_EWOULDBLOCK);
    }
    return FALSE;
}

static BOOL op_shutdown(struct PnSock *s, struct PnReq *r)
{
    LONG how = (LONG)r->a[0];
    int rd = (how == 0 || how == 2), wr = (how == 1 || how == 2);

    if (how < 0 || how > 2)
        REPLY(-1, PN_EINVAL);
    if (s->type == PN_SOCK_STREAM) {
        if (!(s->flags & SF_CONNECTED))
            REPLY(-1, PN_ENOTCONN);
        if (s->pcb.tcp && wr) {               /* rd is handled locally */
            err_t e = tcp_shutdown(s->pcb.tcp, 0, 1);
            if (e != ERR_OK)                  /* no FIN went out: keep writable */
                REPLY(-1, err2errno(e));
        }
    }
    if (rd) {
        s->flags |= SF_RD_SHUT;
        if (s->rxq) {
            if (s->pcb.tcp) tcp_recved(s->pcb.tcp, s->rxq->tot_len);
            pbuf_free(s->rxq);
            s->rxq = NULL;
        }
    }
    if (wr)
        s->flags |= SF_WR_SHUT;
    REPLY(0, 0);
}

static ULONG tv2ms(const ULONG *tv)
{
    /* deadlines are compared as signed 32-bit differences: anything beyond
     * ~12 days is "forever" and must not wrap into the past */
    if (tv[0] >= 0x3fffffffUL / 1000)
        return 0x3fffffffUL;
    return tv[0] * 1000 + (tv[1] + 999) / 1000;
}

static BOOL op_setsockopt(struct PnSock *s, struct PnReq *r)
{
    LONG level = (LONG)r->a[0], name = (LONG)r->a[1], len = (LONG)r->a[3];
    const LONG *val = (const LONG *)r->a[2];
    LONG v;

    if (!val || len < 4)                      /* char one = 1; setsockopt(..., &one, 1) */
        REPLY(-1, PN_EINVAL);
    v = *val;
    if (level == PN_SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR_:
        case SO_REUSEPORT_:
            s->reuseaddr = v != 0;
            REPLY(0, 0);
        case SO_KEEPALIVE_:
            s->keepalive = v != 0;
            if (s->type == PN_SOCK_STREAM && s->pcb.tcp) {
                if (v) ip_set_option(s->pcb.tcp, SOF_KEEPALIVE);
                else   ip_reset_option(s->pcb.tcp, SOF_KEEPALIVE);
            }
            REPLY(0, 0);
        case SO_BROADCAST_:
            s->broadcast = v != 0;
            REPLY(0, 0);
        case SO_RCVTIMEO_:
        case SO_SNDTIMEO_:
            if (!val || len < 8)
                REPLY(-1, PN_EINVAL);
            if (name == SO_RCVTIMEO_) s->rcvtimeo = tv2ms((const ULONG *)val);
            else                      s->sndtimeo = tv2ms((const ULONG *)val);
            REPLY(0, 0);
        case SO_EVENTMASK_:
            if (!s->eventmask && (v & 0x7f)) ev_sockets++;
            if (s->eventmask && !(v & 0x7f)) ev_sockets--;
            s->eventmask   = (ULONG)v & 0x7f;
            s->ev_reported = 0;         /* report conditions that hold now */
            s->events     &= s->eventmask;
            REPLY(0, 0);
        case SO_LINGER_:                /* struct linger { LONG l_onoff, l_linger; } */
            if (len < 8) REPLY(-1, PN_EINVAL);
            s->linger_abort = val[0] != 0 && val[1] == 0;
            s->linger_sec   = val[0] ? (UWORD)(val[1] < 0 ? 0 : val[1] > 65535 ? 65535 : val[1]) : 0;
            if (!val[0]) s->linger_sec = 0;
            REPLY(0, 0);
        case SO_SNDBUF_:
            if (s->type == PN_SOCK_STREAM)
                set_sndbuf(s, s->pcb.tcp && !(s->flags & SF_LISTENING) ? s->pcb.tcp : NULL, v);
            REPLY(0, 0);
        case SO_DEBUG_: case SO_DONTROUTE_: case SO_OOBINLINE_:
        case SO_RCVBUF_: case SO_SNDLOWAT_: case SO_RCVLOWAT_:
            REPLY(0, 0);        /* accepted, no effect */
        }
        REPLY(-1, PN_ENOPROTOOPT);
    }
    if (level == PN_IPPROTO_TCP && s->type == PN_SOCK_STREAM && name == TCP_NODELAY_) {
        s->nodelay = v != 0;
        if (s->pcb.tcp && !(s->flags & SF_LISTENING)) {
            if (v) tcp_nagle_disable(s->pcb.tcp);
            else   tcp_nagle_enable(s->pcb.tcp);
        }
        REPLY(0, 0);
    }
    if (level == PN_IPPROTO_IP) {
        if (name == IP_TTL_ || name == IP_TOS_) {
            u8_t v8 = (u8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            if (name == IP_TTL_ && v8 == 0)
                REPLY(-1, PN_EINVAL);
            /* ttl/tos live in IP_PCB, common to tcp_pcb/udp_pcb/raw_pcb */
            if (s->pcb.any) {
                if (s->type == PN_SOCK_STREAM) {
                    if (name == IP_TTL_) s->pcb.tcp->ttl = v8; else s->pcb.tcp->tos = v8;
                } else if (s->type == PN_SOCK_DGRAM) {
                    if (name == IP_TTL_) s->pcb.udp->ttl = v8; else s->pcb.udp->tos = v8;
                } else {
                    if (name == IP_TTL_) s->pcb.raw->ttl = v8; else s->pcb.raw->tos = v8;
                }
            }
            REPLY(0, 0);
        }
        if (name == IP_HDRINCL_) {            /* raw: the caller builds the IP header */
            if (s->type != PN_SOCK_RAW || !s->pcb.raw)
                REPLY(-1, PN_ENOPROTOOPT);
            raw_setflags(s->pcb.raw, v ? (raw_flags(s->pcb.raw) | RAW_FLAGS_HDRINCL)
                                       : (raw_flags(s->pcb.raw) & ~RAW_FLAGS_HDRINCL));
            REPLY(0, 0);
        }
    }
    REPLY(-1, PN_ENOPROTOOPT);                /* so programs can fall back, not send junk */
}

static BOOL op_getsockopt(struct PnSock *s, struct PnReq *r)
{
    LONG level = (LONG)r->a[0], name = (LONG)r->a[1];
    LONG *val = (LONG *)r->a[2], *lenp = (LONG *)r->a[3];
    LONG v;

    if (!val || !lenp || *lenp < 4)
        REPLY(-1, PN_EINVAL);
    if (level == PN_SOL_SOCKET) {
        switch (name) {
        case SO_ERROR_:      v = take_error(s); break;
        case SO_TYPE_:       v = s->type; break;
        case SO_REUSEADDR_:  v = s->reuseaddr ? name : 0; break;
        case SO_KEEPALIVE_:  v = s->keepalive ? name : 0; break;
        case SO_BROADCAST_:  v = s->broadcast ? name : 0; break;
        case SO_ACCEPTCONN_: v = (s->flags & SF_LISTENING) ? name : 0; break;
        case SO_SNDBUF_:     v = sock_sndbuf(s); break;
        case SO_RCVBUF_:     v = TCP_WND; break;
        case SO_EVENTMASK_:  v = s->eventmask; break;
        case SO_LINGER_:
            if (*lenp >= 8) {
                val[0] = s->linger_abort || s->linger_sec;
                val[1] = s->linger_sec;
                *lenp = 8;
                REPLY(0, 0);
            }
            REPLY(-1, PN_EINVAL);
        case SO_RCVTIMEO_:
        case SO_SNDTIMEO_: {
            ULONG ms = name == SO_RCVTIMEO_ ? s->rcvtimeo : s->sndtimeo;
            if (*lenp < 8) REPLY(-1, PN_EINVAL);
            val[0] = ms / 1000; val[1] = (ms % 1000) * 1000; *lenp = 8;
            REPLY(0, 0);
        }
        default:
            REPLY(-1, PN_ENOPROTOOPT);
        }
    } else if (level == PN_IPPROTO_TCP && name == TCP_NODELAY_) {
        v = s->nodelay ? 1 : 0;
    } else if (level == PN_IPPROTO_IP && (name == IP_TTL_ || name == IP_TOS_) && s->pcb.any) {
        if (s->type == PN_SOCK_STREAM)     v = name == IP_TTL_ ? s->pcb.tcp->ttl : s->pcb.tcp->tos;
        else if (s->type == PN_SOCK_DGRAM) v = name == IP_TTL_ ? s->pcb.udp->ttl : s->pcb.udp->tos;
        else                               v = name == IP_TTL_ ? s->pcb.raw->ttl : s->pcb.raw->tos;
    } else if (level == PN_IPPROTO_IP && name == IP_HDRINCL_ && s->type == PN_SOCK_RAW && s->pcb.raw) {
        v = (raw_flags(s->pcb.raw) & RAW_FLAGS_HDRINCL) ? 1 : 0;
    } else {
        REPLY(-1, PN_ENOPROTOOPT);
    }
    *val = v;
    *lenp = 4;
    REPLY(0, 0);
}

static BOOL op_name(struct PnSock *s, struct PnReq *r, BOOL peer)
{
    BOOL old = (s->flags & SF_OLDSOCKADDR) != 0;
    UBYTE *sa = (UBYTE *)r->a[0];
    LONG *lenp = (LONG *)r->a[1];

    switch (s->type) {
    case PN_SOCK_STREAM:
        if (!s->pcb.tcp) {
            if (peer) REPLY(-1, PN_ENOTCONN);
            sa_put(sa, lenp, NULL, 0, old);
            REPLY(0, 0);
        }
        if (peer) {
            if (!(s->flags & SF_CONNECTED) || (s->flags & SF_LISTENING))
                REPLY(-1, PN_ENOTCONN);
            sa_put(sa, lenp, &s->pcb.tcp->remote_ip, s->pcb.tcp->remote_port, old);
        } else {
            /* local_ip/local_port share layout in tcp_pcb and tcp_pcb_listen */
            sa_put(sa, lenp, &s->pcb.tcp->local_ip, s->pcb.tcp->local_port, old);
        }
        REPLY(0, 0);
    case PN_SOCK_DGRAM:
        if (peer) {
            if (!(s->pcb.udp->flags & UDP_FLAGS_CONNECTED))
                REPLY(-1, PN_ENOTCONN);
            sa_put(sa, lenp, &s->pcb.udp->remote_ip, s->pcb.udp->remote_port, old);
        } else {
            sa_put(sa, lenp, &s->pcb.udp->local_ip, s->pcb.udp->local_port, old);
        }
        REPLY(0, 0);
    default:
        if (peer)
            sa_put(sa, lenp, &s->pcb.raw->remote_ip, 0, old);
        else
            sa_put(sa, lenp, &s->pcb.raw->local_ip, 0, old);
        REPLY(0, 0);
    }
}

static BOOL op_ioctl(struct PnSock *s, struct PnReq *r)
{
    LONG *arg = (LONG *)r->a[1];

    switch (r->a[0]) {
    case PN_FIONBIO:
        if (!arg) REPLY(-1, PN_EFAULT);
        if (*arg) s->flags |= SF_NBIO; else s->flags &= ~SF_NBIO;
        REPLY(0, 0);
    case PN_FIOASYNC:
        if (!arg) REPLY(-1, PN_EFAULT);
        if (*arg) s->flags |= SF_ASYNC; else s->flags &= ~SF_ASYNC;
        REPLY(0, 0);
    case PN_FIONREAD:
        if (!arg) REPLY(-1, PN_EFAULT);
        *arg = s->rxq ? s->rxq->tot_len : s->dg_head ? s->dg_head->p->tot_len : 0;
        REPLY(0, 0);
    case PN_SIOCATMARK:
        if (!arg) REPLY(-1, PN_EFAULT);
        *arg = 0;
        REPLY(0, 0);
    }
    REPLY(-1, PN_EINVAL);
}

static BOOL op_dup2(struct PnClient *c, struct PnReq *r)
{
    LONG err, newfd = (LONG)r->a[0];
    struct PnSock *s = core_fd2sock(c, r->fd, &err);

    if (!s)
        REPLY(-1, err);
    if (newfd < 0) {
        newfd = core_alloc_fd(c, s, 0);
        REPLY(newfd, newfd < 0 ? PN_EMFILE : 0);
    }
    if (newfd >= c->nfds)
        REPLY(-1, PN_EBADF);
    if (c->fds[newfd] == s)
        REPLY(newfd, 0);
    if (c->fds[newfd]) {
        core_fail_pending_fd(c, newfd);
        sock_unref(c->fds[newfd]);
    }
    c->fds[newfd] = s;
    s->refs++;
    REPLY(newfd, 0);
}

static struct PnRelease *find_release(LONG id)
{
    struct MinNode *n;
    for (n = released.mlh_Head; n->mln_Succ; n = n->mln_Succ)
        if (((struct PnRelease *)n)->id == id)
            return (struct PnRelease *)n;
    return NULL;
}

static BOOL op_release(struct PnClient *c, struct PnReq *r)
{
    LONG err, id = (LONG)r->a[0];
    BOOL copy = r->a[1] != 0;
    struct PnSock *s = core_fd2sock(c, r->fd, &err);
    struct PnRelease *rel;

    if (!s)
        REPLY(-1, err);
    if (id == -1) {                         /* UNIQUE_ID */
        do id = core.next_release_id++; while (find_release(id));
    } else if (find_release(id)) {
        REPLY(-1, PN_EINVAL);
    }
    rel = pn_alloc(sizeof(*rel), MEMF_CLEAR);
    if (!rel)
        REPLY(-1, PN_ENOMEM);
    rel->s  = s;
    rel->id = id;
    AddTail((struct List *)&released, (struct Node *)&rel->node);
    if (copy)
        s->refs++;                          /* the fd keeps its reference */
    else
        c->fds[r->fd] = NULL;               /* reference moves to the list */
    REPLY(id, 0);
}

static BOOL op_obtain(struct PnClient *c, struct PnReq *r)
{
    struct PnRelease *rel = find_release((LONG)r->a[0]);
    LONG fd;

    if (!rel)
        REPLY(-1, PN_EINVAL);
    if (r->a[2] && r->a[2] != rel->s->type)
        REPLY(-1, PN_EPROTOTYPE);
    fd = core_alloc_fd(c, rel->s, 0);
    if (fd < 0)
        REPLY(-1, PN_EMFILE);
    rel->s->owner = c;
    rel->s->refs--;                         /* list's reference handed over */
    Remove((struct Node *)&rel->node);
    pn_free(rel);
    REPLY(fd, 0);
}

BOOL sock_op(struct PnClient *c, struct PnReq *r)
{
    struct PnSock *s;
    LONG err;

    switch (r->op) {
    case PNOP_SOCKET:  return op_socket(c, r);
    case PNOP_DUP2:    return op_dup2(c, r);
    case PNOP_OBTAIN:  return op_obtain(c, r);
    case PNOP_RELEASE: return op_release(c, r);
    case PNOP_CLOSE:
        if (!(s = core_fd2sock(c, r->fd, &err)))
            REPLY(-1, err);
        c->fds[r->fd] = NULL;
        core_fail_pending_fd(c, r->fd);
        sock_unref(s);
        REPLY(0, 0);
    default:
        break;
    }

    if (!(s = core_fd2sock(c, r->fd, &err)))
        REPLY(-1, err);
    sock_check_pcb(s);

    switch (r->op) {
    case PNOP_CONNECT:     return op_connect(c, s, r);
    case PNOP_BIND:        return op_bind(c, s, r);
    case PNOP_LISTEN:      return op_listen(s, r);
    case PNOP_ACCEPT:      return op_accept(c, s, r);
    case PNOP_SEND:        return op_send(c, s, r);
    case PNOP_RECV:        return op_recv(s, r);
    case PNOP_SHUTDOWN:    return op_shutdown(s, r);
    case PNOP_SETSOCKOPT:  return op_setsockopt(s, r);
    case PNOP_GETSOCKOPT:  return op_getsockopt(s, r);
    case PNOP_GETSOCKNAME: return op_name(s, r, FALSE);
    case PNOP_GETPEERNAME: return op_name(s, r, TRUE);
    case PNOP_IOCTL:       return op_ioctl(s, r);
    }
    REPLY(-1, PN_EINVAL);
}

/* ---------------------------------------------------------------------- */
/* WaitSelect                                                               */
/* ---------------------------------------------------------------------- */

BOOL sock_select(struct PnClient *c, struct PnReq *r, BOOL final)
{
    LONG nfds = (LONG)r->a[0];
    ULONG *rd = (ULONG *)r->a[1], *wr = (ULONG *)r->a[2], *ex = (ULONG *)r->a[3];
    const ULONG *tv = (const ULONG *)r->a[4];
    ULONG ro[PN_MAX_DTABLESIZE / 32], wo[PN_MAX_DTABLESIZE / 32];
    LONG fd, n = 0, words;

    if (nfds < 0)
        REPLY(-1, PN_EINVAL);
    for (fd = c->nfds; fd < nfds; fd++) {      /* no descriptor can be up there */
        ULONG bit = 1UL << (fd & 31);
        LONG w = fd >> 5;
        if ((rd && (rd[w] & bit)) || (wr && (wr[w] & bit)) || (ex && (ex[w] & bit)))
            REPLY(-1, PN_EBADF);
    }
    if (nfds > c->nfds)
        nfds = c->nfds;
    words = (nfds + 31) / 32;
    memset(ro, 0, sizeof(ro));
    memset(wo, 0, sizeof(wo));

    for (fd = 0; fd < nfds; fd++) {
        ULONG bit = 1UL << (fd & 31);
        LONG w = fd >> 5;
        BOOL want_r = rd && (rd[w] & bit), want_w = wr && (wr[w] & bit);
        BOOL want_e = ex && (ex[w] & bit);
        struct PnSock *s;

        if (!want_r && !want_w && !want_e)
            continue;
        s = c->fds[fd];
        if (!s)
            REPLY(-1, PN_EBADF);
        if (want_r && readable(s)) { ro[w] |= bit; n++; }
        if (want_w && writable(s)) { wo[w] |= bit; n++; }
    }

    if (!final && n == 0) {
        if (!tv)
            return FALSE;                       /* wait forever */
        if (r->progress == 0 && (tv[0] || tv[1])) {
            r->progress = 1;
            core_set_deadline(r, tv2ms(tv));
            return FALSE;
        }
        if (r->has_deadline && (LONG)(sys_now() - r->deadline) < 0)
            return FALSE;
    }
    if (rd) memcpy(rd, ro, words * 4);
    if (wr) memcpy(wr, wo, words * 4);
    if (ex) memset(ex, 0, words * 4);
    REPLY(n, 0);
}

/* ---------------------------------------------------------------------- */
/* socket events (SO_EVENTMASK / GetSocketEvents)                           */
/* ---------------------------------------------------------------------- */

/* Signal owners whose sockets newly meet a selected condition. A condition
 * is reported once, and re-armed when it stops holding (edge-triggered). */
void sock_check_events(void)
{
    struct MinNode *n;

    if (ev_sockets == 0)                      /* most programs never use SO_EVENTMASK */
        return;
    for (n = core.sockets.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
        struct PnSock *s = (struct PnSock *)n;
        ULONG cond = 0, fresh;

        if (!s->eventmask || !s->owner || !s->owner->sigeventmask)
            continue;
        if (s->rxq || s->dg_head)                        cond |= FD_READ_;
        if (s->acc_count > 0)                            cond |= FD_ACCEPT_;
        if (s->so_error)                                 cond |= FD_ERROR_;
        if (s->type == PN_SOCK_STREAM) {
            if (s->flags & SF_CONNECTED)                 cond |= FD_CONNECT_;
            if (s->flags & SF_RD_EOF)                    cond |= FD_CLOSE_;
            if ((s->flags & SF_CONNECTED) && s->pcb.tcp && !(s->flags & SF_WR_SHUT) &&
                tcp_sndbuf(s->pcb.tcp) > 0)              cond |= FD_WRITE_;
        } else {
            cond |= FD_WRITE_;
        }
        fresh = cond & s->eventmask & ~s->ev_reported;
        s->ev_reported = (s->ev_reported & cond) | fresh;
        if (fresh) {
            s->events |= fresh;
            Signal(s->owner->task, s->owner->sigeventmask);
        }
    }
}

/* A client is going away: sockets it released stay, but not its identity. */
void sock_forget_owner(struct PnClient *c)
{
    struct MinNode *n;
    for (n = core.sockets.mlh_Head; n->mln_Succ; n = n->mln_Succ)
        if (((struct PnSock *)n)->owner == c)
            ((struct PnSock *)n)->owner = NULL;
}
/* ---------------------------------------------------------------------- */
/* socket list for PrismNet NETSTAT / PrismMon                              */
/* ---------------------------------------------------------------------- */

int sock_list(struct PnSockInfo *arr, int max)
{
    struct MinNode *n;
    int count = 0;

    for (n = core.sockets.mlh_Head; n->mln_Succ && count < max; n = n->mln_Succ) {
        struct PnSock *s = (struct PnSock *)n;
        struct PnSockInfo *si = &arr[count++];
        const char *owner = "(released)";

        memset(si, 0, sizeof(*si));
        if (s->owner && s->owner->task && s->owner->task->tc_Node.ln_Name)
            owner = s->owner->task->tc_Node.ln_Name;
        strncpy(si->owner, owner, sizeof(si->owner) - 1);
        si->type = s->type;

        if (s->type == PN_SOCK_STREAM) {
            struct tcp_pcb *p = s->pcb.tcp;
            if (s->flags & SF_LISTENING)        si->state = PNSS_LISTEN;
            else if (s->flags & SF_CONNECTING)  si->state = PNSS_CONNECTING;
            else if (s->flags & SF_RD_EOF)      si->state = PNSS_CLOSING;
            else if (s->flags & SF_CONNECTED)   si->state = PNSS_ESTABLISHED;
            if (p) {
                /* local_ip/local_port share layout in tcp_pcb and tcp_pcb_listen */
                si->lip   = ip4_addr_get_u32(ip_2_ip4(&p->local_ip));
                si->lport = p->local_port;
                if (!(s->flags & SF_LISTENING)) {
                    si->rip   = ip4_addr_get_u32(ip_2_ip4(&p->remote_ip));
                    si->rport = p->remote_port;
                    if (s->flags & SF_CONNECTED)
                        si->txq = sock_sndbuf(s) > tcp_sndbuf(p) ? sock_sndbuf(s) - tcp_sndbuf(p) : 0;
                }
            }
            si->rxq = s->rxq ? s->rxq->tot_len : 0;
        } else {
            struct PnDgram *d;
            if (s->type == PN_SOCK_DGRAM && s->pcb.udp) {
                si->lip   = ip4_addr_get_u32(ip_2_ip4(&s->pcb.udp->local_ip));
                si->lport = s->pcb.udp->local_port;
                if (s->pcb.udp->flags & UDP_FLAGS_CONNECTED) {
                    si->rip   = ip4_addr_get_u32(ip_2_ip4(&s->pcb.udp->remote_ip));
                    si->rport = s->pcb.udp->remote_port;
                }
            } else if (s->pcb.raw) {
                si->lip = ip4_addr_get_u32(ip_2_ip4(&s->pcb.raw->local_ip));
                si->lport = s->proto;
            }
            si->state = si->lport ? PNSS_BOUND : PNSS_NONE;
            for (d = s->dg_head; d; d = d->next)
                si->rxq += d->p->tot_len;
        }
    }
    return count;
}
