/*
 * PrismNet - message protocol between bsdsocket.library and the core task.
 * GPL-3.0-only, see LICENSE.
 *
 * The library runs on the caller's task. Every socket call fills a PnReq
 * (on the caller's stack), PutMsg()s it to the core's public port and waits
 * for the reply. The core owns all lwIP state; it may hold a request
 * "pending" (blocking recv/accept/connect/select/resolver) and reply later.
 * Buffers are passed by pointer (one address space); the core copies
 * directly into/out of them before replying.
 *
 * Interrupting a pending call (break signal, WaitSelect user signal): the
 * library sends PNOP_ABORT naming the pending request; the core replies to
 * the pending request (EINTR, or its real result if it just completed) and
 * then to the abort. The caller always collects both replies.
 */
#ifndef PRISMNET_PNMSG_H
#define PRISMNET_PNMSG_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/nodes.h>

#define PN_PORTNAME        "PrismNet.core"
/* "PrismNet STOP HOLD" (before a WHDLoad game) leaves this file: no
 * auto-start while it is there, until "PrismNet START" (ENV: is in RAM, so
 * a reboot clears it too) */
#define PN_HELD_FILE       "ENV:PrismNet/held"
#define PN_PROTO_VERSION   1

enum {
    PNOP_HELLO = 1,      /* a0=task                 -> result=client handle */
    PNOP_BYE,            /* close all of the client's sockets, free client */
    PNOP_ABORT,          /* target=pending request */
    PNOP_SOCKET,         /* a0=domain a1=type a2=protocol -> fd */
    PNOP_CLOSE,          /* fd */
    PNOP_CONNECT,        /* fd a0=sockaddr a1=len */
    PNOP_BIND,           /* fd a0=sockaddr a1=len */
    PNOP_LISTEN,         /* fd a0=backlog */
    PNOP_ACCEPT,         /* fd a0=sockaddr* a1=LONG *len -> new fd */
    PNOP_SEND,           /* fd a0=buf a1=len a2=flags a3=to a4=tolen -> bytes */
    PNOP_RECV,           /* fd a0=buf a1=len a2=flags a3=from a4=LONG *fromlen -> bytes */
    PNOP_SHUTDOWN,       /* fd a0=how */
    PNOP_SETSOCKOPT,     /* fd a0=level a1=name a2=val a3=len */
    PNOP_GETSOCKOPT,     /* fd a0=level a1=name a2=val a3=LONG *len */
    PNOP_GETSOCKNAME,    /* fd a0=sockaddr* a1=LONG *len */
    PNOP_GETPEERNAME,    /* fd a0=sockaddr* a1=LONG *len */
    PNOP_IOCTL,          /* fd a0=request a1=argp */
    PNOP_SELECT,         /* a0=nfds a1=rd a2=wr a3=ex a4=timeval* (NULL=forever) -> n */
    PNOP_GETHOSTBYNAME,  /* a0=name a1=struct PnHostBuf* */
    PNOP_GETHOSTBYADDR,  /* a0=addr(ULONG, network order) a1=struct PnHostBuf* */
    PNOP_GETHOSTNAME,    /* a0=buf a1=len */
    PNOP_DUP2,           /* fd a0=newfd (-1 = lowest free) -> fd */
    PNOP_DTABLESIZE,     /* a0=new size (0 = query) -> size */
    PNOP_OBTAIN,         /* a0=id a1=domain a2=type a3=protocol -> fd */
    PNOP_RELEASE,        /* fd a0=id a1=copy(0/1) -> id */
    PNOP_STATUS,         /* a0=struct PnStatus* a1=its size (0 = PN_STATUS_V1_SIZE) */
    PNOP_GETEVENTS,      /* a0=ULONG *mask -> fd with events, or -1 */
    PNOP_SETEVENTSIG,    /* a0=signal mask the core sends on socket events */
    PNOP_GETLOG,         /* a0=last seq seen a1=char *buf a2=size -> newest seq (tools) */
    PNOP_SOCKLIST,       /* a0=struct PnSockInfo *arr a1=max -> count (tools) */
    PNOP_NETSTATS,       /* a0=NETSTATUS_ type a1=dest (NULL = size) a2=size -> bytes (0.5) */
    PNOP_SETCLOCK,       /* ask the time server again now -> 0, or EINVAL when time is off (0.6) */
    PNOP_NETCFG,         /* a0=PNNC_* a1.. per sub-op -> 0 (or a count), -1 + error (0.5) */
    PNOP_MAX
};

/* PNOP_NETCFG: change the running configuration (Roadshow's routing,
 * interface and DNS calls) and list routes and the ARP table. Addresses are
 * in network order. Changes last until PrismNet stops; ENV:PrismNet is not
 * touched. */
enum {
    PNNC_ROUTE_ADD = 1,  /* a1=net a2=mask a3=gateway; net = mask = 0: the default gateway */
    PNNC_ROUTE_DEL,      /* a1=net a2=mask a3=gateway (0 = any) */
    PNNC_DNS_ADD,        /* a1=address */
    PNNC_DNS_DEL,        /* a1=address */
    PNNC_DOMAIN,         /* a1=char *name ("" = none) */
    PNNC_IF_SET,         /* a1=char *interface a2=PNIF_* a3=value */
    PNNC_IF_ADD,         /* a1=char *interface a2=char *device a3=unit */
    PNNC_IF_REMOVE,      /* a1=char *interface (only the last one added) */
    PNNC_ROUTES,         /* a1=struct PnRouteInfo * a2=max entries -> count */
    PNNC_ARP             /* a1=struct PnArpInfo * a2=max entries -> count */
};
enum { PNIF_ADDRESS = 1, PNIF_NETMASK, PNIF_PEER, PNIF_STATE, PNIF_MTU };
/* PNIF_STATE values: Roadshow's SM_Offline, SM_Online, SM_Down, SM_Up */
#define PNRT_DEFAULT    0x01    /* the default gateway */
#define PNRT_CONNECTED  0x02    /* an interface's own subnet (no gateway) */
#define PNRT_STATIC     0x04    /* a static route (ENV:PrismNet/routes or added) */
#define PNRT_HOST       0x08    /* a single host (/32) */
struct PnRouteInfo {
    ULONG net, mask, gw;
    char  ifname[16];
    UBYTE bits, flags;
    UWORD pad;
};
struct PnArpInfo {
    ULONG ip;
    UBYTE mac[6];
    UWORD stable;               /* 1 = answered, 0 = being asked */
    char  ifname[16];
};

struct PnReq {
    struct Message msg;
    UWORD          op;
    UWORD          flags;       /* PNREQ_* (0.5; older libraries send 0) */
    APTR           client;      /* core-side client handle */
    LONG           fd;
    ULONG          a[6];
    LONG           result;
    LONG           error;       /* BSD/AmiTCP errno value, 0 = none */
    LONG           herror;      /* resolver h_errno */
    struct PnReq  *target;      /* PNOP_ABORT */
    /* core private */
    struct MinNode node;        /* pending-list link */
    ULONG          progress;    /* bytes done so far for partial ops */
    ULONG          deadline;    /* sys_now() value, select timeout */
    BOOL           has_deadline;
};

/*
 * Direct calls (0.5). A request is a message to the core task, two task
 * switches per call. The socket calls can instead run the core's handler
 * in the caller's own task, guarded by this semaphore: the core holds it
 * for each pass of its loop and lets go only while it sleeps. It is a
 * public named semaphore that is never freed, so a library that finds the
 * core gone (run == NULL) has touched no freed memory. A request that has
 * to wait (no data yet) stays on the core's pending list and is replied to
 * by message as before.
 */
#include <exec/semaphores.h>
#define PN_DIRECT_NAME   "PrismNet.direct"
#define PNREQ_DIRECT     0x0001     /* running in the caller's task now */
#define PNREQ_REPLIED    0x0002     /* ... and core_reply() has answered it */
#define PNREQ_RETRIED    0x0100     /* core private: the resolver asked twice */
struct PnDirect {
    struct SignalSemaphore ss;
    /* run one request; TRUE = answered (result/error set), FALSE = pending */
    BOOL (*run)(struct PnReq *r);
};
/* the operations that may run directly; the others always go by message */
#define PN_DIRECT_OP(op) (((op) >= PNOP_SOCKET && (op) <= PNOP_SELECT) || \
                          (op) == PNOP_GETHOSTNAME || \
                          ((op) >= PNOP_DUP2 && (op) <= PNOP_RELEASE) || \
                          (op) == PNOP_GETEVENTS || (op) == PNOP_SETEVENTSIG)

#define PN_MAXHOSTADDRS 8
struct PnHostBuf {
    /* layout-compatible with struct hostent at the start */
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
    char   *alias_ptrs[1];
    char   *addr_ptrs[PN_MAXHOSTADDRS + 1];
    ULONG   addrs[PN_MAXHOSTADDRS];
    char    name[256];
};

/* dos.library version the libraries need: 2.04 */
#define PN_DOS_MIN 37

#ifndef PN_MAX_IFACES
#define PN_MAX_IFACES 4
#endif

struct PnIfStatus {
    char    name[16];
    char    device[64];
    ULONG   unit;
    UBYTE   mac[6];
    UWORD   mtu;
    ULONG   ip, netmask, gateway;           /* network order */
    BOOL    dhcp, link_up, is_default;
    ULONG   rx_packets, rx_bytes, tx_packets, tx_bytes, rx_dropped, tx_dropped;
};

/* More per-interface data (0.5). Only sent to callers whose PNOP_STATUS a1
 * says their buffer is big enough, so tools built before it stay safe. */
struct PnIfStatusEx {
    ULONG   hwtype;                         /* SANA-II HardwareType */
    ULONG   bps;
    UWORD   addrbits;                       /* station address size in bits */
    UWORD   p2p;                            /* point-to-point link (SLIP/PPP/PLIP) */
    ULONG   rx_errors, tx_errors;
    UWORD   rx_reqs, tx_reqs;               /* read/write requests allocated */
    UWORD   rx_busy, tx_busy;               /* ... of which with the driver now */
    ULONG   up_secs;                        /* seconds since 1.1.1978 it came up */
};

/* Memory the running core uses (0.5). "used/peak/max" triples count items. */
struct PnMemStatus {
    ULONG   image;                          /* code + data + BSS of the core (fixed) */
    ULONG   dyn_used, dyn_peak;             /* bytes allocated since: clients, sockets, queues */
    ULONG   budget, mem_used, mem_peak, mem_fails;  /* lwIP's memory: allowed, live, most, refused */
    ULONG   pool_max, pool_used, pool_peak, pool_fails;     /* packet buffers */
    UWORD   tcp_max, tcp_used, tcp_peak;    /* TCP connections */
    UWORD   seg_max, seg_used, seg_peak;    /* queued TCP segments */
    UWORD   udp_max, udp_used, udp_peak;    /* UDP sockets */
    UWORD   pad;
    ULONG   mem_cached;                     /* freed lwIP blocks kept for reuse */
    ULONG   sys_free, sys_chip, sys_largest;    /* the machine's free memory */
    ULONG   stack;                          /* the core task's stack */
    ULONG   stack_peak;                     /* ... most of it ever used */
};

struct PnStatus {
    ULONG   ip;                             /* primary address (default interface) */
    ULONG   dns[3];
    char    hostname[64];
    char    domain[64];
    ULONG   clients, sockets;
    ULONG   nifs;
    struct PnIfStatus ifs[PN_MAX_IFACES];
    struct PnIfStatusEx ifx[PN_MAX_IFACES]; /* 0.5 */
    struct PnMemStatus mem;                 /* 0.5 */
    /* 0.6: the network clock. clock_state: 0 off (no ENV:PrismNet/time),
     * 1 waiting for an answer, 2 set */
    ULONG   clock_state;
    ULONG   clock_set_at;                   /* Amiga seconds (local) when it was last set */
    LONG    clock_delta;                    /* seconds the clock was off by, then */
    ULONG   clock_syncs;                    /* answers applied so far */
    char    clock_server[48];
};
#define PN_STATUS_V1_SIZE  ((ULONG)&((struct PnStatus *)0)->ifx)

/* PNOP_SOCKLIST entries (netstat) */
enum { PNSS_NONE, PNSS_LISTEN, PNSS_CONNECTING, PNSS_ESTABLISHED, PNSS_CLOSING, PNSS_BOUND };
struct PnSockInfo {
    char    owner[24];          /* task name of the owning program */
    UBYTE   type;               /* 1 stream, 2 dgram, 3 raw */
    UBYTE   state;              /* PNSS_* */
    UWORD   lport, rport;
    ULONG   lip, rip;           /* network order */
    ULONG   rxq;                /* bytes waiting to be read */
    ULONG   txq;                /* bytes not yet acknowledged (TCP) */
};

/* BSD (AmiTCP / Roadshow) errno values used on the wire. */
#define PN_EPERM            1
#define PN_ENOENT           2
#define PN_ESRCH            3
#define PN_EBUSY            16
#define PN_EEXIST           17
#define PN_ENODEV           19
#define PN_EINTR            4
#define PN_EBADF            9
#define PN_ENOMEM           12
#define PN_EFAULT           14
#define PN_EINVAL           22
#define PN_EMFILE           24
#define PN_EPIPE            32
#define PN_EWOULDBLOCK      35
#define PN_EINPROGRESS      36
#define PN_EALREADY         37
#define PN_ENOTSOCK         38
#define PN_EDESTADDRREQ     39
#define PN_EMSGSIZE         40
#define PN_EPROTOTYPE       41
#define PN_ENOPROTOOPT      42
#define PN_EPROTONOSUPPORT  43
#define PN_ESOCKTNOSUPPORT  44
#define PN_EOPNOTSUPP       45
#define PN_EAFNOSUPPORT     47
#define PN_EADDRINUSE       48
#define PN_EADDRNOTAVAIL    49
#define PN_ENETDOWN         50
#define PN_ENETUNREACH      51
#define PN_ECONNABORTED     53
#define PN_ECONNRESET       54
#define PN_ENOBUFS          55
#define PN_EISCONN          56
#define PN_ENOTCONN         57
#define PN_ESHUTDOWN        58
#define PN_ETIMEDOUT        60
#define PN_ECONNREFUSED     61
#define PN_EHOSTUNREACH     65

/* h_errno */
#define PN_HOST_NOT_FOUND   1
#define PN_TRY_AGAIN        2
#define PN_NO_RECOVERY      3
#define PN_NO_DATA          4

#endif /* PRISMNET_PNMSG_H */
