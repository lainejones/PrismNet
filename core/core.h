/*
 * PrismNet core task - internal definitions.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_CORE_H
#define PRISMNET_CORE_H

#include <exec/types.h>
#include <exec/lists.h>

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"

#include "prismnet/pnmsg.h"

#define PN_DEFAULT_DTABLESIZE  64
#define PN_MAX_DTABLESIZE      256
#define PN_MAX_DGRAMS          16      /* queued datagrams per socket */
#define PN_MAX_BACKLOG         16

/* AmiTCP / BSD constants used by the core (wire values) */
#define PN_AF_INET        2
#define PN_SOCK_STREAM    1
#define PN_SOCK_DGRAM     2
#define PN_SOCK_RAW       3
#define PN_IPPROTO_IP     0
#define PN_IPPROTO_ICMP   1
#define PN_IPPROTO_TCP    6
#define PN_IPPROTO_UDP    17
#define PN_SOL_SOCKET     0xffff
#define PN_MSG_OOB        0x01
#define PN_MSG_PEEK       0x02
#define PN_MSG_WAITALL    0x40
#define PN_MSG_DONTWAIT   0x80

/* PnSock.flags */
#define SF_NBIO        0x0001
#define SF_LISTENING   0x0002
#define SF_CONNECTING  0x0004
#define SF_CONNECTED   0x0008
#define SF_RD_EOF      0x0010   /* peer sent FIN / socket dead */
#define SF_WR_SHUT     0x0020
#define SF_RD_SHUT     0x0040
#define SF_ASYNC       0x0080
#define SF_OLDSOCKADDR 0x0100   /* app uses 4.3BSD sockaddr (no sin_len) */
#define SF_BOUND       0x0200

struct PnDgram {
    struct PnDgram *next;
    struct pbuf    *p;
    ip_addr_t       addr;
    u16_t           port;
};

struct PnClient;

struct PnSock {
    struct MinNode     node;        /* core.sockets */
    struct PnClient   *owner;       /* NULL while released (ObtainSocket) */
    LONG               refs;        /* fd table references */
    UBYTE              type;        /* PN_SOCK_* */
    UBYTE              proto;
    UWORD              flags;
    union {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
        struct raw_pcb *raw;
        void           *any;
    } pcb;
    struct pbuf       *rxq;         /* TCP: unread stream data */
    struct PnDgram    *dg_head, *dg_tail;
    int                dg_count;
    struct PnSock     *accq[PN_MAX_BACKLOG];
    int                acc_count, backlog;
    LONG               so_error;    /* pending asynchronous error (errno) */
    ULONG              rcvtimeo, sndtimeo;   /* ms, 0 = none */
    BOOL               keepalive, reuseaddr, broadcast, nodelay;
    BOOL               linger_abort;   /* SO_LINGER {on, 0}: close sends RST */
    UWORD              linger_sec;     /* SO_LINGER as set (reported back) */
    UWORD              sndbuf;         /* SO_SNDBUF as set, 0 = TCP_SND_BUF */
    LONG               release_id;
    ULONG              eventmask;   /* SO_EVENTMASK: FD_* the owner wants */
    ULONG              ev_reported; /* conditions already signalled (edge detect) */
    ULONG              events;      /* pending, collected by GetSocketEvents */
};

struct PnClient {
    struct MinNode   node;          /* core.clients */
    struct Task     *task;
    struct PnSock  **fds;
    LONG             nfds;
    BOOL             oldsockaddr;   /* last sockaddr seen had sin_len == 0 */
    ULONG            sigeventmask;  /* SBTC_SIGEVENTMASK: signal for socket events */
};

struct PnCore {
    struct MinList   clients;
    struct MinList   sockets;
    struct MinList   pending;       /* PnReq waiting for an event */
    char             hostname[64];
    char             domain[64];      /* resolver search domain */
    LONG             next_release_id;
};

extern struct PnCore core;

/* NewList() lives in amiga.lib; initialise lists by hand. */
static inline void pn_newlist(struct MinList *l)
{
    l->mlh_Head     = (struct MinNode *)&l->mlh_Tail;
    l->mlh_Tail     = NULL;
    l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

/* core.c */
void   core_reply(struct PnReq *r, LONG result, LONG error);
void   core_pend(struct PnReq *r);
void   core_set_deadline(struct PnReq *r, ULONG ms);
struct PnSock *core_fd2sock(struct PnClient *c, LONG fd, LONG *err);
LONG   core_alloc_fd(struct PnClient *c, struct PnSock *s, LONG min);

/* csock.c - each op either replies (returns TRUE) or wants to pend (FALSE) */
struct PnSock *sock_new(struct PnClient *owner, LONG type, LONG proto, LONG *err);
void   sock_unref(struct PnSock *s);
BOOL   sock_op(struct PnClient *c, struct PnReq *r);
BOOL   sock_select(struct PnClient *c, struct PnReq *r, BOOL final);
void   sock_abort_pending(struct PnReq *r);
void   core_fail_pending_fd(struct PnClient *c, LONG fd);
BOOL   core_have_route(ULONG dest);         /* network order */
LONG   core_netstats(LONG type, APTR dest, LONG size);  /* netstats.c; NULL dest = size */
void   core_memstats(struct PnMemStatus *m);            /* netstats.c */
ULONG  core_stack_mark(void);   /* netstats.c: fill the free stack, return its size */
/* AllocVec/FreeVec that keep count, for the memory statistics */
APTR   pn_alloc(ULONG size, ULONG flags);
void   pn_free(APTR p);
extern ULONG pn_mem_used, pn_mem_peak;
void   pn_assert_fail(const char *msg, const char *file, int line);
void   sock_init(void);
void   sock_free_all(void);
void   sock_check_events(void);
void   sock_forget_owner(struct PnClient *c);
int    sock_list(struct PnSockInfo *arr, int max);

/* core.c log (also printed when not QUIET) */
void   core_log(const char *fmt, ...);

/* resolv.c */
BOOL   resolv_op(struct PnClient *c, struct PnReq *r);
void   resolv_abort(struct PnReq *r);

/* sockaddr helpers (AmiTCP: sin_len, sin_family bytes; 4.3BSD: UWORD family) */
BOOL   sa_get(const UBYTE *sa, LONG len, ip_addr_t *ip, u16_t *port, BOOL *oldstyle);
void   sa_put(UBYTE *sa, LONG *lenp, const ip_addr_t *ip, u16_t port, BOOL oldstyle);
LONG   err2errno(err_t e);

#endif
