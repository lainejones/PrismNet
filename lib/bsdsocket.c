/*
 * PrismNet bsdsocket.library - AmiTCP-compatible API front-end.
 * GPL-3.0-only, see LICENSE.
 *
 * Every OpenLibrary() gets its own library base (a copy of the master's
 * jump table plus per-opener state: errno, signal masks, resolver buffers,
 * the core-side client handle). Calls are forwarded to the PrismNet core
 * task as PnReq messages, see port/include/prismnet/pnmsg.h.
 *
 * All entry points use the Amiga register convention (base in a6).
 * Built -nostartfiles: no libnix startup, only its pure string helpers.
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <utility/tagitem.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "prismnet/pnmsg.h"

#define REG(r) __asm(#r)

struct ExecBase   *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;    /* only for auto-starting the core */

struct pn_servent  { char *s_name; char **s_aliases; LONG s_port; char *s_proto; };
struct pn_protoent { char *p_name; char **p_aliases; LONG p_proto; };

struct PnBase {
    struct Library   lib;
    struct PnBase   *master;        /* NULL in the master base */
    struct MsgPort  *coreport;      /* master: the running core's port, set while
                                     * the core has the library open */
    BOOL             is_core;       /* this base was opened by the core itself */
    BPTR             seglist;       /* master only */
    struct Task     *owner;
    struct MsgPort  *port;          /* reply port, signal owned by `owner` */
    APTR             client;        /* core-side handle */
    LONG             err;
    LONG             herr;
    APTR             errnoptr;
    LONG             errnosize;
    LONG            *herrnoptr;
    ULONG            breakmask, sigiomask, sigurgmask, sigeventmask;
    STRPTR           logtag;
    LONG             logstat, logfacility, logmask;
    struct PnHostBuf hostbuf;
    struct pn_servent  servent;
    struct pn_protoent protoent;
    char            *noaliases[1];
    char             ntoa[20];
    char             ifq_device[64];    /* IFQ_DeviceName points here */
    UBYTE           *dstack;            /* private stack for direct calls */
};

/* ---------------------------------------------------------------------- */
/* errno handling                                                           */
/* ---------------------------------------------------------------------- */

static void set_errno(struct PnBase *b, LONG e)
{
    b->err = e;
    if (b->errnoptr) {
        switch (b->errnosize) {
        case 1: *(UBYTE *)b->errnoptr = (UBYTE)e; break;
        case 2: *(UWORD *)b->errnoptr = (UWORD)e; break;
        default: *(LONG *)b->errnoptr = e; break;
        }
    }
}

static void set_herrno(struct PnBase *b, LONG e)
{
    b->herr = e;
    if (b->herrnoptr)
        *b->herrnoptr = e;
}

/* ---------------------------------------------------------------------- */
/* talking to the core                                                      */
/* ---------------------------------------------------------------------- */

/* Take every message off our reply port, noting which requests came back.
 * A request only counts as done once it is off the port: checking ln_Type
 * alone races with the core's ReplyMsg and can leave a stack request
 * linked into the port after the call returns. */
static void reap(struct MsgPort *port, struct PnReq *r, BOOL *rdone, struct PnReq *a, BOOL *adone)
{
    struct Message *m;

    while ((m = GetMsg(port)) != NULL) {
        if (m == &r->msg)
            *rdone = TRUE;
        else if (a && m == &a->msg)
            *adone = TRUE;
    }
}

/* The core's port is cached in the master base: FindPort() walks every public
 * port with a string compare, on every call. The core sets and clears the
 * cache by opening/closing the library (under Forbid, before its port goes). */
static BOOL send_to_core(struct PnBase *b, struct PnReq *r)
{
    struct MsgPort *core;

    r->msg.mn_Node.ln_Type = NT_MESSAGE;
    Forbid();
    core = b->master ? b->master->coreport : NULL;
    if (!core)
        core = FindPort((CONST_STRPTR)PN_PORTNAME);
    if (core)
        PutMsg(core, &r->msg);
    Permit();
    return core != NULL;
}

/*
 * Send `r` and wait for the reply. Break signals (and `extra`, for
 * WaitSelect) interrupt a pending call via PNOP_ABORT. Signals that
 * arrived are returned in *got. Returns r->result; errno is set on error.
 */
/* ---- direct calls: the core's handler in the caller's task --------------
 * See struct PnDirect in pnmsg.h. The handler runs on a private stack per
 * base, so a program with a small stack is safe; 6 KB is well above the
 * core's whole loop (about 4.2 KB at most). */
#define DSTACK 6144

/* fn(arg) with the stack pointer at `top` (support.S) */
LONG pn_on_stack(APTR fn, APTR arg, APTR top);

static struct PnDirect *direct;         /* public and never freed: cached */

static struct PnDirect *get_direct(void)
{
    if (!direct) {
        Forbid();
        direct = (struct PnDirect *)FindSemaphore((STRPTR)PN_DIRECT_NAME);
        Permit();
    }
    return direct;
}

/* 1 = answered, 0 = pending (the core replies by message), -1 = not run */
static int direct_call(struct PnBase *b, struct PnReq *r)
{
    struct PnDirect *d = get_direct();
    LONG answered;

    if (!d || (!b->dstack && !(b->dstack = AllocMem(DSTACK, MEMF_ANY))))
        return -1;
    ObtainSemaphore(&d->ss);
    if (!d->run) {                      /* no core */
        ReleaseSemaphore(&d->ss);
        return -1;
    }
    answered = pn_on_stack((APTR)d->run, r, b->dstack + DSTACK);
    ReleaseSemaphore(&d->ss);
    return (answered & 0xffff) ? 1 : 0; /* the handler returns a 16-bit BOOL */
}

static LONG call_ex(struct PnBase *b, struct PnReq *r, ULONG extra, ULONG *got, BOOL *broke)
{
    struct MsgPort *port = b->port;
    BOOL temp = FALSE;
    ULONG mask, sigs = 0;
    ULONG brk = b->breakmask & ~extra;     /* caller's own signals win */

    if (got)
        *got = 0;
    if (broke)
        *broke = FALSE;
    if (FindTask(NULL) != b->owner) {
        /* Base shared with another task: use a port of the calling task. */
        port = CreateMsgPort();
        if (!port) {
            set_errno(b, PN_ENOMEM);
            return -1;
        }
        temp = TRUE;
    }
    if (!port) {                /* the core's own base: it is not a client */
        set_errno(b, PN_ENETDOWN);
        return -1;
    }
    r->msg.mn_ReplyPort = port;
    r->msg.mn_Length    = sizeof(*r);
    r->client = b->client;
    r->result = -1;
    r->error  = 0;
    r->herror = 0;

    if (!temp && b->client && PN_DIRECT_OP(r->op)) {
        int d = direct_call(b, r);
        if (d == 1)
            goto out;                   /* answered in this task */
        if (d == 0)
            goto wait;                  /* pending: the reply comes by message */
    }
    if (!send_to_core(b, r)) {
        r->error = PN_ENETDOWN;
        goto out;
    }

wait:
    mask = 1UL << port->mp_SigBit;
    for (;;) {
        BOOL rdone = FALSE;

        reap(port, r, &rdone, NULL, NULL);
        if (rdone)
            break;
        sigs = Wait(mask | brk | extra);
        if (sigs & (brk | extra)) {
            struct PnReq ab;
            BOOL adone = FALSE;

            reap(port, r, &rdone, NULL, NULL);
            if (!rdone) {
                memset(&ab, 0, sizeof(ab));
                ab.msg.mn_ReplyPort = port;
                ab.msg.mn_Length    = sizeof(ab);
                ab.op     = PNOP_ABORT;
                ab.target = r;
                ab.client = b->client;
                if (!send_to_core(b, &ab))
                    adone = TRUE;
                for (;;) {
                    reap(port, r, &rdone, &ab, &adone);
                    if (rdone && adone)
                        break;
                    Wait(mask);
                }
            }
            if (got)
                *got = sigs & extra;
            /* Leave break signals set so the program's own checks see them. */
            if (sigs & brk) {
                SetSignal(sigs & brk, sigs & brk);
                if (broke)
                    *broke = TRUE;
            }
            break;
        }
    }

out:
    if (temp)
        DeleteMsgPort(port);
    if (r->result < 0 && r->error)
        set_errno(b, r->error);
    return r->result;
}

static LONG call(struct PnBase *b, struct PnReq *r)
{
    return call_ex(b, r, 0, NULL, NULL);
}

#define REQ(op_) struct PnReq r; memset(&r, 0, sizeof(r)); r.op = (op_)

/* ---------------------------------------------------------------------- */
/* auto-start: OpenLibrary() with no core running starts one               */
/* ---------------------------------------------------------------------- */

static BOOL core_running(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((CONST_STRPTR)PN_PORTNAME);
    Permit();
    return p != NULL;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Find KEY=value (case-insensitive key) in a small text; copies value. */
static BOOL find_key(const char *text, const char *key, char *val, int size)
{
    const char *p;
    for (p = text; *p; p++) {
        const char *k = key, *q = p;
        if (p != text && p[-1] != '\n' && p[-1] != ' ' && p[-1] != '\t')
            continue;
        while (*k && lc(*q) == lc(*k)) { q++; k++; }
        if (*k == 0 && *q == '=') {
            int n = 0;
            q++;
            while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && n < size - 1)
                val[n++] = *q++;
            val[n] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL autostart_core(struct PnBase *b)
{
    struct Task *me = FindTask(NULL);
    char text[512], cmd[160], v[16];
    BPTR lock, fh, in, out;
    LONG n;
    int i;

    if (!DOSBase || me->tc_Node.ln_Type != NT_PROCESS)
        return FALSE;                   /* plain tasks can't call DOS */
    lock = Lock((CONST_STRPTR)"ENV:PrismNet/interfaces", ACCESS_READ);
    if (!lock)
        return FALSE;                   /* not configured */
    UnLock(lock);

    strcpy(cmd, "C:PrismNet");
    if ((fh = Open((CONST_STRPTR)"ENV:PrismNet/startup", MODE_OLDFILE)) != 0) {
        n = Read(fh, text, sizeof(text) - 1);
        Close(fh);
        text[n > 0 ? n : 0] = 0;
        if (find_key(text, "AUTOSTART", v, sizeof(v)) &&
            (lc(v[0]) == 'n' || (lc(v[0]) == 'o' && lc(v[1]) == 'f') || v[0] == '0'))
            return FALSE;
        find_key(text, "COMMAND", cmd, sizeof(cmd) - 16);
    }
    strcat(cmd, " RUN QUIET");

    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out ||                  /* SYS_Asynch needs both handles */
        SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_Name, (ULONG)"PrismNet", TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        return FALSE;
    }
    for (i = 0; i < 100 && !core_running(); i++)
        Delay(5);                       /* up to 10 s for the core's port */
    (void)b;
    return core_running();
}

/* After an auto-start, give DHCP a moment so the first connect() works. */
static void wait_for_address(struct PnBase *b)
{
    struct PnStatus *st = AllocVec(sizeof(*st), MEMF_ANY | MEMF_CLEAR);
    int i;

    if (!st)
        return;
    for (i = 0; i < 50; i++) {          /* up to 10 s */
        REQ(PNOP_STATUS);
        r.a[0] = (ULONG)st;
        if (call(b, &r) < 0 || st->ip)
            break;
        Delay(10);
    }
    FreeVec(st);
}

/* ---------------------------------------------------------------------- */
/* library open / close / expunge                                           */
/* ---------------------------------------------------------------------- */

static void free_base(struct PnBase *nb)
{
    if (nb->dstack)
        FreeMem(nb->dstack, DSTACK);
    FreeMem((UBYTE *)nb - nb->lib.lib_NegSize, nb->lib.lib_NegSize + nb->lib.lib_PosSize);
}

static BPTR pn_expunge(struct PnBase *lib REG(a6))
{
    BPTR seg;

    if (lib->master)            /* never expunge a per-opener base */
        return 0;
    if (lib->lib.lib_OpenCnt) {
        lib->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seg = lib->seglist;
    Remove(&lib->lib.lib_Node);
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
    free_base(lib);
    return seg;
}

static struct PnBase *pn_open(struct PnBase *lib REG(a6))
{
    struct PnBase *master = lib->master ? lib->master : lib;
    ULONG neg = master->lib.lib_NegSize, size = neg + master->lib.lib_PosSize;
    struct PnBase *nb;
    UBYTE *mem;

    /* Count the open from the start: auto-start and HELLO block for up to
     * 10 s each (Delay/Wait drop OpenLibrary's Forbid), and a master with
     * lib_OpenCnt 0 can be expunged by exec's low-memory handler meanwhile. */
    master->lib.lib_OpenCnt++;
    master->lib.lib_Flags &= ~LIBF_DELEXP;

    mem = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!mem) {
        master->lib.lib_OpenCnt--;
        return NULL;
    }
    CopyMem((UBYTE *)master - neg, mem, neg + sizeof(struct Library));
    CacheClearU();              /* the copied jump table is code */
    nb = (struct PnBase *)(mem + neg);
    nb->lib.lib_OpenCnt = 1;
    nb->master    = master;
    nb->owner     = FindTask(NULL);
    nb->breakmask = SIGBREAKF_CTRL_C;
    nb->noaliases[0] = NULL;

    /* The core opens the library itself to keep it resident: no client. */
    {
        struct MsgPort *cp;
        Forbid();
        cp = FindPort((CONST_STRPTR)PN_PORTNAME);
        Permit();
        if (cp && cp->mp_SigTask == (APTR)nb->owner) {
            nb->is_core = TRUE;
            master->coreport = cp;
            return nb;
        }
    }

    nb->port = CreateMsgPort();
    if (nb->port) {
        BOOL started = FALSE;

        if (!core_running())
            started = autostart_core(nb);
        {
            LONG h;
            REQ(PNOP_HELLO);
            r.a[0] = (ULONG)nb->owner;
            h = call(nb, &r);
            /* call() returns -1 when the core is absent: not a handle */
            nb->client = (r.error == 0 && h != 0 && h != -1) ? (APTR)h : NULL;
        }
        if (nb->client && started)
            wait_for_address(nb);
        if (nb->client)
            return nb;
        DeleteMsgPort(nb->port);
    }
    free_base(nb);
    master->lib.lib_OpenCnt--;
    return NULL;                /* core not running: like AmiTCP, no library */
}

static BPTR pn_close(struct PnBase *b REG(a6))
{
    struct PnBase *master = b->master;

    if (!master)
        return 0;
    if (b->is_core) {
        Forbid();
        master->coreport = NULL;
        Permit();
    }
    if (b->client) {
        REQ(PNOP_BYE);
        call(b, &r);
    }
    if (b->port) {
        /* DeleteMsgPort frees the signal bit in the *calling* task; from
         * another task that would hand out one of its live bits again. */
        if (FindTask(NULL) == b->owner)
            DeleteMsgPort(b->port);
        else
            FreeMem(b->port, sizeof(struct MsgPort));
    }
    free_base(b);
    if (--master->lib.lib_OpenCnt == 0 && (master->lib.lib_Flags & LIBF_DELEXP))
        return pn_expunge(master);
    return 0;
}

static ULONG pn_null(void)
{
    return 0;
}

/* ---------------------------------------------------------------------- */
/* sockets                                                                  */
/* ---------------------------------------------------------------------- */

static LONG pn_socket(LONG domain REG(d0), LONG type REG(d1), LONG proto REG(d2),
                      struct PnBase *b REG(a6))
{
    REQ(PNOP_SOCKET);
    r.a[0] = domain; r.a[1] = type; r.a[2] = proto;
    return call(b, &r);
}

static LONG pn_bind(LONG s REG(d0), APTR name REG(a0), LONG len REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_BIND);
    r.fd = s; r.a[0] = (ULONG)name; r.a[1] = len;
    return call(b, &r);
}

static LONG pn_listen(LONG s REG(d0), LONG backlog REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_LISTEN);
    r.fd = s; r.a[0] = backlog;
    return call(b, &r);
}

static LONG pn_accept(LONG s REG(d0), APTR addr REG(a0), LONG *len REG(a1), struct PnBase *b REG(a6))
{
    REQ(PNOP_ACCEPT);
    r.fd = s; r.a[0] = (ULONG)addr; r.a[1] = (ULONG)len;
    return call(b, &r);
}

static LONG pn_connect(LONG s REG(d0), APTR name REG(a0), LONG len REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_CONNECT);
    r.fd = s; r.a[0] = (ULONG)name; r.a[1] = len;
    return call(b, &r);
}

static LONG pn_sendto(LONG s REG(d0), APTR buf REG(a0), LONG len REG(d1), LONG flags REG(d2),
                      APTR to REG(a1), LONG tolen REG(d3), struct PnBase *b REG(a6))
{
    REQ(PNOP_SEND);
    r.fd = s; r.a[0] = (ULONG)buf; r.a[1] = len; r.a[2] = flags;
    r.a[3] = (ULONG)to; r.a[4] = tolen;
    return call(b, &r);
}

static LONG pn_send(LONG s REG(d0), APTR buf REG(a0), LONG len REG(d1), LONG flags REG(d2),
                    struct PnBase *b REG(a6))
{
    REQ(PNOP_SEND);
    r.fd = s; r.a[0] = (ULONG)buf; r.a[1] = len; r.a[2] = flags;
    return call(b, &r);
}

static LONG pn_recvfrom(LONG s REG(d0), APTR buf REG(a0), LONG len REG(d1), LONG flags REG(d2),
                        APTR from REG(a1), LONG *fromlen REG(a2), struct PnBase *b REG(a6))
{
    REQ(PNOP_RECV);
    r.fd = s; r.a[0] = (ULONG)buf; r.a[1] = len; r.a[2] = flags;
    r.a[3] = (ULONG)from; r.a[4] = (ULONG)fromlen;
    return call(b, &r);
}

static LONG pn_recv(LONG s REG(d0), APTR buf REG(a0), LONG len REG(d1), LONG flags REG(d2),
                    struct PnBase *b REG(a6))
{
    REQ(PNOP_RECV);
    r.fd = s; r.a[0] = (ULONG)buf; r.a[1] = len; r.a[2] = flags;
    return call(b, &r);
}

static LONG pn_shutdown(LONG s REG(d0), LONG how REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_SHUTDOWN);
    r.fd = s; r.a[0] = how;
    return call(b, &r);
}

static LONG pn_setsockopt(LONG s REG(d0), LONG level REG(d1), LONG name REG(d2),
                          APTR val REG(a0), LONG len REG(d3), struct PnBase *b REG(a6))
{
    REQ(PNOP_SETSOCKOPT);
    r.fd = s; r.a[0] = level; r.a[1] = name; r.a[2] = (ULONG)val; r.a[3] = len;
    return call(b, &r);
}

static LONG pn_getsockopt(LONG s REG(d0), LONG level REG(d1), LONG name REG(d2),
                          APTR val REG(a0), LONG *len REG(a1), struct PnBase *b REG(a6))
{
    REQ(PNOP_GETSOCKOPT);
    r.fd = s; r.a[0] = level; r.a[1] = name; r.a[2] = (ULONG)val; r.a[3] = (ULONG)len;
    return call(b, &r);
}

static LONG pn_getsockname(LONG s REG(d0), APTR name REG(a0), LONG *len REG(a1), struct PnBase *b REG(a6))
{
    REQ(PNOP_GETSOCKNAME);
    r.fd = s; r.a[0] = (ULONG)name; r.a[1] = (ULONG)len;
    return call(b, &r);
}

static LONG pn_getpeername(LONG s REG(d0), APTR name REG(a0), LONG *len REG(a1), struct PnBase *b REG(a6))
{
    REQ(PNOP_GETPEERNAME);
    r.fd = s; r.a[0] = (ULONG)name; r.a[1] = (ULONG)len;
    return call(b, &r);
}

static LONG pn_ioctlsocket(LONG s REG(d0), ULONG req REG(d1), APTR argp REG(a0), struct PnBase *b REG(a6))
{
    REQ(PNOP_IOCTL);
    r.fd = s; r.a[0] = req; r.a[1] = (ULONG)argp;
    return call(b, &r);
}

static LONG pn_closesocket(LONG s REG(d0), struct PnBase *b REG(a6))
{
    REQ(PNOP_CLOSE);
    r.fd = s;
    return call(b, &r);
}

static LONG pn_waitselect(LONG nfds REG(d0), APTR rd REG(a0), APTR wr REG(a1), APTR ex REG(a2),
                          APTR tv REG(a3), ULONG *sigs REG(d1), struct PnBase *b REG(a6))
{
    ULONG extra = sigs ? *sigs : 0, got = 0;
    BOOL broke;
    LONG res;
    REQ(PNOP_SELECT);

    r.a[0] = nfds; r.a[1] = (ULONG)rd; r.a[2] = (ULONG)wr;
    r.a[3] = (ULONG)ex; r.a[4] = (ULONG)tv;
    res = call_ex(b, &r, extra, &got, &broke);
    if (sigs) {
        *sigs = got;
        if (got)
            SetSignal(0, got);      /* consumed, reported via *sigs */
    }
    if (broke) {
        set_errno(b, PN_EINTR);
        return -1;
    }
    return res;
}

static void pn_setsocketsignals(ULONG intr REG(d0), ULONG io REG(d1), ULONG urg REG(d2),
                                struct PnBase *b REG(a6))
{
    b->breakmask  = intr;
    b->sigiomask  = io;
    b->sigurgmask = urg;
}

static LONG pn_getdtablesize(struct PnBase *b REG(a6))
{
    REQ(PNOP_DTABLESIZE);
    return call(b, &r);
}

static LONG pn_obtainsocket(LONG id REG(d0), LONG domain REG(d1), LONG type REG(d2),
                            LONG proto REG(d3), struct PnBase *b REG(a6))
{
    REQ(PNOP_OBTAIN);
    r.a[0] = id; r.a[1] = domain; r.a[2] = type; r.a[3] = proto;
    return call(b, &r);
}

static LONG pn_releasesocket(LONG fd REG(d0), LONG id REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_RELEASE);
    r.fd = fd; r.a[0] = id; r.a[1] = 0;
    return call(b, &r);
}

static LONG pn_releasecopyofsocket(LONG fd REG(d0), LONG id REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_RELEASE);
    r.fd = fd; r.a[0] = id; r.a[1] = 1;
    return call(b, &r);
}

static LONG pn_errno(struct PnBase *b REG(a6))
{
    return b->err;
}

static void pn_seterrnoptr(APTR ptr REG(a0), LONG size REG(d0), struct PnBase *b REG(a6))
{
    if (size == 1 || size == 2 || size == 4) {
        b->errnoptr  = ptr;
        b->errnosize = size;
    }
}

static LONG pn_dup2socket(LONG fd1 REG(d0), LONG fd2 REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_DUP2);
    r.fd = fd1; r.a[0] = fd2;
    return call(b, &r);
}

struct pn_iovec  { APTR iov_base; ULONG iov_len; };
struct pn_msghdr {
    APTR             msg_name;
    ULONG            msg_namelen;
    struct pn_iovec *msg_iov;
    ULONG            msg_iovlen;
    APTR             msg_control;
    ULONG            msg_controllen;
    LONG             msg_flags;
};

/* sendmsg/recvmsg: gather/scatter in the library, one SEND/RECV to the core.
 * No ancillary data is supported (msg_controllen comes back 0). */
static LONG pn_sendmsg(LONG s REG(d0), struct pn_msghdr *m REG(a0), LONG flags REG(d1),
                       struct PnBase *b REG(a6))
{
    ULONG i, total = 0;
    UBYTE *buf = NULL, *p;
    LONG res;
    REQ(PNOP_SEND);

    if (!m || (m->msg_iovlen && !m->msg_iov)) {
        set_errno(b, PN_EFAULT);
        return -1;
    }
    for (i = 0; i < m->msg_iovlen; i++)
        total += m->msg_iov[i].iov_len;
    if (m->msg_iovlen == 1) {
        buf = m->msg_iov[0].iov_base;
    } else if (total) {
        buf = AllocVec(total, MEMF_ANY);
        if (!buf) {
            set_errno(b, PN_ENOBUFS);
            return -1;
        }
        for (i = 0, p = buf; i < m->msg_iovlen; p += m->msg_iov[i].iov_len, i++)
            CopyMem(m->msg_iov[i].iov_base, p, m->msg_iov[i].iov_len);
    }
    r.fd = s; r.a[0] = (ULONG)buf; r.a[1] = total; r.a[2] = flags;
    r.a[3] = (ULONG)m->msg_name; r.a[4] = m->msg_name ? m->msg_namelen : 0;
    res = call(b, &r);
    if (m->msg_iovlen > 1 && buf)
        FreeVec(buf);
    return res;
}

static LONG pn_recvmsg(LONG s REG(d0), struct pn_msghdr *m REG(a0), LONG flags REG(d1),
                       struct PnBase *b REG(a6))
{
    ULONG i, total = 0;
    UBYTE *buf = NULL, *p;
    LONG res, left;
    REQ(PNOP_RECV);

    if (!m || (m->msg_iovlen && !m->msg_iov)) {
        set_errno(b, PN_EFAULT);
        return -1;
    }
    for (i = 0; i < m->msg_iovlen; i++)
        total += m->msg_iov[i].iov_len;
    if (m->msg_iovlen == 1) {
        buf = m->msg_iov[0].iov_base;
    } else if (total) {
        buf = AllocVec(total, MEMF_ANY);
        if (!buf) {
            set_errno(b, PN_ENOBUFS);
            return -1;
        }
    }
    r.fd = s; r.a[0] = (ULONG)buf; r.a[1] = total; r.a[2] = flags;
    r.a[3] = (ULONG)m->msg_name; r.a[4] = m->msg_name ? (ULONG)&m->msg_namelen : 0;
    res = call(b, &r);
    if (m->msg_iovlen > 1 && buf) {
        for (i = 0, p = buf, left = res; left > 0 && i < m->msg_iovlen; i++) {
            ULONG n = m->msg_iov[i].iov_len < (ULONG)left ? m->msg_iov[i].iov_len : (ULONG)left;
            CopyMem(p, m->msg_iov[i].iov_base, n);
            p += n;
            left -= n;
        }
        FreeVec(buf);
    }
    m->msg_controllen = 0;
    m->msg_flags = 0;
    return res;
}

/* Next socket with pending events (see SO_EVENTMASK); -1 if none. */
static LONG pn_getsocketevents(ULONG *mask REG(a0), struct PnBase *b REG(a6))
{
    ULONG events = 0;
    LONG fd;
    REQ(PNOP_GETEVENTS);

    r.a[0] = (ULONG)&events;
    fd = call(b, &r);
    if (mask)
        *mask = fd >= 0 ? events : 0;
    return fd;
}
static void pn_vsyslog(LONG level REG(d0), STRPTR fmt REG(a0), APTR args REG(a1), struct PnBase *b REG(a6))
{
    (void)level; (void)fmt; (void)args; (void)b;     /* not forwarded to the core log */
}

/* ---------------------------------------------------------------------- */
/* names                                                                    */
/* ---------------------------------------------------------------------- */

static BOOL aton(const char *cp, ULONG *out);

/* A numeric host ("10.0.2.2") is answered here: no round trip to the core,
 * whose hosts-file lookup alone costs milliseconds. */
static APTR numeric_host(struct PnBase *b, const char *name)
{
    struct PnHostBuf *hb = &b->hostbuf;
    ULONG a;
    int i;

    if (!aton(name, &a))
        return NULL;
    for (i = 0; name[i] && i < (int)sizeof(hb->name) - 1; i++)
        hb->name[i] = name[i];
    hb->name[i]      = 0;
    hb->h_name       = hb->name;
    hb->alias_ptrs[0] = NULL;
    hb->h_aliases    = hb->alias_ptrs;
    hb->h_addrtype   = 2;
    hb->h_length     = 4;
    hb->addrs[0]     = a;
    hb->addr_ptrs[0] = (char *)&hb->addrs[0];
    hb->addr_ptrs[1] = NULL;
    hb->h_addr_list  = hb->addr_ptrs;
    return hb;
}

static APTR pn_gethostbyname(STRPTR name REG(a0), struct PnBase *b REG(a6))
{
    APTR h;
    REQ(PNOP_GETHOSTBYNAME);
    if (name && (h = numeric_host(b, (const char *)name)) != NULL)
        return h;
    r.a[0] = (ULONG)name; r.a[1] = (ULONG)&b->hostbuf;
    if (call(b, &r) < 0) {
        set_herrno(b, r.herror ? r.herror : PN_TRY_AGAIN);
        return NULL;
    }
    return &b->hostbuf;
}

static APTR pn_gethostbyaddr(APTR addr REG(a0), LONG len REG(d0), LONG type REG(d1), struct PnBase *b REG(a6))
{
    REQ(PNOP_GETHOSTBYADDR);
    if (!addr || len != 4 || type != 2) {
        set_herrno(b, PN_HOST_NOT_FOUND);
        return NULL;
    }
    CopyMem((APTR)addr, &r.a[0], 4);        /* addr may be odd: 68000 */
    r.a[1] = (ULONG)&b->hostbuf;
    if (call(b, &r) < 0) {
        set_herrno(b, r.herror ? r.herror : PN_HOST_NOT_FOUND);
        return NULL;
    }
    return &b->hostbuf;
}

static LONG pn_gethostname(STRPTR buf REG(a0), LONG size REG(d0), struct PnBase *b REG(a6))
{
    REQ(PNOP_GETHOSTNAME);
    r.a[0] = (ULONG)buf; r.a[1] = size;
    return call(b, &r);
}

static ULONG pn_gethostid(struct PnBase *b REG(a6))
{
    /* PnStatus is large: keep it off the caller's (possibly 4 KB) stack. */
    struct PnStatus *st = AllocVec(sizeof(*st), MEMF_ANY | MEMF_CLEAR);
    ULONG id = 0;
    REQ(PNOP_STATUS);

    if (!st)
        return 0;
    r.a[0] = (ULONG)st;
    if (call(b, &r) >= 0)
        id = st->ip;
    FreeVec(st);
    return id;
}

static APTR pn_getnetbyname(STRPTR n REG(a0), struct PnBase *b REG(a6))
{
    (void)n; (void)b;
    return NULL;
}

static APTR pn_getnetbyaddr(ULONG net REG(d0), LONG type REG(d1), struct PnBase *b REG(a6))
{
    (void)net; (void)type; (void)b;
    return NULL;
}

static const struct { const char *name; UWORD port; UBYTE udp; } services[] = {
    { "ftp-data", 20, 0 }, { "ftp", 21, 0 }, { "ssh", 22, 0 }, { "telnet", 23, 0 },
    { "smtp", 25, 0 }, { "time", 37, 0 }, { "time", 37, 1 }, { "domain", 53, 0 },
    { "domain", 53, 1 }, { "tftp", 69, 1 }, { "gopher", 70, 0 }, { "finger", 79, 0 },
    { "http", 80, 0 }, { "www", 80, 0 }, { "pop3", 110, 0 }, { "nntp", 119, 0 },
    { "ntp", 123, 1 }, { "imap", 143, 0 }, { "snmp", 161, 1 }, { "irc", 194, 0 },
    { "https", 443, 0 }, { "microsoft-ds", 445, 0 }, { "submission", 587, 0 },
    { "imaps", 993, 0 }, { "pop3s", 995, 0 },
};
#define NSERVICES (sizeof(services) / sizeof(services[0]))

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static APTR fill_serv(struct PnBase *b, int i)
{
    b->servent.s_name    = (char *)services[i].name;
    b->servent.s_aliases = b->noaliases;
    b->servent.s_port    = services[i].port;    /* network order == host order on m68k */
    b->servent.s_proto   = services[i].udp ? (char *)"udp" : (char *)"tcp";
    return &b->servent;
}

static APTR pn_getservbyname(STRPTR name REG(a0), STRPTR proto REG(a1), struct PnBase *b REG(a6))
{
    unsigned i;
    if (!name)
        return NULL;
    for (i = 0; i < NSERVICES; i++)
        if (str_ieq(name, services[i].name) &&
            (!proto || str_ieq(proto, services[i].udp ? "udp" : "tcp")))
            return fill_serv(b, i);
    return NULL;
}

static APTR pn_getservbyport(LONG port REG(d0), STRPTR proto REG(a0), struct PnBase *b REG(a6))
{
    unsigned i;
    for (i = 0; i < NSERVICES; i++)
        if (services[i].port == (UWORD)port &&
            (!proto || str_ieq(proto, services[i].udp ? "udp" : "tcp")))
            return fill_serv(b, i);
    return NULL;
}

static const struct { const char *name; LONG num; } protocols[] = {
    { "ip", 0 }, { "icmp", 1 }, { "tcp", 6 }, { "udp", 17 },
};

static APTR pn_getprotobyname(STRPTR name REG(a0), struct PnBase *b REG(a6))
{
    unsigned i;
    for (i = 0; name && i < 4; i++)
        if (str_ieq(name, protocols[i].name)) {
            b->protoent.p_name    = (char *)protocols[i].name;
            b->protoent.p_aliases = b->noaliases;
            b->protoent.p_proto   = protocols[i].num;
            return &b->protoent;
        }
    return NULL;
}

static APTR pn_getprotobynumber(LONG num REG(d0), struct PnBase *b REG(a6))
{
    unsigned i;
    for (i = 0; i < 4; i++)
        if (protocols[i].num == num)
            return pn_getprotobyname((STRPTR)protocols[i].name, b);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* inet_* helpers (pure, no core call)                                      */
/* ---------------------------------------------------------------------- */

static char *put_dec(char *p, ULONG v)
{
    char tmp[4];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static void ntoa(ULONG a, char *out)
{
    char *p = out;
    p = put_dec(p, (a >> 24) & 255); *p++ = '.';
    p = put_dec(p, (a >> 16) & 255); *p++ = '.';
    p = put_dec(p, (a >> 8) & 255);  *p++ = '.';
    p = put_dec(p, a & 255);         *p = 0;
}

static STRPTR pn_inet_ntoa(ULONG in REG(d0), struct PnBase *b REG(a6))
{
    ntoa(in, b->ntoa);
    return (STRPTR)b->ntoa;
}

/* BSD inet_aton(): a, a.b, a.b.c, a.b.c.d; decimal, 0octal, 0xhex. */
static BOOL aton(const char *cp, ULONG *out)
{
    ULONG parts[4], val;
    int n = 0;

    for (;;) {
        ULONG base = 10;
        int digits = 0;
        val = 0;
        if (*cp == '0') {
            base = 8; cp++; digits = 1;
            if (*cp == 'x' || *cp == 'X') { base = 16; cp++; digits = 0; }
        }
        for (;;) {
            char c = *cp;
            ULONG d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (d >= base) return FALSE;
            val = val * base + d;
            digits++;
            cp++;
        }
        if (!digits) return FALSE;
        if (*cp == '.') {
            if (n >= 3 || val > 255) return FALSE;
            parts[n++] = val;
            cp++;
            continue;
        }
        break;
    }
    if (*cp && *cp != ' ' && *cp != '\t' && *cp != '\n')
        return FALSE;
    switch (n) {
    case 0: break;
    case 1: if (val > 0xffffff) return FALSE; val |= parts[0] << 24; break;
    case 2: if (val > 0xffff) return FALSE; val |= (parts[0] << 24) | (parts[1] << 16); break;
    case 3: if (val > 0xff) return FALSE;
            val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8); break;
    }
    *out = val;
    return TRUE;
}

static ULONG pn_inet_addr(STRPTR cp REG(a0), struct PnBase *b REG(a6))
{
    ULONG a;
    (void)b;
    return (cp && aton((const char *)cp, &a)) ? a : 0xffffffffUL;
}

static LONG pn_inet_aton(STRPTR cp REG(a0), ULONG *addr REG(a1), struct PnBase *b REG(a6))
{
    ULONG a;
    (void)b;
    if (!cp || !aton((const char *)cp, &a))
        return 0;
    if (addr) *addr = a;
    return 1;
}

static STRPTR pn_inet_ntop(LONG af REG(d0), APTR src REG(a0), STRPTR dst REG(a1), LONG size REG(d1),
                           struct PnBase *b REG(a6))
{
    char tmp[16];
    ULONG a;
    if (af != 2 || !src || !dst) { set_errno(b, PN_EAFNOSUPPORT); return NULL; }
    CopyMem((APTR)src, &a, 4);              /* src may be odd: 68000 */
    ntoa(a, tmp);
    if ((LONG)strlen(tmp) + 1 > size) { set_errno(b, PN_ENOBUFS); return NULL; }
    strcpy((char *)dst, tmp);
    return dst;
}

static LONG pn_inet_pton(LONG af REG(d0), STRPTR src REG(a0), APTR dst REG(a1), struct PnBase *b REG(a6))
{
    ULONG a, parts = 0;
    const char *p;
    if (af != 2) { set_errno(b, PN_EAFNOSUPPORT); return -1; }
    for (p = (const char *)src; p && *p; p++) {
        if (*p == '.') parts++;
        else if (*p < '0' || *p > '9') return 0;
    }
    if (!src || parts != 3 || !aton((const char *)src, &a))
        return 0;
    CopyMem(&a, (APTR)dst, 4);              /* dst may be odd: 68000 */
    return 1;
}

static ULONG pn_inet_lnaof(ULONG in REG(d0), struct PnBase *b REG(a6))
{
    (void)b;
    if ((in & 0x80000000UL) == 0) return in & 0x00ffffffUL;
    if ((in & 0xc0000000UL) == 0x80000000UL) return in & 0x0000ffffUL;
    return in & 0x000000ffUL;
}

static ULONG pn_inet_netof(ULONG in REG(d0), struct PnBase *b REG(a6))
{
    (void)b;
    if ((in & 0x80000000UL) == 0) return in >> 24;
    if ((in & 0xc0000000UL) == 0x80000000UL) return in >> 16;
    return in >> 8;
}

static ULONG pn_inet_makeaddr(ULONG net REG(d0), ULONG host REG(d1), struct PnBase *b REG(a6))
{
    (void)b;
    if (net < 128)   return (net << 24) | (host & 0x00ffffffUL);
    if (net < 65536) return (net << 16) | (host & 0x0000ffffUL);
    return (net << 8) | (host & 0x000000ffUL);
}

static ULONG pn_inet_network(STRPTR cp REG(a0), struct PnBase *b REG(a6))
{
    ULONG a, v = 0;
    int parts = 1;
    const char *p;
    (void)b;
    if (!cp || !aton((const char *)cp, &a))
        return 0xffffffffUL;
    for (p = (const char *)cp; *p; p++)
        if (*p == '.') parts++;
    /* inet_network() returns the parts right-aligned (no class padding) */
    switch (parts) {
    case 1: v = a; break;
    case 2: v = ((a >> 24) << 8) | (a & 0xff); break;
    case 3: v = ((a >> 24) << 16) | (((a >> 16) & 0xff) << 8) | (a & 0xff); break;
    default: v = a; break;
    }
    return v;
}

/* ---------------------------------------------------------------------- */
/* SocketBaseTagList                                                        */
/* ---------------------------------------------------------------------- */

static const char *const errstr[] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted system call", "Input/output error", "Device not configured",
    "Argument list too long", "Exec format error", "Bad file descriptor", "No child processes",
    "Resource deadlock avoided", "Cannot allocate memory", "Permission denied", "Bad address",
    "Block device required", "Device busy", "File exists", "Cross-device link",
    "Operation not supported by device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate ioctl for device",
    "Text file busy", "File too large", "No space left on device", "Illegal seek",
    "Read-only file system", "Too many links", "Broken pipe", "Numerical argument out of domain",
    "Result too large", "Operation would block", "Operation now in progress",
    "Operation already in progress", "Socket operation on non-socket",
    "Destination address required", "Message too long", "Protocol wrong type for socket",
    "Protocol not available", "Protocol not supported", "Socket type not supported",
    "Operation not supported", "Protocol family not supported",
    "Address family not supported by protocol family", "Address already in use",
    "Can't assign requested address", "Network is down", "Network is unreachable",
    "Network dropped connection on reset", "Software caused connection abort",
    "Connection reset by peer", "No buffer space available", "Socket is already connected",
    "Socket is not connected", "Can't send after socket shutdown",
    "Too many references: can't splice", "Operation timed out", "Connection refused",
    "Too many levels of symbolic links", "File name too long", "Host is down",
    "No route to host",
};
static const char *const herrstr[] = {
    "Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure",
    "Unknown server error", "No address associated with name",
};

#define SBTC_BREAKMASK_      1
#define SBTC_SIGIOMASK_      2
#define SBTC_SIGURGMASK_     3
#define SBTC_SIGEVENTMASK_   4
#define SBTC_ERRNO_          6
#define SBTC_HERRNO_         7
#define SBTC_DTABLESIZE_     8
#define SBTC_FDCALLBACK_     9
#define SBTC_LOGSTAT_        10
#define SBTC_LOGTAGPTR_      11
#define SBTC_LOGFACILITY_    12
#define SBTC_LOGMASK_        13
#define SBTC_ERRNOSTRPTR_    14
#define SBTC_HERRNOSTRPTR_   15
#define SBTC_IOERRNOSTRPTR_  16
#define SBTC_S2ERRNOSTRPTR_  17
#define SBTC_S2WERRNOSTRPTR_ 18
#define SBTC_ERRNOBYTEPTR_   21
#define SBTC_ERRNOWORDPTR_   22
#define SBTC_ERRNOLONGPTR_   24
#define SBTC_HERRNOLONGPTR_  25
#define SBTC_RELEASESTRPTR_  29

static BOOL tag_code(struct PnBase *b, ULONG code, BOOL set, ULONG *where)
{
    ULONG v = *where;

    switch (code) {
    case SBTC_BREAKMASK_:    if (set) b->breakmask = v;    else *where = b->breakmask;    return TRUE;
    case SBTC_SIGIOMASK_:    if (set) b->sigiomask = v;    else *where = b->sigiomask;    return TRUE;
    case SBTC_SIGURGMASK_:   if (set) b->sigurgmask = v;   else *where = b->sigurgmask;   return TRUE;
    case SBTC_SIGEVENTMASK_:
        if (set) {
            REQ(PNOP_SETEVENTSIG);              /* the core signals this task */
            b->sigeventmask = v;
            r.a[0] = v;
            call(b, &r);
        } else {
            *where = b->sigeventmask;
        }
        return TRUE;
    case SBTC_ERRNO_:        if (set) set_errno(b, v);     else *where = b->err;          return TRUE;
    case SBTC_HERRNO_:       if (set) set_herrno(b, v);    else *where = b->herr;         return TRUE;
    case SBTC_DTABLESIZE_: {
        REQ(PNOP_DTABLESIZE);
        r.a[0] = set ? v : 0;
        v = call(b, &r);
        if (!set) *where = v;
        return (LONG)v > 0;
    }
    case SBTC_FDCALLBACK_:   return set;     /* accepted, unused */
    case SBTC_LOGSTAT_:      if (set) b->logstat = v;      else *where = b->logstat;      return TRUE;
    case SBTC_LOGTAGPTR_:    if (set) b->logtag = (STRPTR)v; else *where = (ULONG)b->logtag; return TRUE;
    case SBTC_LOGFACILITY_:  if (set) b->logfacility = v;  else *where = b->logfacility;  return TRUE;
    case SBTC_LOGMASK_:      if (set) b->logmask = v;      else *where = b->logmask;      return TRUE;
    case SBTC_ERRNOSTRPTR_:
        if (set) return FALSE;
        *where = (ULONG)(v < sizeof(errstr) / sizeof(errstr[0]) ? errstr[v] : "Unknown error");
        return TRUE;
    case SBTC_HERRNOSTRPTR_:
        if (set) return FALSE;
        *where = (ULONG)(v < sizeof(herrstr) / sizeof(herrstr[0]) ? herrstr[v] : "Unknown resolver error");
        return TRUE;
    case SBTC_IOERRNOSTRPTR_:
    case SBTC_S2ERRNOSTRPTR_:
    case SBTC_S2WERRNOSTRPTR_:
        if (set) return FALSE;
        *where = (ULONG)"Unknown error";
        return TRUE;
    case SBTC_ERRNOBYTEPTR_: if (!set) return FALSE; b->errnoptr = (APTR)v; b->errnosize = 1; return TRUE;
    case SBTC_ERRNOWORDPTR_: if (!set) return FALSE; b->errnoptr = (APTR)v; b->errnosize = 2; return TRUE;
    case SBTC_ERRNOLONGPTR_:                /* reading gives back the pointer in use */
        if (set) { b->errnoptr = (APTR)v; b->errnosize = 4; } else *where = (ULONG)b->errnoptr;
        return TRUE;
    case SBTC_HERRNOLONGPTR_:
        if (set) b->herrnoptr = (LONG *)v; else *where = (ULONG)b->herrnoptr;
        return TRUE;
    case SBTC_RELEASESTRPTR_:               /* programs tell stacks apart by this */
        if (set) return FALSE;
        *where = (ULONG)"PrismNet bsdsocket.library 4.3 (lwIP 2.2.1)";
        return TRUE;
    case 41: case 47: case 53: case 54: /* HAVE_ROUTING_API, _INTERFACE_API, _STATUS_API, _DNS_API */
        if (set) return FALSE;
        *where = TRUE;
        return TRUE;
    case 50: case 59: case 60: case 61: case 63: case 67: case 69:
        if (set) return FALSE;          /* routing, monitoring, local database, ... */
        *where = FALSE;
        return TRUE;
    }
    return FALSE;
}

static ULONG pn_socketbasetaglist(struct TagItem *tags REG(a0), struct PnBase *b REG(a6))
{
    ULONG index = 0;

    while (tags) {
        ULONG tag = tags->ti_Tag;

        switch (tag) {
        case TAG_DONE:   return 0;
        case TAG_IGNORE: tags++; continue;
        case TAG_MORE:   tags = (struct TagItem *)tags->ti_Data; continue;
        case TAG_SKIP:   tags += 1 + tags->ti_Data; continue;
        }
        index++;
        if (tag & TAG_USER) {
            BOOL set = (tag & 1) != 0, ref = (tag & 0x8000) != 0;
            ULONG code = (tag >> 1) & 0x3fff;
            ULONG *where = ref ? (ULONG *)tags->ti_Data : &tags->ti_Data;
            ULONG tmp;

            if (ref && !where)
                return index;
            if (set && !ref) {
                tmp = tags->ti_Data;
                where = &tmp;
            }
            if (!tag_code(b, code, set, where))
                return index;
        } else {
            return index;
        }
        tags++;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* getaddrinfo / getnameinfo (Roadshow API, IPv4)                           */
/* ---------------------------------------------------------------------- */

struct pn_addrinfo {
    LONG                ai_flags;
    LONG                ai_family;
    LONG                ai_socktype;
    LONG                ai_protocol;
    ULONG               ai_addrlen;
    APTR                ai_addr;
    char               *ai_canonname;
    struct pn_addrinfo *ai_next;
};

/* One allocation per result: addrinfo + sockaddr_in + canonical name. */
struct pn_ai_block {
    struct pn_addrinfo ai;
    UBYTE              sa[16];
    char               canon[256];
};

#define PN_SOCK_STREAM_  1
#define PN_SOCK_DGRAM_   2

#define AI_PASSIVE_      1
#define AI_CANONNAME_    2
#define AI_NUMERICHOST_  4
#define AI_EXT_          8
#define AI_NUMERICSERV_  16
#define NI_NUMERICHOST_  1
#define NI_NUMERICSERV_  2
#define NI_NOFQDN_       4
#define NI_NAMEREQD_     8
#define NI_DGRAM_        16

#define EAI_BADFLAGS_   (-1)
#define EAI_NONAME_     (-2)
#define EAI_AGAIN_      (-3)
#define EAI_FAIL_       (-4)
#define EAI_NODATA_     (-5)
#define EAI_FAMILY_     (-6)
#define EAI_SOCKTYPE_   (-7)
#define EAI_SERVICE_    (-8)
#define EAI_MEMORY_     (-10)
#define EAI_SYSTEM_     (-11)
#define EAI_BADHINTS_   (-12)
#define EAI_PROTOCOL_   (-13)

static void pn_freeaddrinfo(struct pn_addrinfo *ai REG(a0), struct PnBase *b REG(a6))
{
    (void)b;
    while (ai) {
        struct pn_addrinfo *next = ai->ai_next;
        FreeVec(ai);                    /* ai is the start of its pn_ai_block */
        ai = next;
    }
}

/* Port for a service name/number; -1 if unknown. udp: 0 tcp, 1 udp, -1 either. */
static LONG service_port(const char *serv, int udp, BOOL numeric_only)
{
    const char *p;
    ULONG v = 0;
    unsigned i;

    for (p = serv; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    if (p != serv && *p == 0)
        return v <= 65535 ? (LONG)v : -1;
    if (numeric_only)
        return -1;
    for (i = 0; i < NSERVICES; i++)
        if (str_ieq(serv, services[i].name) && (udp < 0 || services[i].udp == udp))
            return services[i].port;
    return -1;
}

static LONG pn_getaddrinfo(STRPTR host REG(a0), STRPTR serv REG(a1), struct pn_addrinfo *hints REG(a2),
                           struct pn_addrinfo **res REG(a3), struct PnBase *b REG(a6))
{
    LONG flags = 0, family = 0, socktype = 0, protocol = 0, port = 0;
    ULONG addrs[PN_MAXHOSTADDRS];
    int naddrs = 0, i, t;
    const char *canon = NULL;
    struct pn_addrinfo *head = NULL, **tail = &head;
    static const LONG types[2] = { PN_SOCK_STREAM_, PN_SOCK_DGRAM_ };

    if (!res)
        return EAI_BADHINTS_;
    *res = NULL;
    if (hints) {
        flags = hints->ai_flags; family = hints->ai_family;
        socktype = hints->ai_socktype; protocol = hints->ai_protocol;
        if (hints->ai_addrlen || hints->ai_addr || hints->ai_canonname || hints->ai_next)
            return EAI_BADHINTS_;
    }
    if (flags & ~(AI_PASSIVE_ | AI_CANONNAME_ | AI_NUMERICHOST_ | AI_EXT_ | AI_NUMERICSERV_))
        return EAI_BADFLAGS_;
    if (family != 0 && family != 2)
        return EAI_FAMILY_;
    if (socktype < 0 || socktype > 3)
        return EAI_SOCKTYPE_;
    if (!host && !serv)
        return EAI_NONAME_;
    if ((socktype == PN_SOCK_STREAM_ && protocol && protocol != 6) ||
        (socktype == PN_SOCK_DGRAM_ && protocol && protocol != 17))
        return EAI_PROTOCOL_;

    if (serv) {
        if (socktype == 3)
            return EAI_SERVICE_;
        port = service_port((const char *)serv, socktype == PN_SOCK_DGRAM_ ? 1 :
                            socktype == PN_SOCK_STREAM_ ? 0 : -1, (flags & AI_NUMERICSERV_) != 0);
        if (port < 0)
            return (flags & AI_NUMERICSERV_) ? EAI_NONAME_ : EAI_SERVICE_;
    }

    if (!host) {
        addrs[naddrs++] = (flags & AI_PASSIVE_) ? 0 : 0x7f000001UL;
        canon = "localhost";
    } else if (aton((const char *)host, &addrs[0])) {
        naddrs = 1;
        canon = (const char *)host;
    } else if (flags & AI_NUMERICHOST_) {
        return EAI_NONAME_;
    } else {
        REQ(PNOP_GETHOSTBYNAME);
        r.a[0] = (ULONG)host; r.a[1] = (ULONG)&b->hostbuf;
        if (call(b, &r) < 0)
            return r.herror == PN_TRY_AGAIN ? EAI_AGAIN_ :
                   r.error == PN_ENETDOWN  ? EAI_FAIL_ : EAI_NONAME_;
        for (i = 0; b->hostbuf.h_addr_list[i] && naddrs < PN_MAXHOSTADDRS; i++)
            addrs[naddrs++] = *(ULONG *)b->hostbuf.h_addr_list[i];
        canon = b->hostbuf.h_name;
        if (!naddrs)
            return EAI_NODATA_;
    }

    for (i = 0; i < naddrs; i++) {
        for (t = 0; t < 3; t++) {
            LONG st = t < 2 ? types[t] : 3;
            struct pn_ai_block *blk;

            if (socktype ? st != socktype : t == 2)     /* default: stream + dgram */
                continue;
            blk = AllocVec(sizeof(*blk), MEMF_ANY | MEMF_CLEAR);
            if (!blk) {
                pn_freeaddrinfo(head, b);
                return EAI_MEMORY_;
            }
            blk->ai.ai_family   = 2;
            blk->ai.ai_socktype = st;
            blk->ai.ai_protocol = st == PN_SOCK_STREAM_ ? 6 : st == PN_SOCK_DGRAM_ ? 17 : protocol;
            blk->ai.ai_addrlen  = 16;
            blk->ai.ai_addr     = blk->sa;
            blk->sa[0] = 16; blk->sa[1] = 2;
            blk->sa[2] = (UBYTE)(port >> 8); blk->sa[3] = (UBYTE)port;
            CopyMem(&addrs[i], &blk->sa[4], 4);
            if ((flags & AI_CANONNAME_) && !head && canon) {
                strncpy(blk->canon, canon, sizeof(blk->canon) - 1);
                blk->ai.ai_canonname = blk->canon;
            }
            *tail = &blk->ai;
            tail = &blk->ai.ai_next;
        }
    }
    *res = head;
    return head ? 0 : EAI_SOCKTYPE_;
}

static LONG pn_getnameinfo(UBYTE *sa REG(a0), ULONG salen REG(d0), STRPTR host REG(a1), ULONG hostlen REG(d1),
                           STRPTR serv REG(a2), ULONG servlen REG(d2), LONG flags REG(d3),
                           struct PnBase *b REG(a6))
{
    ULONG addr;
    UWORD port;
    char tmp[16];

    if (!sa || salen < 8)
        return EAI_FAIL_;
    if (sa[1] != 2)
        return EAI_FAMILY_;
    port = (UWORD)((sa[2] << 8) | sa[3]);
    CopyMem(&sa[4], &addr, 4);

    if (host && hostlen) {
        const char *name = NULL;
        if (!(flags & NI_NUMERICHOST_)) {
            REQ(PNOP_GETHOSTBYADDR);
            r.a[0] = addr; r.a[1] = (ULONG)&b->hostbuf;
            if (call(b, &r) >= 0)
                name = b->hostbuf.h_name;
            else if (flags & NI_NAMEREQD_)
                return EAI_NONAME_;
        }
        if (!name) {
            ntoa(addr, tmp);
            name = tmp;
        }
        if (strlen(name) + 1 > hostlen)
            return EAI_MEMORY_;
        strcpy((char *)host, name);
    }
    if (serv && servlen) {
        const char *name = NULL;
        unsigned i;
        if (!(flags & NI_NUMERICSERV_))
            for (i = 0; i < NSERVICES && !name; i++)
                if (services[i].port == port && services[i].udp == ((flags & NI_DGRAM_) ? 1 : 0))
                    name = services[i].name;
        if (!name) {
            char *e = put_dec(tmp, port);
            *e = 0;
            name = tmp;
        }
        if (strlen(name) + 1 > servlen)
            return EAI_MEMORY_;
        strcpy((char *)serv, name);
    }
    return 0;
}

static STRPTR pn_gai_strerror(LONG code REG(a0), struct PnBase *b REG(a6))
{
    static const char *const msg[] = {
        "Success", "Invalid value for ai_flags", "Name or service is not known",
        "Temporary failure in name resolution", "Non-recoverable failure in name resolution",
        "No address associated with name", "ai_family not supported",
        "ai_socktype not supported", "Service not supported for ai_socktype",
        "Address family for name not supported", "Memory allocation failure",
        "System error", "Invalid value for hints", "Resolved protocol is unknown",
    };
    (void)b;
    if (code <= 0 && code >= -13)
        return (STRPTR)msg[-code];
    return (STRPTR)"Unknown error";
}
/* ---------------------------------------------------------------------- */
/* Roadshow query API: interfaces, network statistics, name servers         */
/* ---------------------------------------------------------------------- */

#define PN_ENODEV_  19
#define IFQ_BASE_   (TAG_USER + 1900)

/* Current status from the core (AllocVec'd, caller frees), NULL on error. */
static struct PnStatus *get_status(struct PnBase *b)
{
    struct PnStatus *st = AllocVec(sizeof(*st), MEMF_ANY | MEMF_CLEAR);
    REQ(PNOP_STATUS);

    if (!st) {
        set_errno(b, PN_ENOMEM);
        return NULL;
    }
    r.a[0] = (ULONG)st;
    r.a[1] = sizeof(*st);
    if (call(b, &r) < 0) {
        FreeVec(st);
        return NULL;
    }
    return st;
}

static void init_list(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static BOOL name_eq(const char *a, const char *b)
{
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return *a == 0 && *b == 0;
}

struct pn_ifnode { struct Node node; char name[16]; };

/* The list and its nodes are one allocation. Not running: an empty list. */
static struct List *pn_obtaininterfacelist(struct PnBase *b REG(a6))
{
    struct PnStatus *st = get_status(b);
    ULONG n = st ? st->nifs : 0, i;
    struct List *l = AllocVec(sizeof(struct List) + n * sizeof(struct pn_ifnode), MEMF_ANY | MEMF_CLEAR);

    if (l) {
        struct pn_ifnode *in = (struct pn_ifnode *)(l + 1);
        init_list(l);
        for (i = 0; i < n; i++, in++) {
            strncpy(in->name, st->ifs[i].name, sizeof(in->name) - 1);
            in->node.ln_Name = in->name;
            AddTail(l, &in->node);
        }
    } else {
        set_errno(b, PN_ENOMEM);
    }
    if (st)
        FreeVec(st);
    return l;
}

static VOID pn_releaseinterfacelist(struct List *l REG(a0), struct PnBase *b REG(a6))
{
    (void)b;
    if (l)
        FreeVec(l);
}

struct pn_sin { UBYTE len, family; UWORD port; ULONG addr; UBYTE zero[8]; };

static void put_sin(APTR where, ULONG addr)
{
    struct pn_sin *sin = (struct pn_sin *)where;
    memset(sin, 0, sizeof(*sin));
    sin->len    = sizeof(*sin);
    sin->family = 2;                    /* AF_INET */
    sin->addr   = addr;
}

static LONG pn_queryinterfacetaglist(STRPTR name REG(a0), struct TagItem *tags REG(a1),
                                     struct PnBase *b REG(a6))
{
    struct PnStatus *st;
    struct PnIfStatus *is = NULL;
    struct PnIfStatusEx *x = NULL;
    ULONG i;

    if (!name) {
        set_errno(b, PN_EFAULT);
        return -1;
    }
    if (!(st = get_status(b)))
        return -1;
    for (i = 0; i < st->nifs && !is; i++)
        if (name_eq((char *)name, st->ifs[i].name)) {
            is = &st->ifs[i];
            x  = &st->ifx[i];
        }
    if (!is) {
        FreeVec(st);
        set_errno(b, PN_ENODEV_);
        return -1;
    }
    while (tags) {
        ULONG t = tags->ti_Tag;
        APTR d = (APTR)tags->ti_Data;
        ULONG bits = x->addrbits ? x->addrbits : (x->p2p ? 0 : 48);

        switch (t) {
        case TAG_DONE:   tags = NULL; continue;
        case TAG_IGNORE: tags++; continue;
        case TAG_MORE:   tags = (struct TagItem *)d; continue;
        case TAG_SKIP:   tags += 1 + (ULONG)d; continue;
        }
        if (d && t > IFQ_BASE_ && t <= IFQ_BASE_ + 42) {
            ULONG *l = (ULONG *)d;
            switch (t - IFQ_BASE_) {
            case 1:  strncpy(b->ifq_device, is->device, sizeof(b->ifq_device) - 1);
                     *(STRPTR *)d = (STRPTR)b->ifq_device; break;          /* DeviceName */
            case 2:  *l = is->unit; break;                                  /* DeviceUnit */
            case 3:  *l = bits; break;                                      /* HardwareAddressSize */
            case 4:  memcpy(d, is->mac, bits / 8 < 6 ? bits / 8 : 6); break; /* HardwareAddress */
            case 5:                                                         /* MTU */
            case 34: *l = is->mtu; break;                                   /* HardwareMTU */
            case 6:  *l = x->bps; break;                                    /* BPS */
            case 7:  *l = x->hwtype; break;                                 /* HardwareType */
            case 8:  *l = is->rx_packets; break;                            /* PacketsReceived */
            case 9:  *l = is->tx_packets; break;                            /* PacketsSent */
            case 10:                                                        /* BadData */
            case 38: *l = x->rx_errors; break;                              /* InputErrors */
            case 13: l[0] = x->up_secs; l[1] = 0; break;                    /* LastStart (timeval) */
            case 14: put_sin(d, is->ip); break;                             /* Address */
            case 15: put_sin(d, x->p2p ? is->gateway : 0); break;           /* DestinationAddress */
            case 16: put_sin(d, x->p2p || !is->ip ? 0 : (is->ip | ~is->netmask)); break;
            case 17: put_sin(d, is->netmask); break;                        /* NetMask */
            case 19: *l = is->link_up ? 3 : 2; break;                       /* State: SM_Up/SM_Down */
            case 20: *l = is->dhcp ? 2 : is->ip ? 1 : 0; break;             /* AddressBindType */
            case 21: memset(d, 0, sizeof(struct DateStamp)); break;         /* AddressLeaseExpires */
            case 22: put_sin(d, st->dns[0]); break;                         /* PrimaryDNSAddress */
            case 23: put_sin(d, st->dns[1]); break;                         /* SecondaryDNSAddress */
            case 24:
            case 25: *l = x->rx_reqs; break;                                /* Num/MaxReadRequests */
            case 26:
            case 27: *l = x->tx_reqs; break;                                /* Num/MaxWriteRequests */
            case 28: l[0] = 0; l[1] = is->rx_bytes; break;                  /* GetBytesIn (SBQUAD_T) */
            case 29: l[0] = 0; l[1] = is->tx_bytes; break;                  /* GetBytesOut */
            case 31: memset(d, 0, 5 * sizeof(ULONG));                       /* GetSANA2CopyStats */
                     l[2] = is->rx_packets; l[3] = is->tx_packets; break;   /* CopyToBuff/FromBuff */
            case 32: *l = x->rx_busy; break;                                /* NumReadRequestsPending */
            case 33: *l = x->tx_busy; break;                                /* NumWriteRequestsPending */
            case 35: *l = is->tx_dropped; break;                            /* OutputDrops */
            case 36: *l = is->rx_dropped; break;                            /* InputDrops */
            case 37: *l = x->tx_errors; break;                              /* OutputErrors */
            default: *l = 0; break;    /* Overruns, UnknownTypes, Metric, DebugMode, multicasts, drops */
            }
        }
        tags++;
    }
    FreeVec(st);
    return 0;
}

/* NETSTATUS_tcp_sockets / NETSTATUS_udp_sockets entry (24 bytes on m68k). */
struct pn_pcd {
    ULONG faddr; UWORD fport;
    ULONG laddr; UWORD lport;
    ULONG rxq, txq;
    LONG  state;
};

static LONG pn_getnetworkstatistics(LONG type REG(d0), LONG version REG(d1), APTR dest REG(a0),
                                    LONG size REG(d2), struct PnBase *b REG(a6))
{
    /* BSD tcp_fsm states for PNSS_NONE, LISTEN, CONNECTING, ESTABLISHED, CLOSING, BOUND */
    static const BYTE tcp_state[] = { -1, 1, 2, 4, 7, 0 };
    struct PnSockInfo *si;
    struct pn_pcd *out;
    LONG n, i, cnt = 0, total;
    UBYTE want = type == 9 ? 1 : 2;     /* stream or datagram sockets */
    REQ(PNOP_SOCKLIST);

    if (version != 1 || type < 0 || type > 10 || type == 8) {
        set_errno(b, PN_EINVAL);
        return -1;
    }
    if (type <= 7) {                    /* protocol counters: the core fills them */
        REQ(PNOP_NETSTATS);
        r.a[0] = (ULONG)type;
        r.a[1] = (ULONG)dest;
        r.a[2] = (ULONG)size;
        return call(b, &r);
    }
    if (!(si = AllocVec(64 * (sizeof(*si) + sizeof(*out)), MEMF_ANY | MEMF_CLEAR))) {
        set_errno(b, PN_ENOMEM);
        return -1;
    }
    out = (struct pn_pcd *)(si + 64);
    r.a[0] = (ULONG)si;
    r.a[1] = 64;
    if ((n = call(b, &r)) < 0) {
        FreeVec(si);
        return -1;
    }
    for (i = 0; i < n; i++) {
        struct pn_pcd *o;
        if (si[i].type != want)
            continue;
        o = &out[cnt++];
        o->faddr = si[i].rip;  o->fport = si[i].rport;
        o->laddr = si[i].lip;  o->lport = si[i].lport;
        o->rxq   = si[i].rxq;  o->txq   = si[i].txq;
        o->state = want == 1 && si[i].state < sizeof(tcp_state) ? tcp_state[si[i].state] : -1;
    }
    total = cnt * (LONG)sizeof(*out);
    if (dest) {
        if (size < total)
            total = size > 0 ? size : 0;
        CopyMem(out, dest, total);
    }
    FreeVec(si);
    return total;
}

struct pn_dnsnode {
    struct MinNode node;
    LONG   size;                        /* the public part: these four members */
    STRPTR address;
    LONG   usecount;
    char   text[16];
};

static struct List *pn_obtaindomainnameserverlist(struct PnBase *b REG(a6))
{
    struct PnStatus *st = get_status(b);
    struct List *l = AllocVec(sizeof(struct List) + 3 * sizeof(struct pn_dnsnode), MEMF_ANY | MEMF_CLEAR);
    int i;

    if (l) {
        struct pn_dnsnode *dn = (struct pn_dnsnode *)(l + 1);
        init_list(l);
        for (i = 0; st && i < 3; i++) {
            ULONG a = st->dns[i];
            if (!a)
                continue;
            ntoa(a, dn->text);
            dn->size     = (LONG)((UBYTE *)&dn->text - (UBYTE *)dn);
            dn->address  = (STRPTR)dn->text;
            dn->usecount = 1;
            AddTail(l, (struct Node *)&dn->node);
            dn++;
        }
    } else {
        set_errno(b, PN_ENOMEM);
    }
    if (st)
        FreeVec(st);
    return l;
}

static VOID pn_releasedomainnameserverlist(struct List *l REG(a0), struct PnBase *b REG(a6))
{
    (void)b;
    if (l)
        FreeVec(l);
}

static BOOL pn_getdefaultdomainname(STRPTR buf REG(a0), LONG size REG(d0), struct PnBase *b REG(a6))
{
    struct PnStatus *st = get_status(b);
    BOOL ok = st && st->domain[0];

    if (ok && buf && size > 0) {
        strncpy((char *)buf, st->domain, size - 1);
        buf[size - 1] = 0;
    }
    if (st)
        FreeVec(st);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Roadshow's configuration calls (0.5): routes, interfaces, name servers.  */
/* Addresses come as strings ("192.168.1.1", a route also "10.0.0.0/8").    */
/* Each returns 0, or -1 with errno set. The changes last until PrismNet    */
/* stops; the saved configuration in ENV:PrismNet is not changed.           */
/* ---------------------------------------------------------------------- */

#define RTA_BASE_   (TAG_USER + 1600)
#define IFC_BASE_   (TAG_USER + 1800)

static LONG netcfg(struct PnBase *b, ULONG sub, ULONG a1, ULONG a2, ULONG a3)
{
    REQ(PNOP_NETCFG);
    r.a[0] = sub; r.a[1] = a1; r.a[2] = a2; r.a[3] = a3;
    return call(b, &r);
}

/* the next real tag: TAG_IGNORE, TAG_MORE and TAG_SKIP followed */
static struct TagItem *next_tag(struct TagItem **tp)
{
    struct TagItem *t = *tp;

    while (t) {
        switch (t->ti_Tag) {
        case TAG_DONE:   t = NULL; break;
        case TAG_IGNORE: t++; continue;
        case TAG_MORE:   t = (struct TagItem *)t->ti_Data; continue;
        case TAG_SKIP:   t += t->ti_Data + 1; continue;
        default:         *tp = t + 1; return t;
        }
    }
    *tp = NULL;
    return NULL;
}

/* "a.b.c.d" or "a.b.c.d/bits" -> address; *bits = -1 without "/bits" */
static BOOL parse_cidr(const char *s, ULONG *addr, LONG *bits)
{
    char buf[20];
    int i;

    *bits = -1;
    if (!s)
        return FALSE;
    for (i = 0; s[i] && s[i] != '/' && i < (int)sizeof(buf) - 1; i++)
        buf[i] = s[i];
    buf[i] = 0;
    if (s[i] == '/') {
        LONG n = 0;
        const char *p = s + i + 1;
        if (!*p)
            return FALSE;
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || (n = n * 10 + (*p - '0')) > 32)
                return FALSE;
        }
        *bits = n;
    } else if (s[i]) {
        return FALSE;
    }
    return aton(buf, addr);
}

static ULONG bits_mask(LONG bits)
{
    return bits <= 0 ? 0 : bits >= 32 ? 0xffffffffUL : ~((1UL << (32 - bits)) - 1);
}

/* the classful mask BSD's route(8) takes for a network without /bits */
static LONG class_bits(ULONG a)
{
    return (a >> 31) == 0 ? 8 : (a >> 30) == 2 ? 16 : 24;
}

static LONG route_tags(struct TagItem *tags, ULONG *net, ULONG *mask, ULONG *gw)
{
    struct TagItem *t;
    BOOL dest = FALSE, deflt = FALSE;

    *net = *mask = *gw = 0;
    while ((t = next_tag(&tags)) != NULL) {
        ULONG a;
        LONG bits;
        switch (t->ti_Tag) {
        case RTA_BASE_ + 1:             /* RTA_Destination: a host, or a network by its class */
        case RTA_BASE_ + 4:             /* RTA_DestinationHost */
        case RTA_BASE_ + 5:             /* RTA_DestinationNet */
            if (!parse_cidr((const char *)t->ti_Data, &a, &bits))
                return PN_EINVAL;
            if (bits < 0) {
                if (t->ti_Tag == RTA_BASE_ + 4)
                    bits = 32;
                else if (t->ti_Tag == RTA_BASE_ + 5)
                    bits = class_bits(a);
                else
                    bits = (a & ~bits_mask(class_bits(a))) ? 32 : class_bits(a);
            }
            *mask = bits_mask(bits);
            *net = a & *mask;
            dest = TRUE;
            break;
        case RTA_BASE_ + 3:             /* RTA_DefaultGateway */
            deflt = TRUE;
            /* fall through */
        case RTA_BASE_ + 2:             /* RTA_Gateway */
            if (!parse_cidr((const char *)t->ti_Data, gw, &bits) || bits >= 0)
                return PN_EINVAL;
            break;
        }
    }
    if (deflt)
        *net = *mask = 0;
    else if (!dest)
        return PN_EINVAL;
    return 0;
}

static LONG pn_addroutetaglist(struct TagItem *tags REG(a0), struct PnBase *b REG(a6))
{
    ULONG net, mask, gw;
    LONG e = route_tags(tags, &net, &mask, &gw);
    if (e) { set_errno(b, e); return -1; }
    return netcfg(b, PNNC_ROUTE_ADD, net, mask, gw) < 0 ? -1 : 0;
}

static LONG pn_deleteroutetaglist(struct TagItem *tags REG(a0), struct PnBase *b REG(a6))
{
    ULONG net, mask, gw;
    LONG e = route_tags(tags, &net, &mask, &gw);
    if (e) { set_errno(b, e); return -1; }
    return netcfg(b, PNNC_ROUTE_DEL, net, mask, gw) < 0 ? -1 : 0;
}

/* ChangeRouteTagList: adding a route that exists already changes its gateway */
static LONG pn_changeroutetaglist(struct TagItem *tags REG(a0), struct PnBase *b REG(a6))
{
    return pn_addroutetaglist(tags, b);
}

/* GetRouteInfo: the routes as BSD routing messages (struct rt_msghdr, 74
 * bytes on m68k, then a sockaddr_in each for destination, gateway and net
 * mask as rtm_addrs says), the last one followed by a header with
 * rtm_msglen 0. FreeRouteInfo() frees it. */
#define RTMHDR_SIZE 74
static APTR pn_getrouteinfo(LONG family REG(d0), LONG flags REG(d1), struct PnBase *b REG(a6))
{
    struct PnRouteInfo *ri;
    UBYTE *buf, *p;
    LONG n, i;

    (void)flags;
    if (family != 0 && family != 2) {   /* AF_UNSPEC or AF_INET */
        set_errno(b, PN_EAFNOSUPPORT);
        return NULL;
    }
    if (!(ri = AllocVec(32 * sizeof(*ri), MEMF_ANY | MEMF_CLEAR))) {
        set_errno(b, PN_ENOMEM);
        return NULL;
    }
    if ((n = netcfg(b, PNNC_ROUTES, (ULONG)ri, 32, 0)) < 0) {
        FreeVec(ri);
        return NULL;
    }
    if (!(buf = AllocVec((n + 1) * (RTMHDR_SIZE + 3 * 16), MEMF_ANY | MEMF_CLEAR))) {
        FreeVec(ri);
        set_errno(b, PN_ENOMEM);
        return NULL;
    }
    for (p = buf, i = 0; i < n; i++) {
        UBYTE *h = p, *sa = p + RTMHDR_SIZE;
        ULONG f = 0x1, addrs = 0x1 | 0x4;           /* RTF_UP; RTA_DST | RTA_NETMASK */
        int k, cnt = 0;
        if (ri[i].gw) { f |= 0x2; addrs |= 0x2; }   /* RTF_GATEWAY; RTA_GATEWAY */
        if (ri[i].flags & PNRT_HOST) f |= 0x4;      /* RTF_HOST */
        if (ri[i].flags & PNRT_STATIC) f |= 0x800;  /* RTF_STATIC */
        for (k = 0; k < 3; k++) {                   /* in RTA_ bit order: DST, GATEWAY, NETMASK */
            ULONG a = k == 0 ? ri[i].net : k == 1 ? ri[i].gw : ri[i].mask;
            if (!(addrs & (1UL << k)))
                continue;
            sa[0] = 16; sa[1] = 2;                  /* sin_len, sin_family AF_INET */
            CopyMem(&a, sa + 4, 4);
            sa += 16;
            cnt++;
        }
        *(UWORD *)h = (UWORD)(RTMHDR_SIZE + cnt * 16);
        h[2] = 3;                                   /* RTM_VERSION */
        h[3] = 4;                                   /* RTM_GET */
        *(UWORD *)(h + 4) = 0;
        CopyMem(&f, h + 6, 4);
        CopyMem(&addrs, h + 10, 4);
        p = sa;
    }
    FreeVec(ri);
    return buf;                                     /* ends with a zeroed header */
}

static VOID pn_freerouteinfo(APTR buf REG(a0), struct PnBase *b REG(a6))
{
    (void)b;
    if (buf)
        FreeVec(buf);
}

static LONG pn_addinterfacetaglist(STRPTR name REG(a0), STRPTR device REG(a1), LONG unit REG(d0),
                                   struct TagItem *tags REG(a2), struct PnBase *b REG(a6))
{
    (void)tags;                         /* IFA_*: buffer counts, ARP/IP types - PrismNet's own */
    return netcfg(b, PNNC_IF_ADD, (ULONG)name, (ULONG)device, (ULONG)unit) < 0 ? -1 : 0;
}

static LONG pn_configureinterfacetaglist(STRPTR name REG(a0), struct TagItem *tags REG(a1),
                                         struct PnBase *b REG(a6))
{
    struct TagItem *t;

    while ((t = next_tag(&tags)) != NULL) {
        ULONG a, what = 0;
        LONG bits;
        switch (t->ti_Tag) {
        case IFC_BASE_ + 1: what = PNIF_ADDRESS; break;     /* IFC_Address */
        case IFC_BASE_ + 2: what = PNIF_NETMASK; break;     /* IFC_NetMask */
        case IFC_BASE_ + 3: what = PNIF_PEER;    break;     /* IFC_DestinationAddress */
        case IFC_BASE_ + 8:                                 /* IFC_State: SM_* */
            if (netcfg(b, PNNC_IF_SET, (ULONG)name, PNIF_STATE, t->ti_Data) < 0)
                return -1;
            continue;
        case IFC_BASE_ + 17:                                /* IFC_LimitMTU */
            if (netcfg(b, PNNC_IF_SET, (ULONG)name, PNIF_MTU, t->ti_Data) < 0)
                return -1;
            continue;
        case IFC_BASE_ + 6: case IFC_BASE_ + 7:             /* alias addresses: not supported */
            set_errno(b, PN_EOPNOTSUPP);
            return -1;
        default:            /* broadcast, metric, associated route/DNS, Complete, debug: nothing to do */
            continue;
        }
        if (!parse_cidr((const char *)t->ti_Data, &a, &bits) || bits >= 0) {
            set_errno(b, PN_EINVAL);
            return -1;
        }
        if (netcfg(b, PNNC_IF_SET, (ULONG)name, what, a) < 0)
            return -1;
    }
    return 0;
}

static LONG pn_removeinterface(STRPTR name REG(a0), LONG force REG(d0), struct PnBase *b REG(a6))
{
    (void)force;
    return netcfg(b, PNNC_IF_REMOVE, (ULONG)name, 0, 0) < 0 ? -1 : 0;
}

static LONG dns_call(struct PnBase *b, ULONG sub, STRPTR address)
{
    ULONG a;
    LONG bits;
    if (!parse_cidr((const char *)address, &a, &bits) || bits >= 0) {
        set_errno(b, PN_EINVAL);
        return -1;
    }
    return netcfg(b, sub, a, 0, 0) < 0 ? -1 : 0;
}

static LONG pn_adddomainnameserver(STRPTR address REG(a0), struct PnBase *b REG(a6))
{
    return dns_call(b, PNNC_DNS_ADD, address);
}

static LONG pn_removedomainnameserver(STRPTR address REG(a0), struct PnBase *b REG(a6))
{
    return dns_call(b, PNNC_DNS_DEL, address);
}

static VOID pn_setdefaultdomainname(STRPTR name REG(a0), struct PnBase *b REG(a6))
{
    netcfg(b, PNNC_DOMAIN, (ULONG)(name ? name : (STRPTR)""), 0, 0);
}

/* ---------------------------------------------------------------------- */
/* library construction                                                     */
/* ---------------------------------------------------------------------- */

static ULONG pn_stub_zero(void) { return 0; }

#define F(x) ((APTR)(x))
static const APTR functable[] = {
    F(pn_open), F(pn_close), F(pn_expunge), F(pn_null),
    /* -30 .. -300: AmiTCP API */
    F(pn_socket), F(pn_bind), F(pn_listen), F(pn_accept), F(pn_connect),
    F(pn_sendto), F(pn_send), F(pn_recvfrom), F(pn_recv), F(pn_shutdown),
    F(pn_setsockopt), F(pn_getsockopt), F(pn_getsockname), F(pn_getpeername),
    F(pn_ioctlsocket), F(pn_closesocket), F(pn_waitselect), F(pn_setsocketsignals),
    F(pn_getdtablesize), F(pn_obtainsocket), F(pn_releasesocket), F(pn_releasecopyofsocket),
    F(pn_errno), F(pn_seterrnoptr), F(pn_inet_ntoa), F(pn_inet_addr), F(pn_inet_lnaof),
    F(pn_inet_netof), F(pn_inet_makeaddr), F(pn_inet_network), F(pn_gethostbyname),
    F(pn_gethostbyaddr), F(pn_getnetbyname), F(pn_getnetbyaddr), F(pn_getservbyname),
    F(pn_getservbyport), F(pn_getprotobyname), F(pn_getprotobynumber), F(pn_vsyslog),
    F(pn_dup2socket), F(pn_sendmsg), F(pn_recvmsg), F(pn_gethostname), F(pn_gethostid),
    F(pn_socketbasetaglist), F(pn_getsocketevents),
    /* -306 .. -588: Roadshow extensions; the query and configuration calls are implemented */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -306..-330 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -336..-360 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -366..-390 bpf */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero),                                   /* -396..-408 */
    F(pn_addroutetaglist), F(pn_deleteroutetaglist),                                     /* -414..-420 */
    F(pn_changeroutetaglist), F(pn_freerouteinfo), F(pn_getrouteinfo),                  /* -426..-438 */
    F(pn_addinterfacetaglist), F(pn_configureinterfacetaglist),                          /* -444..-450 */
    F(pn_releaseinterfacelist), F(pn_obtaininterfacelist), F(pn_queryinterfacetaglist), /* -456..-468 */
    F(pn_stub_zero), F(pn_stub_zero),                                                    /* -474..-480 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero),                  /* -486..-504 */
    F(pn_getnetworkstatistics),                                                          /* -510 */
    F(pn_adddomainnameserver), F(pn_removedomainnameserver),                             /* -516..-522 */
    F(pn_releasedomainnameserverlist), F(pn_obtaindomainnameserverlist),                 /* -528..-534 */
    F(pn_stub_zero),                                                                     /* -540 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -546..-570 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero),                                   /* -576..-588 */
    F(pn_inet_aton), F(pn_inet_ntop), F(pn_inet_pton),                                   /* -594..-606 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -612..-636 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -642..-666 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -672..-696 */
    F(pn_getdefaultdomainname), F(pn_setdefaultdomainname), F(pn_stub_zero), F(pn_stub_zero), /* -702..-720 */
    F(pn_stub_zero),                                                                     /* -726 */
    F(pn_removeinterface), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -732..-756 */
    F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), F(pn_stub_zero), /* -762..-786 */
    F(pn_stub_zero), F(pn_stub_zero),                                                    /* -792..-798 */
    F(pn_freeaddrinfo), F(pn_getaddrinfo), F(pn_gai_strerror), F(pn_getnameinfo),       /* -804..-822 */
    (APTR)-1
};

extern const char pn_libname[], pn_idstring[];

/* Called from the ROMTag in lib_start.S. */
struct Library *pn_lib_init(BPTR seglist REG(d0), struct ExecBase *sysbase REG(a6))
{
    struct PnBase *lib;

    SysBase = sysbase;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", PN_DOS_MIN);
    lib = (struct PnBase *)MakeLibrary((APTR)functable, NULL, NULL, sizeof(struct PnBase), seglist);
    if (!lib)
        return NULL;
    lib->lib.lib_Node.ln_Type = NT_LIBRARY;
    lib->lib.lib_Node.ln_Name = (char *)pn_libname;
    lib->lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    lib->lib.lib_Version      = 4;
    lib->lib.lib_Revision     = 3;
    lib->lib.lib_IdString     = (APTR)pn_idstring;
    lib->seglist              = seglist;
    AddLibrary(&lib->lib);
    return &lib->lib;
}
