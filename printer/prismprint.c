/*
 * prismprint.device - a network printer port for printer.device.
 * Part of PrismNet. GPL-3.0-only, see LICENSE.
 *
 * printer.device writes a job to its port device as if to a parallel port;
 * this one sends it to a network printer instead, over any bsdsocket.library
 * stack (PrismNet, Roadshow, ...):
 *
 *   RAW  - TCP port 9100 (JetDirect / "port 9100"), the job streamed as it
 *          comes;
 *   LPD  - RFC 1179 (port 515): the job is kept in memory and sent at its
 *          end, since LPD wants the length first.
 *
 * Settings per unit (0-3): ENV:PrismNet/printer, printer1 .. printer3 (also
 * read from ENVARC:, see config_read), lines KEY=VALUE:
 *   HOST=<name or address>  PORT=<n>  PROTOCOL=RAW|LPD  QUEUE=<lpd queue>
 *   IDLE=<seconds>   a job also ends after this long without data (default 10)
 *   TIMEOUT=<seconds> how long the printer may refuse data (default 60)
 * A job starts with the first write and ends when the last opener closes the
 * unit (printer.device closes it after each job) or after IDLE seconds.
 *
 * Commands: CMD_WRITE, CMD_READ (nothing to read), PDCMD_QUERY/SDCMD_QUERY
 * (always ready), CMD_RESET/CLEAR/FLUSH/STOP/START and the SETPARAMS
 * commands (accepted, nothing to set). One process does the network work:
 * a socket library base belongs to the task that opened it.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/parallel.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <proto/bsdsocket.h>

#define REG(r, decl) register decl __asm(#r)

struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *SocketBase = NULL;

extern const char pp_devname[], pp_idstring[];

#define NUNITS      4
#define CHUNK       32768           /* LPD job buffer pieces */
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif

struct Chunk { struct Chunk *next; ULONG used; UBYTE data[CHUNK]; };

struct Cfg {
    char host[64];
    char queue[32];
    UWORD port;
    UBYTE lpd;
    UWORD idle, timeout;
};

struct PUnit {
    struct Unit u;
    UBYTE num;
    UBYTE closing;                  /* last opener gone: end the job          */
    UBYTE job;                      /* a job is open                          */
    UBYTE failed;                   /* the job lost the printer after data went out */
    LONG sock;                      /* RAW: the connection, else -1           */
    ULONG sent;                     /* bytes of this job                      */
    UWORD idle;                     /* seconds without data                   */
    struct Chunk *head, *tail;      /* LPD: the job                           */
    struct Cfg cfg;
};

static struct Device *ppdev;
static BPTR seglist;
static struct PUnit units[NUNITS];
static struct Process *proc;
static struct MsgPort *pport;       /* the process's request port */
static struct Task *parent;
static struct SignalSemaphore devlock;
static ULONG jobno;
static ULONG endsig;               /* to the process: end the closing units' jobs */

/* ---- small helpers (no C library in a device) -------------------------- */

static ULONG slen(const char *s) { const char *p = s; while (*p) p++; return p - s; }

static void scopy(char *d, const char *s, ULONG n)
{
    while (n > 1 && *s) { *d++ = *s++; n--; }
    *d = 0;
}

static void mzero(void *p, ULONG n) { UBYTE *b = p; while (n--) *b++ = 0; }

static void mcopy(void *d, const void *s, ULONG n)
{
    UBYTE *a = d; const UBYTE *b = s;
    while (n--) *a++ = *b++;
}

static UBYTE upper(UBYTE c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static BOOL keyis(const char *line, const char *key, const char **val)
{
    while (*key && upper(*line) == *key) { line++; key++; }
    if (*key || *line != '=') return FALSE;
    *val = line + 1;
    return TRUE;
}

static ULONG num(const char *s)
{
    ULONG v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static char *putnum(char *p, ULONG v)
{
    char t[12];
    int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static void init_list(struct MinList *l)
{
    l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
    l->mlh_Tail = NULL;
    l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

/* ---- settings ------------------------------------------------------------ */

/* ENV:PrismNet/printer[N], or ENVARC: when ENV: hasn't got it (OS 3.2's ENV:
 * only copies a file in when it is opened by name - opening it is enough). */
static void config_read(struct PUnit *u)
{
    char name[40], env[40], *buf, *p, *e;
    BPTR fh;
    LONG n = 0;
    struct Cfg *c = &u->cfg;

    mzero(c, sizeof(*c));
    c->port = 9100;
    c->idle = 10;
    c->timeout = 60;
    scopy(c->queue, "lp", sizeof(c->queue));
    p = put(name, "ENVARC:PrismNet/printer");
    if (u->num) p = putnum(p, u->num);
    *p = 0;
    p = put(env, "ENV:");
    scopy(p, name + 7, sizeof(env) - 4);
    if (!(fh = Open((STRPTR)env, MODE_OLDFILE)))
        fh = Open((STRPTR)name, MODE_OLDFILE);
    if (!fh) return;
    if ((buf = AllocVec(1024, MEMF_ANY))) {
        n = Read(fh, buf, 1023);
        if (n < 0) n = 0;
        buf[n] = 0;
        for (p = buf; *p; p = e) {
            const char *v;
            for (e = p; *e && *e != '\n' && *e != '\r'; e++) ;
            if (*e) *e++ = 0;
            while (*p == ' ' || *p == '\t') p++;
            if (keyis(p, "HOST", &v)) scopy(c->host, v, sizeof(c->host));
            else if (keyis(p, "QUEUE", &v)) scopy(c->queue, v, sizeof(c->queue));
            else if (keyis(p, "PORT", &v)) c->port = num(v);
            else if (keyis(p, "IDLE", &v)) c->idle = num(v);
            else if (keyis(p, "TIMEOUT", &v)) c->timeout = num(v);
            else if (keyis(p, "PROTOCOL", &v)) {
                c->lpd = upper(v[0]) == 'L';
                if (c->lpd && c->port == 9100) c->port = 515;
            }
        }
        FreeVec(buf);
    }
    Close(fh);
    if (!c->idle) c->idle = 10;
    if (!c->timeout) c->timeout = 60;
}

/* ---- the network --------------------------------------------------------- */

static BOOL net_open(void)
{
    if (!SocketBase)
        SocketBase = OpenLibrary("bsdsocket.library", 4);
    return SocketBase != NULL;
}

static void net_close_if_idle(void)
{
    int i;
    for (i = 0; i < NUNITS; i++)
        if (units[i].job) return;
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

/* wait until s can be written (or read), Ctrl-C or secs: >0 ready, 0 timeout */
static LONG sock_wait(LONG s, BOOL wr, ULONG secs)
{
    fd_set fds;
    struct timeval tv;
    ULONG sig = SIGBREAKF_CTRL_C;
    LONG r;
    mzero(&fds, sizeof(fds));
    FD_SET(s, &fds);
    tv.tv_sec = secs;
    tv.tv_usec = 0;
    r = WaitSelect(s + 1, wr ? NULL : &fds, wr ? &fds : NULL, NULL, &tv, &sig);
    return sig ? -1 : r;
}

/* a non-blocking connection to host:port, or -1 */
static LONG net_connect(const struct Cfg *c, UWORD port, BOOL lpdport)
{
    struct sockaddr_in a;
    struct hostent *h;
    LONG s, one = 1, err = 0, len = sizeof(err);
    ULONG addr;

    if (!c->host[0] || !net_open()) return -1;
    mzero(&a, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = port;
    addr = inet_addr((STRPTR)c->host);
    if (addr == 0xffffffffUL) {
        if (!(h = gethostbyname((STRPTR)c->host)) || h->h_length != 4) return -1;
        mcopy(&addr, h->h_addr_list[0], 4);
    }
    a.sin_addr.s_addr = addr;
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    if (lpdport) {
        /* RFC 1179 wants a source port of 721-731; printers mostly don't
         * care, so any port will do when none of those can be had */
        struct sockaddr_in me;
        UWORD p;
        mzero(&me, sizeof(me));
        me.sin_len = sizeof(me);
        me.sin_family = AF_INET;
        for (p = 721; p <= 731; p++) {
            me.sin_port = p;
            if (bind(s, (struct sockaddr *)&me, sizeof(me)) == 0) break;
        }
    }
    IoctlSocket(s, FIONBIO, (char *)&one);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (Errno() != EINPROGRESS || sock_wait(s, TRUE, 10) <= 0 ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, (socklen_t *)&len) < 0 || err) {
            CloseSocket(s);
            return -1;
        }
    }
    return s;
}

static BOOL send_all(LONG s, const UBYTE *p, ULONG n, ULONG secs)
{
    while (n) {
        LONG w = send(s, (UBYTE *)p, n > 8192 ? 8192 : n, 0);
        if (w < 0) {
            if (Errno() != EWOULDBLOCK || sock_wait(s, TRUE, secs) <= 0) return FALSE;
            continue;
        }
        p += w;
        n -= w;
    }
    return TRUE;
}

/* LPD: one acknowledgement byte, 0 = fine */
static BOOL lpd_ack(LONG s, ULONG secs)
{
    UBYTE b;
    LONG r;
    for (;;) {
        r = recv(s, &b, 1, 0);
        if (r == 1) return b == 0;
        if (r == 0 || Errno() != EWOULDBLOCK || sock_wait(s, FALSE, secs) <= 0) return FALSE;
    }
}

/* LPD: the buffered job, as data file and control file of job number n */
static BOOL lpd_send(struct PUnit *u)
{
    struct Cfg *c = &u->cfg;
    char cmd[96], cf[160], *p;
    struct Chunk *k;
    LONG s;
    ULONG n = jobno++ % 1000, cflen;
    BOOL ok = FALSE;
    static const UBYTE zero = 0;
    char id[8];

    p = id;
    if (n < 100) *p++ = '0';
    if (n < 10) *p++ = '0';
    p = putnum(p, n);
    *p = 0;
    /* the control file: host, user, job name, print the data file as is */
    p = cf;
    p = put(p, "Hamiga\nPamiga\nJAmiga print job\nldfA");
    p = put(p, id); p = put(p, "amiga\nUdfA");
    p = put(p, id); p = put(p, "amiga\nNAmiga\n");
    cflen = p - cf;

    if ((s = net_connect(c, c->port, TRUE)) < 0) return FALSE;
    p = cmd; *p++ = 2; p = put(p, c->queue); *p++ = '\n';
    if (!send_all(s, (UBYTE *)cmd, p - cmd, c->timeout) || !lpd_ack(s, c->timeout)) goto out;
    p = cmd; *p++ = 2; p = putnum(p, cflen); p = put(p, " cfA"); p = put(p, id); p = put(p, "amiga\n");
    if (!send_all(s, (UBYTE *)cmd, p - cmd, c->timeout) || !lpd_ack(s, c->timeout)) goto out;
    if (!send_all(s, (UBYTE *)cf, cflen, c->timeout) || !send_all(s, &zero, 1, c->timeout) ||
        !lpd_ack(s, c->timeout)) goto out;
    p = cmd; *p++ = 3; p = putnum(p, u->sent); p = put(p, " dfA"); p = put(p, id); p = put(p, "amiga\n");
    if (!send_all(s, (UBYTE *)cmd, p - cmd, c->timeout) || !lpd_ack(s, c->timeout)) goto out;
    for (k = u->head; k; k = k->next)
        if (!send_all(s, k->data, k->used, c->timeout)) goto out;
    ok = send_all(s, &zero, 1, c->timeout) && lpd_ack(s, c->timeout);
out:
    CloseSocket(s);
    return ok;
}

/* ---- jobs (the process) ---------------------------------------------------- */

static void job_free(struct PUnit *u)
{
    struct Chunk *k, *n;
    for (k = u->head; k; k = n) { n = k->next; FreeVec(k); }
    u->head = u->tail = NULL;
}

/* the first write of a job: settings, and for RAW the connection */
static BOOL job_start(struct PUnit *u)
{
    config_read(u);
    u->sent = 0;
    u->idle = 0;
    u->failed = 0;
    u->sock = -1;
    if (!u->cfg.lpd) {
        if ((u->sock = net_connect(&u->cfg, u->cfg.port, FALSE)) < 0) {
            net_close_if_idle();
            return FALSE;
        }
    } else if (!net_open())
        return FALSE;
    u->job = 1;
    return TRUE;
}

static void job_end(struct PUnit *u)
{
    if (!u->job) return;
    if (u->cfg.lpd) {
        if (u->sent && !u->failed) lpd_send(u);
        job_free(u);
    } else if (u->sock >= 0) {
        CloseSocket(u->sock);
    }
    u->sock = -1;
    u->job = 0;
    net_close_if_idle();
}

static BOOL job_write(struct PUnit *u, const UBYTE *p, ULONG n)
{
    if (u->failed) return FALSE;
    u->idle = 0;
    if (!u->cfg.lpd) {
        if (!send_all(u->sock, p, n, u->cfg.timeout)) {
            u->failed = 1;
            return FALSE;
        }
    } else {
        while (n) {
            ULONG k;
            if (!u->tail || u->tail->used == CHUNK) {
                struct Chunk *c = AllocVec(sizeof(struct Chunk), MEMF_ANY);
                if (!c) { u->failed = 1; return FALSE; }
                c->next = NULL;
                c->used = 0;
                if (u->tail) u->tail->next = c; else u->head = c;
                u->tail = c;
            }
            k = CHUNK - u->tail->used;
            if (k > n) k = n;
            mcopy(u->tail->data + u->tail->used, p, k);
            u->tail->used += k;
            p += k;
            n -= k;
        }
    }
    return TRUE;
}

static void do_write(struct IOStdReq *io)
{
    struct PUnit *u = (struct PUnit *)io->io_Unit;
    ULONG n = io->io_Length;
    if (n == 0xffffffffUL) n = slen(io->io_Data);
    io->io_Actual = 0;
    if (!u->job && !job_start(u)) {
        io->io_Error = ParErr_LineErr;   /* printer.device asks to retry or cancel */
        return;
    }
    if (!job_write(u, io->io_Data, n)) {
        io->io_Error = ParErr_LineErr;
        if (!u->sent) job_end(u);        /* nothing went out yet: a retry starts afresh */
        return;
    }
    u->sent += n;
    io->io_Actual = n;
}

static void pp_proc(void)
{
    struct MsgPort *mp = CreateMsgPort();
    struct IOStdReq *io;
    ULONG sigs;
    int i;

    BYTE eb = AllocSignal(-1);

    if (mp && eb < 0) { DeleteMsgPort(mp); mp = NULL; }
    endsig = mp ? 1UL << eb : 0;
    pport = mp;
    proc = mp ? (struct Process *)FindTask(NULL) : NULL;
    Signal(parent, SIGF_SINGLE);
    if (!mp) return;
    for (;;) {
        ULONG mask = (1UL << mp->mp_SigBit) | SIGBREAKF_CTRL_C | endsig;
        BOOL busy = FALSE;
        for (i = 0; i < NUNITS; i++) busy |= units[i].job;
        if (busy && SocketBase) {
            /* a job is open: wake once a second to count its idle time */
            struct timeval tv;
            ULONG got = mask;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (WaitSelect(0, NULL, NULL, NULL, &tv, &got) == 0 && !got)
                for (i = 0; i < NUNITS; i++)
                    if (units[i].job && ++units[i].idle >= units[i].cfg.idle)
                        job_end(&units[i]);
            sigs = got | SetSignal(0, mask);
        } else
            sigs = Wait(mask);
        while ((io = (struct IOStdReq *)GetMsg(mp))) {
            if (io->io_Command == CMD_WRITE) do_write(io);
            ReplyMsg((struct Message *)io);
        }
        if (sigs & endsig) {
            for (i = 0; i < NUNITS; i++)
                if (units[i].closing) { job_end(&units[i]); units[i].closing = 0; }
            Signal(parent, SIGF_SINGLE);
        }
        if (sigs & SIGBREAKF_CTRL_C) break;
    }
    for (i = 0; i < NUNITS; i++) job_end(&units[i]);
    while ((io = (struct IOStdReq *)GetMsg(mp))) {
        io->io_Error = IOERR_ABORTED;
        ReplyMsg((struct Message *)io);
    }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
    Forbid();                            /* gone before the device can be expunged */
    pport = NULL;
    DeleteMsgPort(mp);
    FreeSignal(eb);
    proc = NULL;
    Signal(parent, SIGF_SINGLE);
}

static void proc_start(void)
{
    parent = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    /* the opener may be a plain task (OS 3.1's printer.device opens its port
     * from its own task): nothing is inherited from it */
    if (CreateNewProcTags(NP_Entry, (ULONG)pp_proc, NP_Name, (ULONG)pp_devname,
                          NP_StackSize, 8192, NP_CurrentDir, 0, NP_HomeDir, 0,
                          NP_CopyVars, FALSE, NP_Input, 0, NP_Output, 0,
                          NP_CloseInput, FALSE, NP_CloseOutput, FALSE, TAG_DONE))
        Wait(SIGF_SINGLE);
}

/* to the process, and wait for its answer (sig = endsig or Ctrl-C) */
static void proc_signal_wait(ULONG sig)
{
    if (!proc) return;
    parent = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    Signal((struct Task *)proc, sig);
    Wait(SIGF_SINGLE);
}

/* ---- the device ------------------------------------------------------------ */

#ifdef PP_DEBUG
/* a trace of opens and commands after the device base (pptrace prints it) */
#define TRACE 512
static void trace(ULONG a, ULONG b, ULONG c)
{
    ULONG *t = (ULONG *)((UBYTE *)ppdev + sizeof(struct Device));
    ULONG i;
    Disable();
    i = t[0];
    if (i < (TRACE / 4 - 1) / 3) { t[1 + 3 * i] = a; t[2 + 3 * i] = b; t[3 + 3 * i] = c; t[0] = i + 1; }
    Enable();
}
#else
#define TRACE 0
#define trace(a, b, c)
#endif

void pp_open(REG(a1, struct IOStdReq *io), REG(d0, ULONG unitnum), REG(d1, ULONG flags),
             REG(a6, struct Device *dev))
{
    struct PUnit *u;
    (void)flags;
    trace(0x4f50454e, unitnum, ((ULONG)io->io_Message.mn_Length << 16) | flags);
    dev->dd_Library.lib_OpenCnt++;       /* no expunge while we may Wait() */
    io->io_Error = IOERR_OPENFAIL;
    /* (no length check: OS 3.1's printer.device opens its port with an
     * mn_Length of 0, and parallel.device never looked at it) */
    if (unitnum >= NUNITS) goto out;
    ObtainSemaphore(&devlock);
    if (!proc) {
        if (!DOSBase) DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
        if (DOSBase) proc_start();
    }
    if (proc) {
        u = &units[unitnum];
        u->u.unit_OpenCnt++;
        io->io_Unit = &u->u;
        io->io_Error = 0;
        dev->dd_Library.lib_OpenCnt++;
        dev->dd_Library.lib_Flags &= ~LIBF_DELEXP;
    }
    ReleaseSemaphore(&devlock);
out:
    trace(0x4f504e44, (ULONG)(LONG)io->io_Error, (ULONG)proc);
    dev->dd_Library.lib_OpenCnt--;
}

static BPTR do_expunge(struct Device *dev)
{
    BPTR seg;
    if (dev->dd_Library.lib_OpenCnt || proc) { dev->dd_Library.lib_Flags |= LIBF_DELEXP; return 0; }
    seg = seglist;
    Remove(&dev->dd_Library.lib_Node);
    if (DOSBase) { CloseLibrary((struct Library *)DOSBase); DOSBase = NULL; }
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize,
            dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
    return seg;
}

BPTR pp_close(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    struct PUnit *u = (struct PUnit *)io->io_Unit;
    trace(0x434c4f53, u->num, u->u.unit_OpenCnt);
    ObtainSemaphore(&devlock);
    if (!--u->u.unit_OpenCnt) {
        u->closing = 1;                  /* its job ends now */
        proc_signal_wait(endsig);
    }
    io->io_Device = (struct Device *)-1;
    io->io_Unit = (struct Unit *)-1;
    if (dev->dd_Library.lib_OpenCnt == 1)
        proc_signal_wait(SIGBREAKF_CTRL_C);  /* the last opener: the process ends */
    ReleaseSemaphore(&devlock);
    if (!--dev->dd_Library.lib_OpenCnt && (dev->dd_Library.lib_Flags & LIBF_DELEXP))
        return do_expunge(dev);
    return 0;
}

BPTR pp_expunge(REG(a6, struct Device *dev))
{
    return do_expunge(dev);
}

ULONG pp_null(void) { return 0; }

void pp_beginio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    (void)dev;
    trace(0x434d4400 | io->io_Command, io->io_Length, ((ULONG)io->io_Message.mn_Length << 16) | io->io_Flags);
    io->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->io_Error = 0;
    switch (io->io_Command) {
    case CMD_WRITE:
        if (pport) {
            io->io_Flags &= ~IOF_QUICK;
            PutMsg(pport, (struct Message *)io);
            return;
        }
        io->io_Error = ParErr_NotOpen;
        break;
    case CMD_READ:
        io->io_Actual = 0;
        break;
    case PDCMD_QUERY:                    /* = SDCMD_QUERY: always ready */
        /* the request's size tells parallel from serial; no size (OS 3.1's
         * printer.device) is a parallel one */
        if (!io->io_Message.mn_Length || (io->io_Message.mn_Length >= sizeof(struct IOExtPar) &&
                                          io->io_Message.mn_Length < sizeof(struct IOExtPar) + 16))
            ((struct IOExtPar *)io)->io_Status = IOPTF_PARSEL;   /* selected, not busy, paper */
        else if (io->io_Message.mn_Length >= 82)
            *(UWORD *)((UBYTE *)io + 80) = 0;                     /* IOExtSer io_Status: all fine */
        io->io_Actual = 0;
        break;
    case PDCMD_SETPARAMS:                /* = SDCMD_SETPARAMS */
    case CMD_RESET:
    case CMD_CLEAR:
    case CMD_FLUSH:
    case CMD_STOP:
    case CMD_START:
        break;
    default:
        io->io_Error = IOERR_NOCMD;
        break;
    }
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg((struct Message *)io);
}

/* a write still waiting in the process's port comes back aborted; one being
 * sent finishes (the job's own timeout bounds it) */
LONG pp_abortio(REG(a1, struct IOStdReq *io), REG(a6, struct Device *dev))
{
    struct Node *n;
    (void)dev;
    Forbid();
    if (pport)
        for (n = pport->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
            if (n == (struct Node *)io) {
                Remove(n);
                io->io_Error = IOERR_ABORTED;
                ReplyMsg((struct Message *)io);
                break;
            }
    Permit();
    return 0;
}

static const APTR vectors[] = {
    (APTR)pp_open, (APTR)pp_close, (APTR)pp_expunge, (APTR)pp_null,
    (APTR)pp_beginio, (APTR)pp_abortio, (APTR)-1
};

/* called from start.S with the segment list */
struct Device *pp_init(REG(d0, BPTR seg))
{
    struct Device *dev;
    int i;
    SysBase = *(struct ExecBase **)4;
    dev = (struct Device *)MakeLibrary((APTR)vectors, NULL, NULL, sizeof(struct Device) + TRACE, 0);
    if (!dev) return NULL;
    seglist = seg;
    InitSemaphore(&devlock);
    for (i = 0; i < NUNITS; i++) {
        mzero(&units[i], sizeof(units[i]));
        units[i].num = i;
        units[i].sock = -1;
        units[i].u.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        units[i].u.unit_MsgPort.mp_Flags = PA_IGNORE;
        init_list((struct MinList *)&units[i].u.unit_MsgPort.mp_MsgList);
    }
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)pp_devname;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = 1;
    dev->dd_Library.lib_Revision = 0;
    dev->dd_Library.lib_IdString = (APTR)pp_idstring;
    ppdev = dev;
    AddDevice(dev);
    return dev;
}
