/*
 * PrismNet core task: owns lwIP and serves bsdsocket.library requests.
 * GPL-3.0-only, see LICENSE.
 *
 *   PrismNet [START]        start in the background from ENV:PrismNet (config.h)
 *   PrismNet STATUS | STOP
 *   PrismNet RUN [QUIET]    run in the foreground (what START launches)
 *   PrismNet DEVICE=cnet.device [UNIT=0] [IP=a.b.c.d NETMASK=.. GATEWAY=..]
 *            [DNS=..] [DNS2=..] [HOSTNAME=name] [QUIET]   one interface, foreground
 *   TRACE=file logs every request/reply.
 */
#include <stdio.h>
#include <setjmp.h>
#include <stdarg.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "lwip/init.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/apps/netbiosns.h"
#include "lwip/apps/mdns.h"
#include "lwip/apps/sntp.h"
#include <libraries/locale.h>
#include <proto/locale.h>

#include "prismnet/pnsys.h"
#include "prismnet/sanaif.h"
#include "prismnet/pnclient.h"
#include "prismnet/pnmem.h"
#include "core.h"
#include "config.h"

static const char version[] = "$VER: PrismNet 1.0.7 (23.10.2026) core";

/* libnix startup swaps to a stack this big: lwIP callbacks nest deeply and
 * "Run" only gives the shell's default (often 4 KB). The gate's stress runs
 * peak at about 4.2 KB (STATUS reports the high-water mark), so 16 KB has
 * nearly 4x headroom; the 64 KB of before cost every user 48 KB of RAM.
 * Needs swapstack.o: see the Makefile. */
unsigned long __stack = 16384;

#define CORE_PRI 3   /* above normal apps so the stack isn't starved */

#define TEMPLATE "START/S,RUN/S,STOP/S,STATUS/S,LOG/S,NETSTAT/S,DEVICE/K,UNIT/K/N,IP/K,NETMASK/K," \
                 "GATEWAY/K,DNS/K,DNS2/K,HOSTNAME/K,TRACE/K,QUIET/S,SETCLOCK/S,ROUTE/S,ARP/S,HOLD/S"
enum { A_START, A_RUN, A_STOP, A_STATUS, A_LOG, A_NETSTAT, A_DEVICE, A_UNIT, A_IP, A_NETMASK,
       A_GATEWAY, A_DNS, A_DNS2, A_HOSTNAME, A_TRACE, A_QUIET, A_SETCLOCK, A_ROUTE, A_ARP, A_HOLD,
       A_COUNT };

struct PnCore core;

struct PnIface {
    struct sanaif     s;
    struct PnIfConfig cfg;
    ULONG             up_secs;          /* DateStamp seconds when it was opened */
};
static struct PnIface *ifs;        /* one per configured interface, allocated at start */
static const char *p2p_kind(ULONG hwtype);
static int nifs;                    /* interfaces actually open */
static struct PnConfig config;
static struct MsgPort *core_port;
static BOOL quiet, port_added;
static FILE *trace;           /* TRACE=file: one line per request reply */

/* time from the network (SNTP), see start_time_service() */
static struct {
    BOOL  running;                      /* sntp_init() done */
    ULONG state;                        /* as PnStatus.clock_state */
    ULONG set_at;
    ULONG syncs;                        /* answers applied (SETCLOCK waits for the next) */
    LONG  delta;
    char  server[48];
} ntp;

static void start_time_service(void);
static void stop_time_service(void);
static struct Library *self_base;   /* keeps bsdsocket.library resident */
static jmp_buf assert_jmp;          /* an lwIP assertion jumps to the shutdown path */
static BOOL    assert_armed;

/* ---- log: last LOG_LINES messages, read by PrismNet LOG and PrismMon ---- */
#define LOG_LINES 64
static struct { ULONG seq; char text[100]; } logbuf[LOG_LINES];
static ULONG log_seq;

void core_log(const char *fmt, ...)
{
    struct DateStamp ds;
    char *t;
    va_list ap;
    LONG secs;

    DateStamp(&ds);
    secs = ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
    log_seq++;
    t = logbuf[log_seq % LOG_LINES].text;
    logbuf[log_seq % LOG_LINES].seq = log_seq;
    snprintf(t, 12, "%02ld:%02ld:%02ld ", (long)(secs / 3600 % 24), (long)(secs / 60 % 60),
             (long)(secs % 60));
    va_start(ap, fmt);
    vsnprintf(t + 9, sizeof(logbuf[0].text) - 9, fmt, ap);
    va_end(ap);
    if (!quiet)
        printf("PrismNet: %s\n", t + 9);
    if (trace) {                        /* TRACE=file: the log too, line by line, so a */
        fprintf(trace, "log %s\n", t);  /* hang during start-up still leaves its last line */
        fflush(trace);
    }
}

/* Lines newer than `after` as "seq<TAB>text<LF>"; returns the last seq copied. */
static ULONG copy_log(ULONG after, char *buf, LONG size)
{
    ULONG seq = log_seq > LOG_LINES ? log_seq - LOG_LINES + 1 : 1;
    LONG used = 0;

    if (size <= 0)
        return log_seq;
    buf[0] = 0;
    if (after >= seq)
        seq = after + 1;
    for (; seq <= log_seq; seq++) {
        int n = snprintf(buf + used, size - used, "%lu\t%s\n", (unsigned long)seq,
                         logbuf[seq % LOG_LINES].text);
        if (n < 0 || used + n >= size) {
            buf[used] = 0;
            return seq - 1;             /* caller asks again for the rest */
        }
        used += n;
    }
    return log_seq;
}

#ifdef PN_PROFILE
/* Profiling build (make PROFILE=1): E-clock ticks per main-loop section,
 * logged every 16384 passes (PrismNet LOG). */
#include <devices/timer.h>
#include <proto/timer.h>
static ULONG prof_t[8], prof_last, prof_passes, prof_wall;
static void prof_mark(int i)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    if (i)
        prof_t[i] += ev.ev_lo - prof_last;
    prof_last = ev.ev_lo;
}
extern ULONG sprof_in, sprof_inn, sprof_rq, sprof_rqn, sprof_tx, sprof_txn, sprof_ck, sprof_ckn, sprof_ckb;
static void prof_report(void)
{
    if (++prof_passes & 2047)
        return;
    core_log("sprof input %lu/%lu readq %lu/%lu tx %lu/%lu", (unsigned long)sprof_in,
             (unsigned long)sprof_inn, (unsigned long)sprof_rq, (unsigned long)sprof_rqn,
             (unsigned long)sprof_tx, (unsigned long)sprof_txn);
    core_log("sprof chksum %lu/%lu calls, %lu bytes", (unsigned long)sprof_ck,
             (unsigned long)sprof_ckn, (unsigned long)sprof_ckb);
    core_log("prof %lu: wall %lu msg %lu poll %lu tmo %lu pend %lu ev %lu arm %lu",
             (unsigned long)prof_passes, (unsigned long)(prof_last - prof_wall),
             (unsigned long)prof_t[1], (unsigned long)prof_t[2],
             (unsigned long)prof_t[3], (unsigned long)prof_t[4], (unsigned long)prof_t[5],
             (unsigned long)prof_t[6]);
    memset(prof_t, 0, sizeof(prof_t));
    prof_wall = prof_last;
}
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/priv/tcp_priv.h"
static struct PnReq *node2req(struct MinNode *n);
static void prof_diag(void)
{
    struct tcp_pcb *pcb;
    int i, k, q;

    for (i = 0; i < nifs; i++) {
        for (q = 0, k = 0; k < SANAIF_NUM_RX; k++)
            q += ifs[i].s.rx[k].busy;
        core_log("diag if%d: reads queued %d starved %lu resets %lu, rx %lu drop %lu err %lu, tx %lu drop %lu",
                 i, q, (unsigned long)ifs[i].s.stats.rx_starved, (unsigned long)ifs[i].s.stats.resets, (unsigned long)ifs[i].s.stats.rx_packets,
                 (unsigned long)ifs[i].s.stats.rx_dropped, (unsigned long)ifs[i].s.stats.rx_errors,
                 (unsigned long)ifs[i].s.stats.tx_packets, (unsigned long)ifs[i].s.stats.tx_dropped);
    }
    core_log("diag pool used %u max %u err %u, heap used %lu err %u, timer armed %d now %lu",
             (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->used, (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->max,
             (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err, (unsigned long)(pn_mem.held - pn_mem.cached),
             (unsigned)pn_mem.fails, (int)pn_timer_armed(), (unsigned long)sys_now());
    for (pcb = tcp_active_pcbs; pcb; pcb = pcb->next)
        core_log("diag pcb %u st %d rcv_nxt %lu rcv_wnd %u ann %u snd_wnd %u ooseq %d refused %d flags %x rtime %d",
                 (unsigned)pcb->local_port, (int)pcb->state, (unsigned long)pcb->rcv_nxt,
                 (unsigned)pcb->rcv_wnd, (unsigned)pcb->rcv_ann_wnd, (unsigned)pcb->snd_wnd,
                 pcb->ooseq != NULL, pcb->refused_data != NULL, (unsigned)pcb->flags, (int)pcb->rtime);
    {
        struct MinNode *n; int np = 0;
        for (n = core.pending.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
            struct PnReq *r = node2req(n);
            np++;
            core_log("diag pending op %u fd %ld progress %lu dl %d", (unsigned)r->op, (long)r->fd,
                     (unsigned long)r->progress, (int)r->has_deadline);
        }
        for (n = core.sockets.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
            struct PnSock *so = (struct PnSock *)n;
            core_log("diag sock flags %x rxq %u", (unsigned)so->flags, so->rxq ? (unsigned)so->rxq->tot_len : 0);
        }
    }
}
#define PROF(i)       prof_mark(i)
#define PROF_REPORT() prof_report()
#define PROF_DIAG()   prof_diag()
#else
#define PROF_DIAG()   ((void)0)
#define PROF(i)       ((void)0)
#define PROF_REPORT() ((void)0)
#endif

#ifdef PN_DIAGPORT
/* Test builds: loop state in a public port "PrismNet.diag" that a probe reads
 * without messaging (and so without waking) the core. */
struct PnDiag {
    struct MsgPort mp;
    ULONG magic, passes, stage, waitmask, sigs, timed, timer_busy, timer_due, now,
          msgs, pending, txbusy, rxbusy, lastop, polls;
};
static struct PnDiag pdiag;
#define DIAG(f, v) (pdiag.f = (ULONG)(v))
#else
#define DIAG(f, v) ((void)0)
#endif

/* ---------------------------------------------------------------------- */
/* replies, pending list, clients, fd table                                */
/* ---------------------------------------------------------------------- */

void core_reply(struct PnReq *r, LONG result, LONG error)
{
    if (trace) {
        fprintf(trace, "%lu c=%lx op=%u fd=%ld a0=%lx a1=%lu a2=%lx -> %ld err %ld\n",
                (unsigned long)sys_now(), (unsigned long)r->client, (unsigned)r->op, (long)r->fd,
                (unsigned long)r->a[0], (unsigned long)r->a[1], (unsigned long)r->a[2],
                (long)result, (long)error);
        fflush(trace);
    }
    r->result = result;
    r->error  = error;
    if (r->flags & PNREQ_DIRECT)
        r->flags |= PNREQ_REPLIED;      /* the caller is waiting in core_direct() */
    else
        ReplyMsg(&r->msg);
}

void core_pend(struct PnReq *r)
{
    AddTail((struct List *)&core.pending, (struct Node *)&r->node);
}

/* A request with a deadline: the main loop must re-arm its wakeup timer. */
static BOOL deadline_added;

void core_set_deadline(struct PnReq *r, ULONG ms)
{
    r->deadline     = sys_now() + ms;
    r->has_deadline = TRUE;
    deadline_added  = TRUE;
}

static struct PnReq *node2req(struct MinNode *n)
{
    return (struct PnReq *)((UBYTE *)n - offsetof(struct PnReq, node));
}

static struct PnClient *find_client(APTR handle)
{
    struct MinNode *n;
    for (n = core.clients.mlh_Head; n->mln_Succ; n = n->mln_Succ)
        if ((APTR)n == handle)
            return (struct PnClient *)n;
    return NULL;
}

struct PnSock *core_fd2sock(struct PnClient *c, LONG fd, LONG *err)
{
    if (fd < 0 || fd >= c->nfds || !c->fds[fd]) {
        *err = PN_EBADF;
        return NULL;
    }
    return c->fds[fd];
}

LONG core_alloc_fd(struct PnClient *c, struct PnSock *s, LONG min)
{
    LONG i;
    for (i = min; i < c->nfds; i++) {
        if (!c->fds[i]) {
            c->fds[i] = s;
            s->refs++;
            return i;
        }
    }
    return -1;
}

static LONG set_dtablesize(struct PnClient *c, LONG n)
{
    struct PnSock **nf;
    LONG i;

    if (n <= c->nfds)
        return c->nfds;              /* never shrink below open fds */
    if (n > PN_MAX_DTABLESIZE)
        n = PN_MAX_DTABLESIZE;
    nf = pn_alloc(n * sizeof(*nf), MEMF_CLEAR);
    if (!nf)
        return c->nfds;
    for (i = 0; i < c->nfds; i++)
        nf[i] = c->fds[i];
    pn_free(c->fds);
    c->fds  = nf;
    c->nfds = n;
    return n;
}

static void abort_client_pending(struct PnClient *c)
{
    struct MinNode *n, *next;
    for (n = core.pending.mlh_Head; (next = n->mln_Succ) != NULL; n = next) {
        struct PnReq *r = node2req(n);
        if (r->client == (APTR)c) {
            Remove((struct Node *)n);
            sock_abort_pending(r);
            resolv_abort(r);
            core_reply(r, -1, PN_EINTR);
        }
    }
}

/* LWIP_PLATFORM_ASSERT: an lwIP invariant broke. abort() would leave the
 * public port findable and SANA-II reads pointed at freed pbufs, so log it
 * and run the normal shutdown instead. A second assertion while shutting
 * down cannot be helped. */
static BOOL direct_assert;      /* an assertion fired inside a direct call */

void pn_assert_fail(const char *msg, const char *file, int line)
{
    core_log("lwIP assertion failed: %s (%s:%d)", msg, file, line);
    if (core_port && FindTask(NULL) != core_port->mp_SigTask) {
        /* in a client's task (direct call): cannot jump to the core's
         * shutdown path from here; the core shuts down on its next pass */
        direct_assert = TRUE;
        Signal(core_port->mp_SigTask, SIGBREAKF_CTRL_C);
        return;
    }
    if (assert_armed) {
        assert_armed = FALSE;
        longjmp(assert_jmp, 1);
    }
    printf("PrismNet: lwIP assertion failed: %s (%s:%d)\n", msg, file, line);
    abort();
}

/* Take the public port down and fail every request still queued on it.
 * Every exit path must go through here: a client that found the port and
 * sent HELLO waits for the reply and cannot be interrupted (its ABORT has
 * nowhere to go), so the port must never vanish with a request on it. */
static void close_public_port(void)
{
    struct PnReq *r;

    Forbid();
    if (port_added) {
        RemPort(core_port);
#ifdef PN_DIAGPORT
        RemPort(&pdiag.mp);
#endif
        port_added = FALSE;
    }
    if (self_base) {                    /* clears the library's cached port */
        CloseLibrary(self_base);
        self_base = NULL;
    }
    Permit();
    while ((r = (struct PnReq *)GetMsg(core_port)) != NULL)
        core_reply(r, r->op == PNOP_ABORT ? 0 : -1, PN_ENETDOWN);
}

/* An fd is closed or dup2'd over while a request on it is still pending
 * (another task sharing the SocketBase): fail that request now, or it would
 * carry on against whatever socket gets the number next. */
void core_fail_pending_fd(struct PnClient *c, LONG fd)
{
    struct MinNode *n, *next;
    for (n = core.pending.mlh_Head; (next = n->mln_Succ) != NULL; n = next) {
        struct PnReq *r = node2req(n);
        if (r->client != (APTR)c || r->fd != fd)
            continue;
        if (r->op != PNOP_CONNECT && r->op != PNOP_ACCEPT && r->op != PNOP_SEND &&
            r->op != PNOP_RECV)
            continue;
        Remove((struct Node *)n);
        sock_abort_pending(r);
        if (r->progress > 0 && (r->op == PNOP_RECV || r->op == PNOP_SEND))
            core_reply(r, (LONG)r->progress, 0);
        else
            core_reply(r, -1, PN_EBADF);
    }
}

static void free_client(struct PnClient *c)
{
    LONG i;
    abort_client_pending(c);
    for (i = 0; i < c->nfds; i++) {
        if (c->fds[i]) {
            sock_unref(c->fds[i]);
            c->fds[i] = NULL;
        }
    }
    sock_forget_owner(c);
    Remove((struct Node *)&c->node);
    pn_free(c->fds);
    pn_free(c);
}

/* ---------------------------------------------------------------------- */
/* request dispatch                                                         */
/* ---------------------------------------------------------------------- */

ULONG pn_mem_used, pn_mem_peak;

/* A start-up error: printed, and left in ENV:PrismNet/error for "PrismNet
 * START", which runs the core with its output going to NIL:. Memory errors
 * add what the machine has free, the usual cause on small machines. */
#define PN_ERROR_FILE "ENV:PrismNet/error"
static void startup_fail(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    BPTR f;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (strstr(msg, "memory") && n > 0 && n < (int)sizeof(msg))
        snprintf(msg + n, sizeof(msg) - n, " - %lu KB free, largest block %lu KB",
                 (unsigned long)(AvailMem(MEMF_ANY) / 1024),
                 (unsigned long)(AvailMem(MEMF_ANY | MEMF_LARGEST) / 1024));
    printf("PrismNet: %s\n", msg);
    if ((f = Open((CONST_STRPTR)PN_ERROR_FILE, MODE_NEWFILE)) != 0) {
        Write(f, msg, strlen(msg));
        Write(f, (APTR)"\n", 1);
        Close(f);
    }
}

APTR pn_alloc(ULONG size, ULONG flags)
{
    ULONG *p = AllocVec(size + 8, flags);       /* 8 keeps the result aligned */
    if (!p)
        return NULL;
    p[0] = size + 8;
    if ((pn_mem_used += size + 8) > pn_mem_peak)
        pn_mem_peak = pn_mem_used;
    return p + 2;
}

void pn_free(APTR q)
{
    ULONG *p;
    if (!q)
        return;
    p = (ULONG *)q - 2;
    pn_mem_used -= p[0];
    FreeVec(p);
}

static void fill_status(struct PnStatus *st)
{
    struct MinNode *n;
    int i;

    memset(st, 0, sizeof(*st));
    for (i = 0; i < 3; i++)
        st->dns[i] = ip4_addr_get_u32(ip_2_ip4(dns_getserver((u8_t)i)));
    strncpy(st->hostname, core.hostname, sizeof(st->hostname) - 1);
    strncpy(st->domain, core.domain, sizeof(st->domain) - 1);
    for (n = core.clients.mlh_Head; n->mln_Succ; n = n->mln_Succ) st->clients++;
    for (n = core.sockets.mlh_Head; n->mln_Succ; n = n->mln_Succ) st->sockets++;
    if (netif_default)
        st->ip = ip4_addr_get_u32(netif_ip4_addr(netif_default));
    st->clock_state  = ntp.state;
    st->clock_set_at = ntp.set_at;
    st->clock_syncs  = ntp.syncs;
    st->clock_delta  = ntp.delta;
    strncpy(st->clock_server, ntp.server, sizeof(st->clock_server) - 1);

    for (i = 0; i < nifs; i++) {
        struct PnIfStatus *is = &st->ifs[st->nifs++];
        struct sanaif *s = &ifs[i].s;

        strncpy(is->name, ifs[i].cfg.name, sizeof(is->name) - 1);
        strncpy(is->device, s->devname, sizeof(is->device) - 1);
        is->unit    = s->unit;
        memcpy(is->mac, s->mac, 6);
        is->mtu     = s->netif.mtu;
        is->ip      = ip4_addr_get_u32(netif_ip4_addr(&s->netif));
        is->netmask = ip4_addr_get_u32(netif_ip4_netmask(&s->netif));
        is->gateway = ip4_addr_get_u32(netif_ip4_gw(&s->netif));
        is->dhcp    = ifs[i].cfg.dhcp;
        is->link_up = netif_is_link_up(&s->netif);
        is->is_default = netif_default == &s->netif;
        is->rx_packets = s->stats.rx_packets;  is->rx_bytes = s->stats.rx_bytes;
        is->tx_packets = s->stats.tx_packets;  is->tx_bytes = s->stats.tx_bytes;
        is->rx_dropped = s->stats.rx_dropped;  is->tx_dropped = s->stats.tx_dropped;
        {
            struct PnIfStatusEx *x = &st->ifx[i];
            int k;
            x->hwtype   = s->hwtype;
            x->bps      = s->bps;
            x->addrbits = s->addrbits;
            x->p2p      = s->p2p;
            x->rx_errors = s->stats.rx_errors;
            x->tx_errors = s->stats.tx_errors;
            x->rx_reqs  = SANAIF_NUM_RX;
            x->tx_reqs  = SANAIF_NUM_TX;
            for (k = 0; k < SANAIF_NUM_RX; k++) if (s->rx[k].busy) x->rx_busy++;
            for (k = 0; k < SANAIF_NUM_TX; k++) if (s->tx[k].busy) x->tx_busy++;
            x->up_secs  = ifs[i].up_secs;
        }
    }
    core_memstats(&st->mem);
}
/* Run (or re-run) a request. Returns TRUE if it was replied. */
static BOOL run_request(struct PnClient *c, struct PnReq *r)
{
    switch (r->op) {
    case PNOP_GETHOSTBYNAME:
    case PNOP_GETHOSTBYADDR:
        return resolv_op(c, r);
    case PNOP_GETHOSTNAME: {
        LONG len = (LONG)r->a[1];
        if (len <= 0) { core_reply(r, -1, PN_EINVAL); return TRUE; }
        strncpy((char *)r->a[0], core.hostname, len - 1);
        ((char *)r->a[0])[len - 1] = 0;
        core_reply(r, 0, 0);
        return TRUE;
    }
    case PNOP_SETEVENTSIG:
        c->sigeventmask = r->a[0];
        core_reply(r, 0, 0);
        return TRUE;
    case PNOP_GETEVENTS: {
        LONG fd;
        for (fd = 0; fd < c->nfds; fd++) {
            struct PnSock *s = c->fds[fd];
            if (s && s->events) {
                *(ULONG *)r->a[0] = s->events;
                s->events = 0;
                core_reply(r, fd, 0);
                return TRUE;
            }
        }
        core_reply(r, -1, 0);
        return TRUE;
    }
    case PNOP_DTABLESIZE:
        core_reply(r, r->a[0] ? set_dtablesize(c, (LONG)r->a[0]) : c->nfds, 0);
        return TRUE;
    case PNOP_SELECT:
        return sock_select(c, r, FALSE);
    default:
        return sock_op(c, r);
    }
}

static LONG netcfg(struct PnReq *r, LONG *err);

static void handle_message(struct PnReq *r)
{
    struct PnClient *c;

    DIAG(lastop, r->op);
#ifdef PN_DIAGPORT
    pdiag.msgs++;
#endif
    r->progress = 0;
    r->has_deadline = FALSE;

    switch (r->op) {
    case PNOP_HELLO:
        c = pn_alloc(sizeof(*c), MEMF_CLEAR);
        if (c && (c->fds = pn_alloc(PN_DEFAULT_DTABLESIZE * sizeof(*c->fds), MEMF_CLEAR))) {
            c->task = (struct Task *)r->a[0];
            c->nfds = PN_DEFAULT_DTABLESIZE;
            AddTail((struct List *)&core.clients, (struct Node *)&c->node);
            core_reply(r, (LONG)c, 0);
        } else {
            pn_free(c);
            core_reply(r, 0, PN_ENOMEM);
        }
        return;
    case PNOP_STATUS: {
        static struct PnStatus stbuf;   /* the caller may be an older, smaller layout */
        ULONG size = r->a[1] ? r->a[1] : PN_STATUS_V1_SIZE;
        fill_status(&stbuf);
        CopyMem(&stbuf, (APTR)r->a[0], size < sizeof(stbuf) ? size : sizeof(stbuf));
        core_reply(r, 0, 0);
        return;
    }
    case PNOP_GETLOG:
        if (r->a[0] == 0) PROF_DIAG();
        core_reply(r, (LONG)copy_log(r->a[0], (char *)r->a[1], (LONG)r->a[2]), 0);
        return;
    case PNOP_SOCKLIST:
        core_reply(r, sock_list((struct PnSockInfo *)r->a[0], (int)r->a[1]), 0);
        return;
    case PNOP_NETSTATS: {
        LONG n = core_netstats((LONG)r->a[0], (APTR)r->a[1], (LONG)r->a[2]);
        core_reply(r, n, n < 0 ? PN_EINVAL : 0);
        return;
    }
    case PNOP_NETCFG: {
        LONG err = 0, n = netcfg(r, &err);
        core_reply(r, err ? -1 : n, err);
        return;
    }
    case PNOP_SETCLOCK: {
        if (!ntp.running) { core_reply(r, -1, PN_EINVAL); return; }
        sntp_stop();                    /* restart: a request goes out at once */
        sntp_init();
        core_reply(r, 0, 0);
        return;
    }
    case PNOP_ABORT: {
        struct MinNode *n;
        for (n = core.pending.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
            struct PnReq *t = node2req(n);
            if (t == r->target && t->client == r->client) {
                Remove((struct Node *)n);
                resolv_abort(t);
                if (t->op == PNOP_SELECT && (c = find_client(t->client)) != NULL) {
                    sock_select(c, t, TRUE);      /* reply with what is ready now */
                } else {
                    sock_abort_pending(t);
                    if (t->progress > 0 && (t->op == PNOP_RECV || t->op == PNOP_SEND))
                        core_reply(t, (LONG)t->progress, 0);
                    else
                        core_reply(t, -1, PN_EINTR);
                }
                break;
            }
        }
        core_reply(r, 0, 0);
        return;
    }
    default:
        break;
    }

    c = find_client(r->client);
    if (!c) {
        core_reply(r, -1, PN_ENETDOWN);
        return;
    }
    if (r->op == PNOP_BYE) {
        free_client(c);
        core_reply(r, 0, 0);
        return;
    }
    if (!run_request(c, r))
        core_pend(r);
}

/* Re-run every pending request after an event. */
static void service_pending(void)
{
    struct MinList todo;
    struct MinNode *n;

    /* Take the whole list first: requests that stay pending go back onto
     * core.pending, and must not be visited again in this pass. */
    pn_newlist(&todo);
    while ((n = (struct MinNode *)RemHead((struct List *)&core.pending)) != NULL)
        AddTail((struct List *)&todo, (struct Node *)n);

    while ((n = (struct MinNode *)RemHead((struct List *)&todo)) != NULL) {
        struct PnReq *r = node2req(n);
        struct PnClient *c;

        if ((r->op == PNOP_GETHOSTBYNAME || r->op == PNOP_GETHOSTBYADDR) && r->progress) {
            core_pend(r);               /* attached: completed by the DNS callback */
            continue;
        }
        c = find_client(r->client);
        if (!c)
            core_reply(r, -1, PN_ENETDOWN);
        else if (!run_request(c, r))
            core_pend(r);
    }
}

/* ---- direct calls (see struct PnDirect in pnmsg.h) ---------------------- */

static struct PnDirect *direct;

/* Run one request in the caller's task, with the direct semaphore held (so
 * the core task is asleep). Does the work of one loop pass that the request
 * may need: pending requests, socket events, loopback. The core task is
 * woken only if it has to arm its timer or another request is now pending. */
static BOOL core_direct(struct PnReq *r)
{
    struct PnClient *c = find_client(r->client);
    BOOL replied;

    r->progress = 0;
    r->has_deadline = FALSE;
    if (!c) {
        r->result = -1;
        r->error = PN_ENETDOWN;
        return TRUE;
    }
    pn_clock_update();                  /* deadlines set here need the time now */
    pn_clock_hold(TRUE);                /* no more reads in the caller's task */
    r->flags |= PNREQ_DIRECT;
    replied = run_request(c, r);
    r->flags &= ~(PNREQ_DIRECT | PNREQ_REPLIED);
    /* A query, or a call that found nothing to do, changed no state: the
     * rest of a loop pass has nothing to catch up on. */
    if (!(replied && (r->op == PNOP_GETSOCKOPT || r->op == PNOP_GETSOCKNAME ||
                      r->op == PNOP_GETPEERNAME || r->op == PNOP_GETHOSTNAME ||
                      r->op == PNOP_DTABLESIZE || r->op == PNOP_GETEVENTS ||
                      r->error == PN_EWOULDBLOCK))) {
        service_pending();
        sock_check_events();
        netif_poll_all();
    }
    pn_clock_hold(FALSE);
    if (!replied)
        core_pend(r);                   /* replied to later, by message */
    if (!replied || deadline_added)
        Signal(core_port->mp_SigTask, 1UL << core_port->mp_SigBit);
    return replied;
}

/* Find or create the public direct semaphore; NULL if there is no memory. */
static struct PnDirect *direct_find(void)
{
    struct PnDirect *d;

    Forbid();
    d = (struct PnDirect *)FindSemaphore((STRPTR)PN_DIRECT_NAME);
    if (!d && (d = AllocMem(sizeof(*d) + sizeof(PN_DIRECT_NAME),
                            MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
        char *name = (char *)(d + 1);
        strcpy(name, PN_DIRECT_NAME);
        d->ss.ss_Link.ln_Name = name;
        d->ss.ss_Link.ln_Pri = 0;
        AddSemaphore(&d->ss);           /* initialises it too; never removed */
    }
    Permit();
    return d;
}

/* Milliseconds until the earliest pending deadline (or `limit`). */
static u32_t next_deadline(u32_t limit)
{
    struct MinNode *n;
    u32_t now = sys_now();

    for (n = core.pending.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
        struct PnReq *r = node2req(n);
        if (r->has_deadline) {
            LONG left = (LONG)(r->deadline - now);
            if (left <= 0)
                return 0;
            if ((u32_t)left < limit)
                limit = (u32_t)left;
        }
    }
    return limit;
}

/* ---------------------------------------------------------------------- */
/* talking to a running core: STATUS / STOP / START                         */
/* ---------------------------------------------------------------------- */

static void print_ip(const char *label, ULONG a, const char *suffix)
{
    ip4_addr_t x;
    ip4_addr_set_u32(&x, a);
    printf("%s%s%s", label, ip4addr_ntoa(&x), suffix);
}

/* Ask the running core for its status. 1 = ok, 0 = not running, -1 = no answer. */
static int query_status(struct PnStatus *out)
{
    struct PnStatus *st = AllocVec(sizeof(*st), MEMF_PUBLIC | MEMF_CLEAR);
    LONG res;

    if (!st)
        return -1;
    res = pn_core_request(PNOP_STATUS, (ULONG)st, sizeof(*st), 0, 250);
    if (res == PNC_TIMEOUT)
        return -1;                      /* st deliberately leaked */
    if (res == PNC_NOT_RUNNING) {
        FreeVec(st);
        return 0;
    }
    *out = *st;
    FreeVec(st);
    return 1;
}

/* PrismNet SETCLOCK: ask now, then watch STATUS for the answer */
static int cmd_setclock(void)
{
    struct PnStatus st;
    ULONG before;
    LONG res = pn_core_request(PNOP_SETCLOCK, 0, 0, 0, 250);
    int i;

    if (res == PNC_NOT_RUNNING) { printf("PrismNet is not running.\n"); return RETURN_WARN; }
    if (res == PNC_TIMEOUT)     { printf("PrismNet is not responding.\n"); return RETURN_ERROR; }
    if (res < 0) {
        printf("PrismNet: network time is off - create ENV:PrismNet/time (SERVER=DHCP or a host name)\n");
        return RETURN_WARN;
    }
    if (query_status(&st) != 1) return RETURN_ERROR;
    before = st.clock_syncs;
    for (i = 0; i < 200; i++) {         /* up to 20 s: DNS, the server, one retry */
        Delay(5);
        if (query_status(&st) != 1) return RETURN_ERROR;
        if (st.clock_state == 2 && st.clock_syncs != before) {
            printf("clock set from %s, was off by %ld s\n", st.clock_server, (long)st.clock_delta);
            return RETURN_OK;
        }
    }
    printf("no answer from the time server yet (PrismNet LOG shows when it comes)\n");
    return RETURN_WARN;
}

static int cmd_log(void)
{
    char *buf = AllocVec(8192, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG after = 0;
    LONG res;

    if (!buf)
        return RETURN_FAIL;
    for (;;) {
        char *line;
        res = pn_core_request(PNOP_GETLOG, after, (ULONG)buf, 8192, 250);
        if (res == PNC_NOT_RUNNING) { printf("PrismNet is not running.\n"); FreeVec(buf); return RETURN_WARN; }
        if (res == PNC_TIMEOUT)     { printf("PrismNet is not responding.\n"); return RETURN_ERROR; }
        for (line = buf; *line; ) {
            char *tab = strchr(line, '\t'), *eol = strchr(line, '\n');
            if (!eol) break;
            *eol = 0;
            printf("%s\n", tab ? tab + 1 : line);
            line = eol + 1;
        }
        if ((ULONG)res == after || !buf[0])
            break;
        after = (ULONG)res;
    }
    FreeVec(buf);
    return RETURN_OK;
}

static const char *const state_names[] = {
    "-", "LISTEN", "SYN_SENT", "ESTABLISHED", "CLOSING", "BOUND",
};

static int cmd_netstat(void)
{
    struct PnSockInfo *si = AllocVec(64 * sizeof(*si), MEMF_PUBLIC | MEMF_CLEAR);
    LONG n, i;

    if (!si)
        return RETURN_FAIL;
    n = pn_core_request(PNOP_SOCKLIST, (ULONG)si, 64, 0, 250);
    if (n == PNC_NOT_RUNNING) { printf("PrismNet is not running.\n"); FreeVec(si); return RETURN_WARN; }
    if (n == PNC_TIMEOUT)     { printf("PrismNet is not responding.\n"); return RETURN_ERROR; }
    printf("Proto Local                 Remote                State       Recv-Q Send-Q Program\n");
    for (i = 0; i < n; i++) {
        char l[24], rm[24];
        ip4_addr_t a;
        ip4_addr_set_u32(&a, si[i].lip);
        snprintf(l, sizeof(l), "%s:%u", ip4addr_ntoa(&a), (unsigned)si[i].lport);
        ip4_addr_set_u32(&a, si[i].rip);
        if (si[i].rip || si[i].rport)
            snprintf(rm, sizeof(rm), "%s:%u", ip4addr_ntoa(&a), (unsigned)si[i].rport);
        else
            strcpy(rm, "*");
        printf("%-5s %-21s %-21s %-11s %6lu %6lu %s\n",
               si[i].type == 1 ? "tcp" : si[i].type == 2 ? "udp" : "raw", l, rm,
               state_names[si[i].state < 6 ? si[i].state : 0], (unsigned long)si[i].rxq,
               (unsigned long)si[i].txq, si[i].owner);
    }
    if (n == 0)
        printf("(no sockets)\n");
    FreeVec(si);
    return RETURN_OK;
}

/* PrismNet ROUTE: where packets go - each interface's own subnet, the static
 * routes and the default gateway. PrismNet ARP: the hardware addresses
 * PrismNet has learnt on the LAN. What people look at when something does not
 * answer. */
static int cmd_route(void)
{
    struct PnRouteInfo *ri = AllocVec(40 * sizeof(*ri), MEMF_PUBLIC | MEMF_CLEAR);
    LONG n, i;

    if (!ri)
        return RETURN_FAIL;
    n = pn_core_request(PNOP_NETCFG, PNNC_ROUTES, (ULONG)ri, 40, 250);
    if (n == PNC_NOT_RUNNING) { printf("PrismNet is not running.\n"); FreeVec(ri); return RETURN_WARN; }
    if (n == PNC_TIMEOUT)     { printf("PrismNet is not responding.\n"); return RETURN_ERROR; }
    printf("Destination        Gateway          Interface  Kind\n");
    for (i = 0; i < n; i++) {
        char d[24], g[16];
        ip4_addr_t a;
        if (ri[i].flags & PNRT_DEFAULT) {
            strcpy(d, "default");
        } else {
            ip4_addr_set_u32(&a, ri[i].net);
            snprintf(d, sizeof(d), "%s/%u", ip4addr_ntoa(&a), (unsigned)ri[i].bits);
        }
        if (ri[i].gw) {
            ip4_addr_set_u32(&a, ri[i].gw);
            strcpy(g, ip4addr_ntoa(&a));
        } else {
            strcpy(g, (ri[i].flags & PNRT_DEFAULT) ? "(link)" : "-");
        }
        printf("%-18s %-16s %-10s %s\n", d, g, ri[i].ifname[0] ? ri[i].ifname : "?",
               (ri[i].flags & PNRT_DEFAULT) ? "default" : (ri[i].flags & PNRT_CONNECTED) ? "local subnet" :
               (ri[i].flags & PNRT_HOST) ? "static, host" : "static");
    }
    if (n == 0)
        printf("(no routes: no interface has an address yet)\n");
    FreeVec(ri);
    return RETURN_OK;
}

static int cmd_arp(void)
{
    struct PnArpInfo *ai = AllocVec(64 * sizeof(*ai), MEMF_PUBLIC | MEMF_CLEAR);
    LONG n, i;

    if (!ai)
        return RETURN_FAIL;
    n = pn_core_request(PNOP_NETCFG, PNNC_ARP, (ULONG)ai, 64, 250);
    if (n == PNC_NOT_RUNNING) { printf("PrismNet is not running.\n"); FreeVec(ai); return RETURN_WARN; }
    if (n == PNC_TIMEOUT)     { printf("PrismNet is not responding.\n"); return RETURN_ERROR; }
    printf("Address          Hardware address   Interface\n");
    for (i = 0; i < n; i++) {
        ip4_addr_t a;
        ip4_addr_set_u32(&a, ai[i].ip);
        printf("%-16s %02x:%02x:%02x:%02x:%02x:%02x  %s\n", ip4addr_ntoa(&a),
               ai[i].mac[0], ai[i].mac[1], ai[i].mac[2], ai[i].mac[3], ai[i].mac[4], ai[i].mac[5],
               ai[i].ifname[0] ? ai[i].ifname : "?");
    }
    if (n == 0)
        printf("(no entries: nothing on the LAN has been talked to yet, or a serial link)\n");
    FreeVec(ai);
    return RETURN_OK;
}

static void print_status(const struct PnStatus *st)
{
    ULONG i;

    printf("PrismNet running: host %s%s%s, %lu clients, %lu sockets\n", st->hostname,
           st->domain[0] ? "." : "", st->domain, (unsigned long)st->clients,
           (unsigned long)st->sockets);
    for (i = 0; i < st->nifs; i++) {
        const struct PnIfStatus *is = &st->ifs[i];
        if (st->ifx[i].p2p)
            printf("  %s: %s unit %lu, point-to-point (%s), mtu %u, link %s%s\n",
                   is->name, is->device, (unsigned long)is->unit, p2p_kind(st->ifx[i].hwtype),
                   (unsigned)is->mtu, is->link_up ? "up" : "down",
                   is->is_default ? ", default route" : "");
        else
            printf("  %s: %s unit %lu, hw %02x:%02x:%02x:%02x:%02x:%02x, mtu %u, link %s%s\n",
                   is->name, is->device, (unsigned long)is->unit, is->mac[0], is->mac[1],
                   is->mac[2], is->mac[3], is->mac[4], is->mac[5], (unsigned)is->mtu,
                   is->link_up ? "up" : "down", is->is_default ? ", default route" : "");
        print_ip("    address  ", is->ip, is->dhcp ? " (DHCP)" : " (static)");
        print_ip("  netmask ", is->netmask, "");
        print_ip("  gateway ", is->gateway, "\n");
        printf("    rx %lu pkts/%lu bytes (%lu dropped), tx %lu pkts/%lu bytes (%lu dropped)\n",
               (unsigned long)is->rx_packets, (unsigned long)is->rx_bytes,
               (unsigned long)is->rx_dropped, (unsigned long)is->tx_packets,
               (unsigned long)is->tx_bytes, (unsigned long)is->tx_dropped);
    }
    print_ip("  dns      ", st->dns[0], "");
    if (st->dns[1]) print_ip(" ", st->dns[1], "");
    if (st->dns[2]) print_ip(" ", st->dns[2], "");
    printf("\n");
    if (st->clock_state == 2) {
        ULONG t = st->clock_set_at % 86400UL;
        printf("  clock    set from %s at %02lu:%02lu:%02lu, was off by %ld s\n", st->clock_server,
               (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60),
               (long)st->clock_delta);
    } else if (st->clock_state == 1)
        printf("  clock    waiting for the time server\n");
    {
        const struct PnMemStatus *m = &st->mem;
#define KB(x) (unsigned long)(((x) + 512) / 1024)
        printf("  memory   %lu KB in use: %lu KB loaded + %lu KB stack + %lu KB lwIP"
               " + %lu bytes sockets (peak %lu)\n",
               KB(m->image + m->stack + m->mem_used + m->mem_cached + m->dyn_used),
               KB(m->image), KB(m->stack), KB(m->mem_used + m->mem_cached),
               (unsigned long)m->dyn_used, (unsigned long)m->dyn_peak);
        printf("    stack used at most %lu of %lu bytes\n", (unsigned long)m->stack_peak,
               (unsigned long)m->stack);
        printf("    lwIP memory %lu KB (peak %lu KB, %lu KB kept for reuse) of %lu KB budget,"
               " %lu refused\n", KB(m->mem_used), KB(m->mem_peak), KB(m->mem_cached),
               KB(m->budget), (unsigned long)(m->mem_fails + m->pool_fails));
        printf("    packet buffers %lu/%lu (peak %lu), tcp %u/%u (peak %u), segments %u/%u"
               " (peak %u), udp %u/%u\n",
               (unsigned long)m->pool_used, (unsigned long)m->pool_max,
               (unsigned long)m->pool_peak, m->tcp_used, m->tcp_max, m->tcp_peak,
               m->seg_used, m->seg_max, m->seg_peak, m->udp_used, m->udp_max);
        printf("    system free %lu KB (chip %lu KB), largest block %lu KB\n",
               KB(m->sys_free), KB(m->sys_chip), KB(m->sys_largest));
#undef KB
    }
}

static int cmd_status(void)
{
    struct PnStatus st;
    switch (query_status(&st)) {
    case 0:  printf("PrismNet is not running.\n"); return RETURN_WARN;
    case -1: printf("PrismNet is not responding (core task hung or crashed).\n"); return RETURN_ERROR;
    }
    print_status(&st);
    return RETURN_OK;
}

/* STOP waits for the core's process itself, not only its port: the port
 * goes first and the network devices are closed after it, so a program
 * that takes the machine over as soon as STOP returns (WHDLoad's
 * ExecuteStartup) could still have the card's interrupt live. HOLD keeps
 * it from being started again by a program until START. */
static int cmd_stop(BOOL hold)
{
    struct MsgPort *port;
    struct Task *task = NULL;
    char name[40];
    BOOL gone = FALSE;
    int i;

    if (hold) {
        BPTR f = Open((CONST_STRPTR)PN_HELD_FILE, MODE_NEWFILE);
        if (f) {
            Write(f, "held by PrismNet STOP HOLD until PrismNet START\n", 48);
            Close(f);
        }
    }
    name[0] = 0;
    Forbid();
    if ((port = FindPort((CONST_STRPTR)PN_PORTNAME)) != NULL) {
        task = port->mp_SigTask;
        if (task->tc_Node.ln_Name) {
            strncpy(name, task->tc_Node.ln_Name, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        }
        Signal(task, SIGBREAKF_CTRL_C);
    }
    Permit();
    if (!port) {
        if (!quiet)
            printf(hold ? "PrismNet is not running; held off until PrismNet START.\n"
                        : "PrismNet is not running.\n");
        return hold ? RETURN_OK : RETURN_WARN;
    }
    for (i = 0; i < 100 && !gone; i++) {    /* up to 10 s for the process to end */
        struct Task *t;
        Forbid();
        t = name[0] ? FindTask((CONST_STRPTR)name) : NULL;
        gone = !FindPort((CONST_STRPTR)PN_PORTNAME) && t != task;
        Permit();
        if (!gone)
            Delay(5);
    }
    if (!quiet)
        printf(!gone ? "PrismNet is stopping.\n" :
               hold ? "PrismNet stopped; held off until PrismNet START.\n" : "PrismNet stopped.\n");
    return gone ? RETURN_OK : RETURN_WARN;
}

/* The reason the core gave for not starting, if it left one. */
static BOOL print_start_error(void)
{
    char buf[220];
    LONG n;
    BPTR f = Open((CONST_STRPTR)PN_ERROR_FILE, MODE_OLDFILE);

    if (!f)
        return FALSE;
    n = Read(f, buf, sizeof(buf) - 1);
    Close(f);
    buf[n > 0 ? n : 0] = 0;
    printf("PrismNet failed to start: %s", buf);
    return TRUE;
}

/* Launch "<this program> RUN QUIET" detached, wait until it is up, report. */
static int cmd_start(const char *trace_arg)
{
    char cmd[300], prog[200];
    BPTR in, out;
    struct PnStatus st;
    int i, have_addr = 0;

    if (query_status(&st) == 1) {
        printf("PrismNet is already running.\n");
        print_status(&st);
        return RETURN_WARN;
    }
    /* Same binary as this one: PROGDIR: + our file name. */
    if (!NameFromLock(GetProgramDir(), (STRPTR)prog, sizeof(prog)))
        strcpy(prog, "C:");
    {
        char name[64];
        if (!GetProgramName((STRPTR)name, sizeof(name)))
            strcpy(name, "PrismNet");
        AddPart((STRPTR)prog, FilePart((STRPTR)name), sizeof(prog));
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" RUN QUIET%s%s", prog,
             trace_arg ? " TRACE=" : "", trace_arg ? trace_arg : "");

    DeleteFile((CONST_STRPTR)PN_ERROR_FILE);
    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_Name, (ULONG)"PrismNet", TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        printf("PrismNet: cannot start %s\n", prog);
        return RETURN_FAIL;
    }
    for (i = 0; i < 100; i++) {         /* port appears within 10 s */
        if (query_status(&st) == 1) break;
        if (print_start_error())
            return RETURN_FAIL;
        Delay(5);
    }
    if (i == 100) {
        if (!print_start_error())
            printf("PrismNet failed to start - run \"PrismNet RUN\" to see why.\n");
        return RETURN_FAIL;
    }
    for (i = 0; i < 150 && !have_addr; i++) {   /* addresses (DHCP) within 15 s */
        ULONG k;
        if (query_status(&st) != 1) break;
        have_addr = st.nifs > 0;
        for (k = 0; k < st.nifs; k++)
            if (!st.ifs[k].ip) have_addr = 0;
        if (!have_addr) Delay(5);
    }
    print_status(&st);
    if (!have_addr)
        printf("  (still waiting for an address - check PrismNet STATUS later)\n");
    return RETURN_OK;
}

/* ---------------------------------------------------------------------- */
/* the core itself (RUN)                                                    */
/* ---------------------------------------------------------------------- */

static void apply_nameservers(void)
{
    int i;
    for (i = 0; i < config.nns; i++) {
        ip_addr_t d;
        ip_addr_set_ip4_u32(&d, config.nameservers[i]);
        dns_setserver((u8_t)i, &d);
    }
}

static struct PnIface *iface_of(struct netif *n)
{
    int i;
    for (i = 0; i < nifs; i++)
        if (&ifs[i].s.netif == n)
            return &ifs[i];
    return NULL;
}

static const char *p2p_kind(ULONG hwtype)
{
    switch (hwtype) {
    case S2WireType_PPP:   return "PPP";
    case S2WireType_SLIP:  return "SLIP";
    case S2WireType_CSLIP: return "CSLIP";
    case S2WireType_PLIP:  return "PLIP";
    }
    return "serial";
}

static void status_cb(struct netif *n)
{
    struct PnIface *pi = iface_of(n);

    if (ip4_addr_isany_val(*netif_ip4_addr(n)))
        return;
    if (pi && pi->cfg.dhcp)
        apply_nameservers();            /* configured servers win over DHCP's */
    start_time_service();
    {
        char a[16], m[16], g[16];
        strcpy(a, ip4addr_ntoa(netif_ip4_addr(n)));
        strcpy(m, ip4addr_ntoa(netif_ip4_netmask(n)));
        strcpy(g, ip4addr_ntoa(netif_ip4_gw(n)));
        core_log("%s: address %s netmask %s gateway %s%s", pi ? pi->cfg.name : "?", a, m, g,
                 pi && pi->cfg.dhcp ? " (DHCP)" : "");
    }
}

static void warn_cb(const char *msg)
{
    core_log("warning: %s", msg);
}

static BOOL arg_ip(LONG arg, ULONG *out, const char *what)
{
    if (!pn_parse_ip((const char *)arg, out)) {
        printf("PrismNet: bad %s address '%s'\n", what, (const char *)arg);
        return FALSE;
    }
    return TRUE;
}

/* ---- static routes (ROUTE lines in ENV:PrismNet/routes) ---------------- */
static struct PnRoute routes[PN_MAX_ROUTES];
static ip4_addr_t     route_gw[PN_MAX_ROUTES];     /* lwIP form of routes[i].gw */
static int            nroutes;

/* The longest-prefix route for dest whose gateway is on netif's subnet
 * (netif NULL: on any interface that is up). -1 = none. */
static int route_lookup(const ip4_addr_t *dest, struct netif *netif, struct netif **via)
{
    ULONG d = ip4_addr_get_u32(dest);
    int i, best = -1, k;

    for (i = 0; i < nroutes; i++) {
        if ((d & routes[i].mask) != routes[i].net)
            continue;
        if (best >= 0 && routes[i].bits <= routes[best].bits)
            continue;
        for (k = 0; k < nifs; k++) {
            struct netif *n = &ifs[k].s.netif;
            if (netif && n != netif)
                continue;
            if (netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n)) &&
                (ip4_addr_net_eq(&route_gw[i], netif_ip4_addr(n), netif_ip4_netmask(n)) ||
                 (ifs[k].s.p2p && ip4_addr_eq(&route_gw[i], netif_ip4_gw(n))))) {   /* the peer */
                best = i;
                if (via) *via = n;
                break;
            }
        }
    }
    return best;
}

/* LWIP_HOOK_IP4_ROUTE: lwIP asks only after no interface's subnet matched. */
struct netif *pn_route_netif(const ip4_addr_t *dest)
{
    struct netif *via = NULL;
    return route_lookup(dest, NULL, &via) >= 0 ? via : NULL;
}

/* LWIP_HOOK_ETHARP_GET_GW: the next hop for an off-subnet destination. */
const ip4_addr_t *pn_route_gw(struct netif *netif, const ip4_addr_t *dest)
{
    int i = route_lookup(dest, netif, NULL);
    return i >= 0 ? &route_gw[i] : NULL;
}

/* Is there any way to reach dest (network order)? On an interface's own
 * subnet, a static route, or a default gateway. Without one, connect() and
 * sendto() fail with EHOSTUNREACH at once instead of timing out. */
BOOL core_have_route(ULONG dest)
{
    ip4_addr_t d;
    int i;

    ip4_addr_set_u32(&d, dest);
    if (ip4_addr_isloopback(&d) || ip4_addr_ismulticast(&d) || dest == 0xffffffffUL)
        return TRUE;
    for (i = 0; i < nifs; i++) {
        struct netif *n = &ifs[i].s.netif;
        if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n)))
            continue;
        if (ip4_addr_net_eq(&d, netif_ip4_addr(n), netif_ip4_netmask(n)) ||
            ip4_addr_eq(&d, netif_ip4_addr(n)))
            return TRUE;
    }
    if (pn_route_netif(&d))
        return TRUE;
    if (!netif_default || !netif_is_up(netif_default))
        return FALSE;
    if (!(netif_default->flags & NETIF_FLAG_ETHARP))   /* SLIP/PPP: no next hop needed */
        return !ip4_addr_isany_val(*netif_ip4_addr(netif_default));
    return !ip4_addr_isany_val(*netif_ip4_gw(netif_default));
}

static void setup_static_routes(void)
{
    int i;

    nroutes = config.nroutes;
    for (i = 0; i < nroutes; i++) {
        ULONG n = routes[i].net = config.routes[i].net, g = config.routes[i].gw;
        routes[i].mask = config.routes[i].mask;
        routes[i].bits = config.routes[i].bits;
        routes[i].gw   = g;
        ip4_addr_set_u32(&route_gw[i], g);
        core_log("route %lu.%lu.%lu.%lu/%u via %lu.%lu.%lu.%lu",
                 (unsigned long)(PP_NTOHL(n) >> 24), (unsigned long)(PP_NTOHL(n) >> 16) & 255,
                 (unsigned long)(PP_NTOHL(n) >> 8) & 255, (unsigned long)PP_NTOHL(n) & 255,
                 (unsigned)routes[i].bits,
                 (unsigned long)(PP_NTOHL(g) >> 24), (unsigned long)(PP_NTOHL(g) >> 16) & 255,
                 (unsigned long)(PP_NTOHL(g) >> 8) & 255, (unsigned long)PP_NTOHL(g) & 255);
    }
}

/* The computer name on the LAN: NetBIOS (Windows) and mDNS (name.local).
 * NetBIOS names are at most 15 characters and have no dots; mDNS takes the
 * name as one DNS label. */
/* ---- time from the network ------------------------------------------- */

struct LocaleBase *LocaleBase;

/* Seconds east of UTC from the Workbench locale (the Amiga clock is local).
 * Returns FALSE when no Locale prefs were ever saved: locale.library then
 * answers with a built-in default (UTC+5 on 3.x), which must not be used. */
static BOOL locale_offset(LONG *out)
{
    static LONG east;
    static int known;                   /* 0 not asked, 1 from prefs, -1 no prefs */
    struct Locale *loc;
    BPTR lk;

    if (known) { *out = east; return known > 0; }
    known = -1;
    if ((lk = Lock((CONST_STRPTR)"ENV:Sys/locale.prefs", SHARED_LOCK)) != 0) {
        UnLock(lk);
        known = 1;
    }
    if ((LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR)"locale.library", 38)) != NULL) {
        if ((loc = OpenLocale(NULL)) != NULL) {
            east = -loc->loc_GMTOffset * 60L;   /* minutes west -> seconds east */
            CloseLocale(loc);
        }
        CloseLibrary((struct Library *)LocaleBase);
        LocaleBase = NULL;
    }
    *out = east;
    return known > 0;
}

/* lwIP's SNTP got an answer: sec is UTC since 1970 */
void pn_sntp_set(unsigned long sec, unsigned long us)
{
    ULONG now_s = 0, now_us = 0, want, set_s, set_us = (ULONG)us;
    const char *name = sntp_getservername(1);
    const ip_addr_t *dhcp_srv = sntp_getserver(0);
    LONG east;
    BOOL from_prefs = locale_offset(&east);

    if (!pn_systime(FALSE, &now_s, &now_us))
        return;
    if (!from_prefs) {
        /* no time zone set in Prefs/Locale: keep the zone the clock is in
         * (whole quarter hours) and only take out the drift */
        LONG diff = (LONG)(now_s + 252460800UL - (ULONG)sec);
        east = (diff + (diff < 0 ? -450 : 450)) / 900 * 900;
    }
    want = (ULONG)((long)sec - 252460800L + east);   /* 1970 -> 1978, UTC -> local */
    ntp.delta = (LONG)(want - now_s);
    if (!config.setclock)
        return;
    set_s = want;
    if (!pn_systime(TRUE, &set_s, &set_us))
        return;
    ntp.set_at = want;
    ntp.syncs++;
    ntp.state  = 2;
    if (dhcp_srv && !ip_addr_isany(dhcp_srv))
        strncpy(ntp.server, ipaddr_ntoa(dhcp_srv), sizeof(ntp.server) - 1);
    else
        strncpy(ntp.server, name ? name : "?", sizeof(ntp.server) - 1);
    core_log("clock set from %s, was off by %ld s (UTC%c%ld:%02ld %s)", ntp.server,
             (long)ntp.delta, east < 0 ? '-' : '+', (long)((east < 0 ? -east : east) / 3600),
             (long)((east < 0 ? -east : east) / 60 % 60),
             from_prefs ? "from Locale" : "kept: no time zone in Prefs/Locale");
}

static void start_time_service(void)
{
    if (ntp.running || !config.setclock)
        return;
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_servermode_dhcp(config.ntp_dhcp);
    sntp_setservername(1, config.ntp_server);
    if (!config.ntp_dhcp || ip_addr_isany(sntp_getserver(0)))
        sntp_setservername(0, config.ntp_server);   /* no DHCP server (yet): don't wait 3 s on an empty slot */
    sntp_init();
    ntp.running = TRUE;
    ntp.state = 1;
}

static void stop_time_service(void)
{
    if (ntp.running)
        sntp_stop();
    ntp.running = FALSE;
}

static void start_name_services(void)
{
    char nb[16];
    int n;
#if LWIP_MDNS_RESPONDER
    int i;
#endif

    for (n = 0; core.hostname[n] && core.hostname[n] != '.' && n < 15; n++)
        nb[n] = core.hostname[n];
    nb[n] = 0;
    if (n > 0) {
        netbiosns_set_name(nb);
        netbiosns_init();
    }
#if LWIP_MDNS_RESPONDER
    mdns_resp_init();
    for (i = 0; i < nifs; i++) {
        struct sanaif *s = &ifs[i].s;
        if (s->p2p)
            continue;                   /* no multicast on a serial link */
        if (mdns_resp_add_netif(&s->netif, nb[0] ? nb : "amiga") != ERR_OK)
            core_log("%s: mDNS could not start", ifs[i].cfg.name);
        if (s->mcast_err)
            core_log("%s: %s refused multicast (error %ld): %s.local may not resolve",
                     ifs[i].cfg.name, s->devname, (long)s->mcast_err, nb);
    }
    core_log("names: %s (NetBIOS), %s.local (mDNS)", nb, nb);
#else
    core_log("name: %s (NetBIOS)", nb);
#endif
}

static void stop_name_services(void)
{
    stop_time_service();
    netbiosns_stop();
#if LWIP_MDNS_RESPONDER
    {
        int i;
        for (i = 0; i < nifs; i++)
            if (!ifs[i].s.p2p)
                mdns_resp_remove_netif(&ifs[i].s.netif);
    }
#endif
}

/* ---- servers: inetd, when ENV:PrismNet/services lists any ----------------
 * Started once the stack is up and told to stop (CTRL-C) when it goes down.
 * Not waited for: inetd's own last calls need this task, which holds the
 * request semaphore while it is not asleep. */
#define INETD_NAME "PrismNet inetd"

static void start_services(void)
{
    BPTR lk = Lock((CONST_STRPTR)PN_CONFIG_DIR "/services", ACCESS_READ), in, out;
    struct Task *t;
    char cmd[128];

    if (!lk)
        return;
    UnLock(lk);
    Forbid();
    t = FindTask((CONST_STRPTR)INETD_NAME);
    Permit();
    if (t)
        return;                         /* one from an earlier run is still going */
    snprintf(cmd, sizeof(cmd), "%s QUIET", config.inetd);
    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out ||
        SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_Name, (ULONG)INETD_NAME, NP_StackSize, 8192, NP_Priority, 0, TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        core_log("services: cannot start %s", config.inetd);
        return;
    }
    core_log("services: %s started", config.inetd);
}

static void stop_services(void)
{
    struct Task *t;
    Forbid();
    if ((t = FindTask((CONST_STRPTR)INETD_NAME)) != NULL)
        Signal(t, SIGBREAKF_CTRL_C);
    Permit();
}

/* Choose the default interface and put the default gateway on it. */
static void setup_default_route(void)
{
    struct PnIface *def = NULL;
    int i;

    if (config.default_gw) {
        for (i = 0; i < nifs && !def; i++) {
            struct PnIfConfig *ic = &ifs[i].cfg;
            if (!ic->dhcp && ((config.default_gw ^ ic->address) & ic->netmask) == 0)
                def = &ifs[i];
        }
        if (def && !def->cfg.gateway) {
            ip4_addr_t gw;
            ip4_addr_set_u32(&gw, config.default_gw);
            netif_set_gw(&def->s.netif, &gw);
        } else if (!def) {
            warn_cb("routes: DEFAULT gateway is not on a static interface's subnet");
        }
    }
    for (i = 0; i < nifs && !def; i++)
        if (ifs[i].cfg.gateway || ifs[i].cfg.dhcp)
            def = &ifs[i];
    if (!def && nifs)
        def = &ifs[0];
    if (def)
        netif_set_default(&def->s.netif);
}

/* ---- run-time configuration: PNOP_NETCFG ---------------------------------
 * Roadshow's routing, interface and DNS calls (AddRouteTagList,
 * ConfigureInterfaceTagList, AddDomainNameServer ...) and the route and ARP
 * lists. A change lasts until PrismNet stops; ENV:PrismNet is not written. */

#include "lwip/etharp.h"

static BOOL ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return FALSE;
    }
    return *a == *b;
}

static int iface_index(const char *name)
{
    int i;
    for (i = 0; name && i < nifs; i++)
        if (ieq(ifs[i].cfg.name, name))
            return i;
    return -1;
}

static UBYTE mask_bits(ULONG mask)
{
    ULONG m = PP_NTOHL(mask);
    UBYTE n = 0;
    while (m & 0x80000000UL) { n++; m <<= 1; }
    return n;
}

static void log_ip(const char *what, ULONG a, ULONG b)
{
    char x[16], y[16];
    ip4_addr_t ia, ib;
    ip4_addr_set_u32(&ia, a); ip4_addr_set_u32(&ib, b);
    strcpy(x, ip4addr_ntoa(&ia));
    strcpy(y, b ? ip4addr_ntoa(&ib) : "");
    core_log(what, x, y);
}

static LONG nc_route_add(ULONG net, ULONG mask, ULONG gw)
{
    ip4_addr_t a;
    int i;

    if (!gw)
        return PN_EINVAL;
    if (!net && !mask) {                /* the default gateway */
        for (i = 0; i < nifs; i++) {
            struct netif *n = &ifs[i].s.netif;
            if (ifs[i].s.p2p || (!ip4_addr_isany_val(*netif_ip4_addr(n)) &&
                ((gw ^ ip4_addr_get_u32(netif_ip4_addr(n))) & ip4_addr_get_u32(netif_ip4_netmask(n))) == 0))
                break;
        }
        if (i == nifs)
            return PN_ENETUNREACH;
        ip4_addr_set_u32(&a, gw);
        netif_set_gw(&ifs[i].s.netif, &a);
        netif_set_default(&ifs[i].s.netif);
        config.default_gw = gw;
        log_ip("default gateway now %s%s", gw, 0);
        return 0;
    }
    if (net & ~mask)
        return PN_EINVAL;
    for (i = 0; i < nroutes && !(routes[i].net == net && routes[i].mask == mask); i++)
        ;
    if (i == nroutes) {
        if (nroutes >= PN_MAX_ROUTES)
            return PN_ENOBUFS;
        nroutes++;
    }
    routes[i].net = net; routes[i].mask = mask; routes[i].gw = gw;
    routes[i].bits = mask_bits(mask);
    ip4_addr_set_u32(&route_gw[i], gw);
    log_ip("route %s added via %s", net, gw);
    return 0;
}

static LONG nc_route_del(ULONG net, ULONG mask, ULONG gw)
{
    int i;

    if (!net && !mask) {
        struct netif *d = netif_default;
        if (!d || ip4_addr_isany_val(*netif_ip4_gw(d)) ||
            (gw && gw != ip4_addr_get_u32(netif_ip4_gw(d))))
            return PN_ESRCH;
        netif_set_gw(d, IP4_ADDR_ANY4);
        config.default_gw = 0;
        core_log("default gateway removed");
        return 0;
    }
    for (i = 0; i < nroutes; i++)
        if (routes[i].net == net && routes[i].mask == mask && (!gw || routes[i].gw == gw))
            break;
    if (i == nroutes)
        return PN_ESRCH;
    log_ip("route %s via %s removed", net, routes[i].gw);
    for (nroutes--; i < nroutes; i++) {
        routes[i] = routes[i + 1];
        route_gw[i] = route_gw[i + 1];
    }
    return 0;
}

static LONG nc_dns(BOOL add, ULONG addr)
{
    int i, k;
    BOOL found = FALSE;

    if (!addr)
        return PN_EINVAL;
    for (i = 0; i < config.nns; i++)
        if (config.nameservers[i] == addr)
            break;
    if (add) {
        if (i < config.nns)
            return 0;
        if (config.nns >= 3)
            return PN_ENOBUFS;
        config.nameservers[config.nns++] = addr;
        apply_nameservers();
        log_ip("name server %s added%s", addr, 0);
        return 0;
    }
    if (i < config.nns) {
        for (config.nns--; i < config.nns; i++)
            config.nameservers[i] = config.nameservers[i + 1];
        found = TRUE;
    }
    for (k = 0; k < DNS_MAX_SERVERS; k++)       /* also one DHCP gave us */
        if (ip4_addr_get_u32(ip_2_ip4(dns_getserver((u8_t)k))) == addr) {
            dns_setserver((u8_t)k, NULL);
            found = TRUE;
        }
    if (!found)
        return PN_ESRCH;
    apply_nameservers();
    log_ip("name server %s removed%s", addr, 0);
    return 0;
}

static LONG nc_if_set(const char *name, ULONG what, ULONG v)
{
    int i = iface_index(name);
    struct netif *n;
    ip4_addr_t a;

    if (i < 0)
        return PN_ENODEV;
    n = &ifs[i].s.netif;
    ip4_addr_set_u32(&a, v);
    switch (what) {
    case PNIF_ADDRESS:
        if (ifs[i].cfg.dhcp) {          /* a fixed address replaces DHCP */
            dhcp_release_and_stop(n);
            ifs[i].cfg.dhcp = FALSE;
        }
        netif_set_ipaddr(n, &a);
        ifs[i].cfg.address = v;
        status_cb(n);
        return 0;
    case PNIF_NETMASK:
        netif_set_netmask(n, &a);
        ifs[i].cfg.netmask = v;
        return 0;
    case PNIF_PEER:                     /* the far end of a point-to-point link */
        netif_set_gw(n, &a);
        return 0;
    case PNIF_STATE:                    /* SM_Offline 0, SM_Online 1, SM_Down 2, SM_Up 3 */
        if (v == 1 || v == 3) {
            netif_set_link_up(n);
            netif_set_up(n);
        } else if (v == 0 || v == 2) {
            netif_set_down(n);
        } else {
            return PN_EINVAL;
        }
        core_log("%s: %s", ifs[i].cfg.name, (v & 1) ? "up" : "down");
        return 0;
    case PNIF_MTU:
        if (v < 68)
            return PN_EINVAL;
        if (v < n->mtu)                 /* only lower: the driver's limit stands */
            n->mtu = (u16_t)v;
        return 0;
    }
    return PN_EINVAL;
}

/* interface slots whose driver kept requests at removal: never reused or freed */
static ULONG lost_slots;

static LONG nc_if_add(const char *name, const char *device, ULONG unit)
{
    struct PnIface *pi;
    const char *why = "";
    LONG e;

    if (!name || !name[0] || !device || !device[0] || strlen(name) >= sizeof(pi->cfg.name) ||
        strlen(device) >= sizeof(pi->cfg.device))
        return PN_EINVAL;
    if (iface_index(name) >= 0)
        return PN_EEXIST;
    if (nifs >= PN_MAX_IFACES || (lost_slots & (1UL << nifs)))
        return PN_ENOBUFS;
    pi = &ifs[nifs];
    memset(pi, 0, sizeof(*pi));
    e = sanaif_open(&pi->s, device, unit, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, &why);
    if (e != 0) {
        core_log("%s: %s unit %lu: %s (error %ld)", name, device, (unsigned long)unit, why, (long)e);
        return PN_ENODEV;
    }
    strcpy(pi->cfg.name, name);
    strcpy(pi->cfg.device, device);
    pi->cfg.unit = unit;
    {
        struct DateStamp ds;
        DateStamp(&ds);
        pi->up_secs = ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
    }
    nifs++;
    netif_set_status_callback(&pi->s.netif, status_cb);
    netif_set_hostname(&pi->s.netif, core.hostname);
    netif_set_up(&pi->s.netif);
    if (!netif_default)
        netif_set_default(&pi->s.netif);
    core_log("%s: %s unit %lu added", name, device, (unsigned long)unit);
    return 0;
}

static LONG nc_if_remove(const char *name)
{
    int i = iface_index(name);
    struct PnIface *pi;

    if (i < 0)
        return PN_ENODEV;
    if (i != nifs - 1 || nifs == 1)     /* the interfaces after it would have to move */
        return PN_EBUSY;
    pi = &ifs[i];
    if (pi->cfg.dhcp)
        dhcp_release_and_stop(&pi->s.netif);
    dhcp_cleanup(&pi->s.netif);         /* free its struct dhcp */
#if LWIP_MDNS_RESPONDER
    if (!pi->s.p2p)
        mdns_resp_remove_netif(&pi->s.netif);   /* ERR_VAL if it never had one */
#endif
    sanaif_close(&pi->s);               /* also takes it out of lwIP's list */
    if (sanaif_requests_lost(&pi->s))
        lost_slots |= 1UL << i;         /* the driver still holds this slot's I/O */
    nifs--;
    if (!netif_default)
        netif_set_default(&ifs[0].s.netif);
    core_log("%s: removed", name);
    return 0;
}

static LONG nc_routes(struct PnRouteInfo *out, LONG max)
{
    LONG n = 0;
    int i;

    for (i = 0; i < nifs && n < max; i++) {     /* each interface's own subnet */
        struct netif *ni = &ifs[i].s.netif;
        ULONG a = ip4_addr_get_u32(netif_ip4_addr(ni)), m = ip4_addr_get_u32(netif_ip4_netmask(ni));
        if (!a || !netif_is_up(ni))
            continue;
        memset(&out[n], 0, sizeof(*out));
        out[n].net = a & m; out[n].mask = m; out[n].bits = mask_bits(m);
        out[n].flags = PNRT_CONNECTED;
        strcpy(out[n].ifname, ifs[i].cfg.name);
        n++;
    }
    for (i = 0; i < nroutes && n < max; i++) {
        struct netif *via = NULL;
        ip4_addr_t d;
        int k;
        memset(&out[n], 0, sizeof(*out));
        out[n].net = routes[i].net; out[n].mask = routes[i].mask; out[n].gw = routes[i].gw;
        out[n].bits = routes[i].bits;
        out[n].flags = PNRT_STATIC | (routes[i].bits == 32 ? PNRT_HOST : 0);
        ip4_addr_set_u32(&d, routes[i].net);
        route_lookup(&d, NULL, &via);
        for (k = 0; k < nifs; k++)
            if (&ifs[k].s.netif == via)
                strcpy(out[n].ifname, ifs[k].cfg.name);
        n++;
    }
    if (netif_default && n < max && (!ip4_addr_isany_val(*netif_ip4_gw(netif_default)) ||
                                     !(netif_default->flags & NETIF_FLAG_ETHARP))) {
        struct PnIface *pi = iface_of(netif_default);
        memset(&out[n], 0, sizeof(*out));
        out[n].gw = ip4_addr_get_u32(netif_ip4_gw(netif_default));
        out[n].flags = PNRT_DEFAULT;
        if (pi) strcpy(out[n].ifname, pi->cfg.name);
        n++;
    }
    return n;
}

static LONG nc_arp(struct PnArpInfo *out, LONG max)
{
    LONG n = 0;
#if LWIP_ARP
    size_t i;
    for (i = 0; i < ARP_TABLE_SIZE && n < max; i++) {
        ip4_addr_t *ip;
        struct netif *ni;
        struct eth_addr *eth;
        struct PnIface *pi;
        if (!etharp_get_entry(i, &ip, &ni, &eth))
            continue;
        memset(&out[n], 0, sizeof(*out));
        out[n].ip = ip4_addr_get_u32(ip);
        memcpy(out[n].mac, eth->addr, 6);
        out[n].stable = 1;
        if ((pi = iface_of(ni)) != NULL)
            strcpy(out[n].ifname, pi->cfg.name);
        n++;
    }
#endif
    return n;
}

static LONG netcfg(struct PnReq *r, LONG *err)
{
    LONG e = PN_EINVAL, n = 0;

    switch (r->a[0]) {
    case PNNC_ROUTE_ADD: e = nc_route_add(r->a[1], r->a[2], r->a[3]); break;
    case PNNC_ROUTE_DEL: e = nc_route_del(r->a[1], r->a[2], r->a[3]); break;
    case PNNC_DNS_ADD:   e = nc_dns(TRUE, r->a[1]); break;
    case PNNC_DNS_DEL:   e = nc_dns(FALSE, r->a[1]); break;
    case PNNC_DOMAIN:
        strncpy(core.domain, r->a[1] ? (const char *)r->a[1] : "", sizeof(core.domain) - 1);
        core.domain[sizeof(core.domain) - 1] = 0;
        core_log("default domain now \"%s\"", core.domain);
        e = 0;
        break;
    case PNNC_IF_SET:    e = nc_if_set((const char *)r->a[1], r->a[2], r->a[3]); break;
    case PNNC_IF_ADD:    e = nc_if_add((const char *)r->a[1], (const char *)r->a[2], r->a[3]); break;
    case PNNC_IF_REMOVE: e = nc_if_remove((const char *)r->a[1]); break;
    case PNNC_ROUTES:    n = nc_routes((struct PnRouteInfo *)r->a[1], (LONG)r->a[2]); e = 0; break;
    case PNNC_ARP:       n = nc_arp((struct PnArpInfo *)r->a[1], (LONG)r->a[2]); e = 0; break;
    }
    *err = e;
    return n;
}

int main(void)
{
    LONG args[A_COUNT] = { 0 };
    ULONG sigs;
    BOOL timed;
    struct RDArgs *rda;
    char errbuf[160];
    int rc = RETURN_OK, i;

    (void)version;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"PrismNet");
        return RETURN_FAIL;
    }
    if (args[A_STATUS]) { rc = cmd_status(); FreeArgs(rda); return rc; }
    quiet = args[A_QUIET] != 0;
    if (args[A_STOP])   { rc = cmd_stop(args[A_HOLD] != 0); FreeArgs(rda); return rc; }
    if (args[A_LOG])    { rc = cmd_log();    FreeArgs(rda); return rc; }
    if (args[A_NETSTAT]){ rc = cmd_netstat(); FreeArgs(rda); return rc; }
    if (args[A_ROUTE])  { rc = cmd_route();   FreeArgs(rda); return rc; }
    if (args[A_ARP])    { rc = cmd_arp();     FreeArgs(rda); return rc; }
    if (args[A_SETCLOCK]){ rc = cmd_setclock(); FreeArgs(rda); return rc; }
    /* anything that starts the core ends a STOP HOLD */
    DeleteFile((CONST_STRPTR)PN_HELD_FILE);
    /* START (or no arguments at all): detach and report. DEVICE= or RUN: foreground. */
    if (args[A_START] || (!args[A_RUN] && !args[A_DEVICE])) {
        rc = cmd_start((const char *)args[A_TRACE]);
        FreeArgs(rda);
        return rc;
    }

    quiet = args[A_QUIET] != 0;
    if (args[A_TRACE])
        trace = fopen((const char *)args[A_TRACE], "w");

    /* ---- configuration: ENV:PrismNet, or one interface from the command line ---- */
    if (!pn_config_load(&config, errbuf, sizeof(errbuf), warn_cb)) {
        startup_fail("%s", errbuf);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (args[A_DEVICE]) {
        struct PnIfConfig *ic = &config.ifs[0];
        memset(ic, 0, sizeof(*ic));
        config.nifs = 1;
        config.default_gw = 0;
        strcpy(ic->name, "eth0");
        strncpy(ic->device, (const char *)args[A_DEVICE], sizeof(ic->device) - 1);
        ic->unit = args[A_UNIT] ? *(LONG *)args[A_UNIT] : 0;
        ic->dhcp = !args[A_IP];
        if (!ic->dhcp) {
            if (!arg_ip(args[A_IP], &ic->address, "IP") ||
                !arg_ip(args[A_NETMASK] ? args[A_NETMASK] : (LONG)"255.255.255.0", &ic->netmask, "NETMASK") ||
                (args[A_GATEWAY] && !arg_ip(args[A_GATEWAY], &ic->gateway, "GATEWAY"))) {
                FreeArgs(rda);
                return RETURN_ERROR;
            }
        }
    }
    if (args[A_DNS] || args[A_DNS2]) {
        config.nns = 0;
        if (args[A_DNS]  && arg_ip(args[A_DNS],  &config.nameservers[config.nns], "DNS"))  config.nns++;
        if (args[A_DNS2] && arg_ip(args[A_DNS2], &config.nameservers[config.nns], "DNS2")) config.nns++;
    }
    if (args[A_HOSTNAME])
        strncpy(config.hostname, (const char *)args[A_HOSTNAME], sizeof(config.hostname) - 1);
    if (config.nifs == 0) {
        printf("PrismNet: no interfaces configured.\n"
               "  Create ENV:PrismNet/interfaces/eth0 (and ENVARC:) with e.g.\n"
               "    DEVICE=cnet.device UNIT=0 CONFIG=DHCP\n"
               "  or start it with PrismNet DEVICE=<driver> [UNIT=n].\n");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    pn_newlist(&core.clients);
    pn_newlist(&core.sockets);
    pn_newlist(&core.pending);
    strncpy(core.hostname, config.hostname, sizeof(core.hostname) - 1);
    strncpy(core.domain, config.domain, sizeof(core.domain) - 1);
    core.next_release_id = 1000;

    /* Single instance: claim the public port. */
    core_port = CreateMsgPort();
    if (!core_port) { FreeArgs(rda); return RETURN_FAIL; }
    core_port->mp_Node.ln_Name = (char *)PN_PORTNAME;
    core_port->mp_Node.ln_Pri  = 0;
    Forbid();
    if (FindPort((CONST_STRPTR)PN_PORTNAME)) {
        Permit();
        printf("PrismNet is already running.\n");
        DeleteMsgPort(core_port);
        FreeArgs(rda);
        return RETURN_WARN;
    }
    AddPort(core_port);
    port_added = TRUE;
#ifdef PN_DIAGPORT
    pdiag.magic = 0x50444941;
    pdiag.mp.mp_Node.ln_Name = (char *)"PrismNet.diag";
    pdiag.mp.mp_Node.ln_Type = NT_MSGPORT;
    pdiag.mp.mp_Flags = PA_IGNORE;
    pn_newlist((struct MinList *)&pdiag.mp.mp_MsgList);
    AddPort(&pdiag.mp);
#endif
    Permit();
    /* __stack only takes effect with libnix's swapstack.o linked in (see the
     * Makefile); without it the core silently ran on 4 KB and overran it. */
    {
        ULONG st = core_stack_mark();
        if (st < 16384) {
            startup_fail("only %lu bytes of stack, need 16384 or more", (unsigned long)st);
            rc = RETURN_FAIL;
            goto out_port;
        }
    }
    sock_init();
    SetTaskPri(FindTask(NULL), CORE_PRI);

    /* Load bsdsocket.library now and keep it resident, so many programs
     * opening it at once never race ramlib's first load. The library
     * recognises the core's task and registers no client for it. */
    self_base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!self_base && !quiet)
        printf("PrismNet: warning: LIBS:bsdsocket.library not found or not PrismNet's\n");

    if (!pn_sys_init()) {
        startup_fail("cannot open timer.device");
        rc = RETURN_FAIL;
        goto out_port;
    }
    pn_mem_init(config.mem_budget);
    lwip_init();

    /* ---- interfaces ---- */
    /* room for the most interfaces: AddInterfaceTagList can add one later,
     * and lwIP keeps pointers into this array, so it can never move */
    if (!(ifs = pn_alloc(PN_MAX_IFACES * sizeof(*ifs), MEMF_PUBLIC | MEMF_CLEAR))) {
        startup_fail("not enough memory for the interfaces (%lu bytes)",
                     (unsigned long)(PN_MAX_IFACES * sizeof(*ifs)));
        rc = RETURN_FAIL;
        goto out_sys;
    }
    for (i = 0; i < config.nifs; i++) {
        struct PnIfConfig *ic = &config.ifs[i];
        struct PnIface *pi = &ifs[nifs];
        ip4_addr_t ip, mask, gw;
        const char *why;
        LONG err;

        ip4_addr_set_u32(&ip, ic->dhcp ? 0 : ic->address);
        ip4_addr_set_u32(&mask, ic->dhcp ? 0 : ic->netmask);
        ip4_addr_set_u32(&gw, ic->dhcp ? 0 : ic->gateway);
        if (ic->slip)
            err = serslip_open(&pi->s, ic->device, ic->unit, ic->baud, ic->rtscts, ic->mtu,
                               &ip, &mask, &gw, &why);
        else
            err = sanaif_open(&pi->s, ic->device, ic->unit, &ip, &mask, &gw, &why);
        if (err != 0) {
            core_log("%s: %s unit %lu: %s (error %ld)", ic->name, ic->device,
                     (unsigned long)ic->unit, why, (long)err);
            continue;
        }
        pi->cfg = *ic;
        if (ic->stall)
            pi->s.stall_ms = pi->s.stall_base = ic->stall;
        {
            struct DateStamp ds;
            DateStamp(&ds);
            pi->up_secs = ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
        }
        nifs++;
        if (ic->mtu && ic->mtu < pi->s.netif.mtu)
            pi->s.netif.mtu = (u16_t)ic->mtu;
        if (pi->s.p2p) {
            /* No DHCP on a serial link: the address is configured, or the
             * driver's own (rhslip's IP, PPP's IPCP result). */
            pi->cfg.dhcp = FALSE;
            core_log("%s: %s unit %lu, point-to-point (%s), mtu %u", ic->name, ic->device,
                     (unsigned long)ic->unit, p2p_kind(pi->s.hwtype), (unsigned)pi->s.netif.mtu);
            if (ip4_addr_isany_val(*netif_ip4_addr(&pi->s.netif)))
                core_log("%s: no address - set ADDRESS= in its interface file", ic->name);
        } else {
            core_log("%s: %s unit %lu, hw %02x:%02x:%02x:%02x:%02x:%02x, mtu %u", ic->name,
                     ic->device, (unsigned long)ic->unit, pi->s.mac[0], pi->s.mac[1],
                     pi->s.mac[2], pi->s.mac[3], pi->s.mac[4], pi->s.mac[5],
                     (unsigned)pi->s.netif.mtu);
        }
    }
    if (nifs == 0) {
        startup_fail("no interface could be opened - see PrismNet LOG, or run PrismNet RUN");
        rc = RETURN_FAIL;
        goto out_sys;
    }
    core_log("PrismNet 1.0.7 (lwIP %s) started, host %s, %d interface%s", LWIP_VERSION_STRING,
             core.hostname, nifs, nifs == 1 ? "" : "s");

    setup_default_route();
    setup_static_routes();
    start_name_services();
    start_services();
    apply_nameservers();
    for (i = 0; i < nifs; i++) {
        struct netif *n = &ifs[i].s.netif;
        netif_set_status_callback(n, status_cb);
        netif_set_hostname(n, core.hostname);
        netif_set_up(n);
        if (ifs[i].cfg.dhcp)
            dhcp_start(n);
        else
            status_cb(n);
    }

    /*
     * ---- main loop ---- A pass after a bare client message (the common case: every
     * library call is one) does no time keeping: lwIP's timeouts only become
     * due when the wakeup timer fires, and the timer only needs re-arming once
     * it has fired or a request got a deadline. Every lwIP timeout is set at
     * least 250 ms ahead (TCP's is the shortest), and the timer is never armed
     * further ahead than that, so it always wakes the loop in time.
     */
    timed = TRUE;                       /* first pass: full time keeping */
    sigs  = ~0UL;                       /* ... and polls everything */
    /* Direct calls: clients run requests in their own task while the core
     * sleeps. It holds the semaphore whenever it is not in Wait(). TRACE
     * logs through this task's stdio, so it keeps everything on messages. */
    if ((direct = direct_find()) != NULL) {
        ObtainSemaphore(&direct->ss);
        if (!trace)
            direct->run = core_direct;
    }
    if (setjmp(assert_jmp) != 0) {
        rc = RETURN_FAIL;
        goto shutdown;
    }
    assert_armed = TRUE;
    for (;;) {
        ULONG wait = (1UL << core_port->mp_SigBit) | pn_timer_sigmask() | SIGBREAKF_CTRL_C;
        struct PnReq *r;
        u32_t sleep;

        PROF(0);
        while ((r = (struct PnReq *)GetMsg(core_port)) != NULL)
            handle_message(r);
        PROF(1);
        for (i = 0; i < nifs; i++) {
            /* A driver reply sets the port's signal, which stays set until
             * the next Wait: an unsignalled port is empty. Timed passes poll
             * anyway, to requeue reads that found no free pbuf. */
            if (timed || (sigs & sanaif_sigmask(&ifs[i].s)))
                sanaif_poll(&ifs[i].s);
            wait |= sanaif_sigmask(&ifs[i].s);
        }
        PROF(2);
        netif_poll_all();               /* loopback */
        if (timed) {
            sys_check_timeouts();
            for (i = 0; i < nifs; i++)
                if (sanaif_watchdog(&ifs[i].s, sys_now()))
                    core_log("%s: %s stopped answering, restarted it", ifs[i].cfg.name,
                             ifs[i].s.devname);
        }
        PROF(3);
        service_pending();
        PROF(4);
        sock_check_events();
        netif_poll_all();               /* loopback output made during this pass */
        PROF(5);

#ifdef PN_DIAGPORT
        {
            struct MinNode *n; ULONG np = 0, tb = 0, rb = 0; int k;
            for (n = core.pending.mlh_Head; n->mln_Succ; n = n->mln_Succ) np++;
            for (k = 0; k < SANAIF_NUM_TX; k++) tb += ifs[0].s.tx[k].busy;
            for (k = 0; k < SANAIF_NUM_RX; k++) rb += ifs[0].s.rx[k].busy;
            pdiag.pending = np; pdiag.txbusy = tb; pdiag.rxbusy = rb;
            pdiag.passes++;
        }
#endif
        if (timed || deadline_added || !pn_timer_armed()) {
            deadline_added = FALSE;
            sleep = next_deadline(sys_timeouts_sleeptime());
            if (sleep > 250) sleep = 250;
            if (sleep == 0)  sleep = 1;
            pn_timer_arm(sleep);
        }
        PROF(6);
        PROF_REPORT();
        DIAG(stage, 2); DIAG(waitmask, wait); DIAG(timer_busy, pn_timer_armed());
        DIAG(timer_due, pn_timer_due()); DIAG(now, sys_now());
        if (direct)
            ReleaseSemaphore(&direct->ss);
        sigs = Wait(wait);
        if (direct)
            ObtainSemaphore(&direct->ss);
        if (direct_assert) {
            rc = RETURN_FAIL;
            break;
        }
        DIAG(stage, 1); DIAG(sigs, sigs);
        timed = pn_timer_fired();
        DIAG(timed, timed);
        if (sigs & SIGBREAKF_CTRL_C)
            break;
    }

    /* ---- shutdown: refuse new requests, fail everything outstanding ---- */
shutdown:
    assert_armed = FALSE;
    if (direct)
        direct->run = NULL;             /* held: no client is inside */
    stop_services();
    close_public_port();
    while (core.clients.mlh_Head->mln_Succ)
        free_client((struct PnClient *)core.clients.mlh_Head);
    sock_free_all();
    if (nifs > 0)
        stop_name_services();
    for (i = 0; i < nifs; i++) {
        if (ifs[i].cfg.dhcp) {
            dhcp_release_and_stop(&ifs[i].s.netif);
            Delay(2);                   /* let the RELEASE leave before I/O is aborted */
            sanaif_poll(&ifs[i].s);
        }
        sanaif_close(&ifs[i].s);
    }
    {
        BOOL held = FALSE;
        for (i = 0; i < nifs; i++)
            held |= sanaif_requests_lost(&ifs[i].s);
        if (lost_slots)
            held = TRUE;                /* a removed interface's driver still has I/O */
        if (!held && ifs) {
            pn_free(ifs);
            ifs = NULL;
        }
    }
    pn_mem_cleanup();
    core_log("stopped");
out_sys:
    pn_sys_cleanup();
out_port:
    close_public_port();                /* also for the early exits above */
    if (direct) {
        direct->run = NULL;
        ReleaseSemaphore(&direct->ss);  /* the semaphore itself stays, for good */
        direct = NULL;
    }
    DeleteMsgPort(core_port);
    if (trace)
        fclose(trace);
    FreeArgs(rda);
    return rc;
}