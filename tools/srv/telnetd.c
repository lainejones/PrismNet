/*
 * telnetd - PrismNet's telnet server: an AmigaDOS Shell over the network.
 *
 * Started by inetd for each connection: "telnetd SOCKET=<id> [options]".
 * After a login (PrismNet's users, as for ftpd) the caller gets a Shell in
 * their home directory: "NewShell TNxxxxxx:", where TNxxxxxx: is a device we
 * add for the session whose handler is a message port in this process. The
 * Shell opens it as its console, and we answer its DOS packets
 * (ACTION_READ with cooked line editing, or raw after SetMode(), ACTION_WRITE,
 * WAIT_CHAR, SCREEN_MODE, FINDINPUT/OUTPUT for Open("*")), so every CLI program
 * works without knowing it is on the network.
 *
 * The telnet layer and the console handler follow NilBBS (src/node/telnet.c
 * and door.c, same author), cut down for a Shell session.
 *
 *   telnetd SOCKET/N   the connection, from inetd
 *           NOLOGIN=user   no login: everyone is that user (a trusted LAN only)
 *           IDLE=s         hang up after s seconds without input (default 1800)
 *
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <bsdsocket/socketbasetags.h>
#include <proto/bsdsocket.h>

#include <libraries/usergroup.h>
/* Only the inline stubs: clib/usergroup_protos.h clashes with libnix's grp.h. */
struct passwd;
struct group;
#include <../ndk-include/inline/usergroup.h>
struct ug_passwd { char *pw_name, *pw_passwd; LONG pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };

static const char version[] __attribute__((used)) = "$VER: telnetd 1.0 (18.10.2026) PrismNet";

struct Library *SocketBase, *UserGroupBase;

/* ---- telnet ---------------------------------------------------------------- */

#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240
#define O_BINARY 0
#define O_ECHO   1
#define O_SGA    3

static LONG sock = -1;
static BOOL online = TRUE;
static UBYTE inbuf[2048];           /* decoded bytes from the caller (ring) */
static LONG in_head, in_tail;
static UBYTE outbuf[2048];
static LONG outlen;
static int  tstate, lastcr;
static UBYTE optverb;
static ULONG last_input;            /* ticks */
static LONG idle_secs = 1800;

static ULONG ticks_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 4320000UL + (ULONG)ds.ds_Minute * 3000UL + ds.ds_Tick;
}

static void flush_out(void)
{
    UBYTE *p = outbuf;
    while (outlen > 0 && online) {
        LONG n = send(sock, p, outlen, 0);
        if (n <= 0) { online = FALSE; break; }
        p += n; outlen -= n;
    }
    outlen = 0;
}

static void out_byte(UBYTE c)
{
    if (outlen >= (LONG)sizeof(outbuf) - 2) flush_out();
    outbuf[outlen++] = c;
}

static void send_cmd(UBYTE verb, UBYTE opt)
{
    out_byte(IAC); out_byte(verb); out_byte(opt);
}

/* text to the caller: LF -> CR LF, the Amiga's one-byte CSI -> ESC [, IAC doubled */
static void tn_write(const UBYTE *p, LONG len)
{
    LONG i;
    for (i = 0; i < len; i++) {
        UBYTE c = p[i];
        if (c == '\n') { out_byte('\r'); out_byte('\n'); }
        else if (c == 0x9B) { out_byte(27); out_byte('['); }
        else if (c == IAC) { out_byte(IAC); out_byte(IAC); }
        else out_byte(c);
    }
}

static void tn_puts(const char *s)
{
    tn_write((const UBYTE *)s, strlen(s));
}

static void in_put(UBYTE c)
{
    LONG next = (in_head + 1) % sizeof(inbuf);
    if (next != in_tail) { inbuf[in_head] = c; in_head = next; }
}

static LONG in_avail(void)
{
    return (in_head - in_tail + sizeof(inbuf)) % sizeof(inbuf);
}

static LONG in_get(void)
{
    UBYTE c;
    if (in_head == in_tail) return -1;
    c = inbuf[in_tail];
    in_tail = (in_tail + 1) % sizeof(inbuf);
    return c;
}

/* one byte off the wire: telnet commands answered, data into inbuf */
static void tn_feed(UBYTE c)
{
    switch (tstate) {
    case 0:
        if (c == IAC) { tstate = 1; return; }
        if (lastcr && (c == '\n' || c == 0)) { lastcr = 0; return; }   /* CR LF / CR NUL = one Enter */
        lastcr = c == '\r';
        in_put(c);
        return;
    case 1:                                         /* after IAC */
        if (c == IAC) { in_put(IAC); tstate = 0; return; }
        if (c >= WILL && c <= DONT) { optverb = c; tstate = 2; return; }
        tstate = c == SB ? 3 : 0;
        return;
    case 2:                                         /* WILL/WONT/DO/DONT <option> */
        tstate = 0;
        if (optverb == DO)
            send_cmd(c == O_ECHO || c == O_SGA || c == O_BINARY ? WILL : WONT, c);
        else if (optverb == WILL)
            send_cmd(c == O_SGA || c == O_BINARY ? DO : DONT, c);
        return;                                     /* WONT/DONT: nothing to say */
    case 3:                                         /* inside SB ... IAC SE: ignored */
        if (c == IAC) tstate = 4;
        return;
    case 4:
        tstate = c == SE ? 0 : 3;
        return;
    }
}

/* read what the network has, without waiting */
static void tn_poll(void)
{
    UBYTE buf[512];
    LONG n, i;
    for (;;) {
        fd_set rd;
        struct timeval tv;
        tv.tv_secs = 0; tv.tv_micro = 0;
        FD_ZERO(&rd); FD_SET(sock, &rd);
        if (WaitSelect(sock + 1, &rd, NULL, NULL, &tv, NULL) <= 0) return;
        n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) { online = FALSE; return; }
        for (i = 0; i < n; i++) tn_feed(buf[i]);
        last_input = ticks_now();
    }
}

/* wait for network input or one of `sigs` for up to ms; returns the signals seen */
static ULONG tn_wait(ULONG sigs, ULONG ms)
{
    fd_set rd;
    struct timeval tv;
    ULONG s = sigs | SIGBREAKF_CTRL_C;
    flush_out();
    FD_ZERO(&rd); FD_SET(sock, &rd);
    tv.tv_secs = ms / 1000; tv.tv_micro = (ms % 1000) * 1000;
    if (WaitSelect(sock + 1, &rd, NULL, NULL, &tv, &s) > 0)
        tn_poll();
    return s;
}

/* ---- login -------------------------------------------------------------------- */

/* one line from the caller; echo shows it (or '*' when FALSE) */
static BOOL get_line(char *buf, int size, BOOL echo)
{
    int n = 0;
    ULONG start = ticks_now();
    for (;;) {
        LONG c;
        while ((c = in_get()) >= 0) {
            if (c == '\r' || c == '\n') { buf[n] = 0; tn_puts("\n"); return TRUE; }
            if ((c == 8 || c == 127) && n > 0) { n--; if (echo) tn_puts("\b \b"); continue; }
            if (c == 3 || c == 4) return FALSE;
            if (c >= 32 && n < size - 1) {
                UBYTE b = (UBYTE)c;
                buf[n++] = (char)c;
                if (echo) tn_write(&b, 1);
            }
        }
        if (!online || ticks_now() - start > 120 * 50) return FALSE;
        tn_wait(0, 1000);
    }
}

static struct ug_passwd *login(const char *nologin)
{
    char user[64], pass[64];
    int tries;
    for (tries = 0; tries < 3 && online; tries++) {
        struct ug_passwd *pw;
        if (nologin) {
            strncpy(user, nologin, sizeof(user) - 1);
            user[sizeof(user) - 1] = 0;
            return UserGroupBase ? (struct ug_passwd *)getpwnam((STRPTR)user) : NULL;
        }
        tn_puts("login: ");
        if (!get_line(user, sizeof(user), TRUE) || !user[0]) continue;
        tn_puts("Password: ");
        if (!get_line(pass, sizeof(pass), FALSE)) return NULL;
        pw = UserGroupBase ? (struct ug_passwd *)getpwnam((STRPTR)user) : NULL;
        if (pw && (!pw->pw_passwd || !pw->pw_passwd[0]))
            pw = NULL;                              /* no password: no network login */
        if (pw) {
            STRPTR c = crypt((STRPTR)pass, (STRPTR)pw->pw_passwd);
            if (!c || strcmp((char *)c, pw->pw_passwd)) pw = NULL;
        }
        memset(pass, 0, sizeof(pass));
        if (pw) return pw;
        Delay(100);
        tn_puts("Login incorrect\n\n");
    }
    return NULL;
}

/* ---- the console handler: the Shell's I/O ----------------------------------------- */

#define H_IN  1
#define H_OUT 2

static struct MsgPort *port;
static struct Task *shelltask;      /* whoever sends us packets */
static BOOL raw, in_open, out_open, hungup;
static LONG extra_open, next_id = 10;
static struct DosPacket *readq[8];
static int nread;
static struct DosPacket *waitpkt;
static ULONG wait_deadline;
static UBYTE line[256], ready[512];
static int linelen, readylen;
static BOOL eof_pending;

static void cooked_input(void)
{
    LONG c;
    while (readylen < (int)sizeof(ready) - 2 && (c = in_get()) >= 0) {
        if (c == 27) {                              /* ESC [ ... : a cursor key, not for a line */
            while (in_avail() && (c = in_get()) >= 0 && !(c >= '@' && c <= '~' && c != '['))
                ;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (readylen + linelen + 1 > (int)sizeof(ready)) break;
            memcpy(ready + readylen, line, linelen);
            readylen += linelen;
            ready[readylen++] = '\n';
            linelen = 0;
            tn_puts("\n");
        } else if (c == 8 || c == 127) {
            if (linelen > 0) { linelen--; tn_puts("\b \b"); }
        } else if (c == 21 || c == 24) {            /* ^U ^X: kill the line */
            while (linelen > 0) { linelen--; tn_puts("\b \b"); }
        } else if (c == 3) {                        /* ^C: break */
            if (shelltask) Signal(shelltask, SIGBREAKF_CTRL_C);
            tn_puts("^C");
        } else if (c == 4) {                        /* ^D */
            if (shelltask) Signal(shelltask, SIGBREAKF_CTRL_D);
        } else if (c == 28) {                       /* ^\ = end of file */
            if (linelen == 0) eof_pending = TRUE;
        } else if (c >= 32 && linelen < (int)sizeof(line) - 1) {
            UBYTE b = (UBYTE)c;
            line[linelen++] = b;
            tn_write(&b, 1);                        /* echo */
        }
    }
}

static BOOL input_ready(void)
{
    if (hungup) return TRUE;
    return raw ? in_avail() > 0 : (readylen > 0 || eof_pending);
}

static BOOL serve_read(struct DosPacket *p)
{
    UBYTE *buf = (UBYTE *)p->dp_Arg2;
    LONG want = p->dp_Arg3, got = 0;

    if (hungup) { ReplyPkt(p, 0, 0); return TRUE; }  /* EOF: the Shell ends */
    if (want <= 0) { ReplyPkt(p, 0, 0); return TRUE; }
    if (raw) {
        LONG c;
        while (got < want && (c = in_get()) >= 0) buf[got++] = (UBYTE)c;
        if (!got) return FALSE;
    } else {
        if (readylen == 0) {
            if (eof_pending) { eof_pending = FALSE; ReplyPkt(p, 0, 0); return TRUE; }
            return FALSE;
        }
        got = readylen < want ? readylen : want;
        memcpy(buf, ready, got);
        memmove(ready, ready + got, readylen - got);
        readylen -= got;
    }
    ReplyPkt(p, got, 0);
    return TRUE;
}

static void service_queues(void)
{
    if (!raw && !hungup) cooked_input();
    while (nread > 0 && serve_read(readq[0])) {
        memmove(readq, readq + 1, (nread - 1) * sizeof(readq[0]));
        nread--;
    }
    if (waitpkt) {
        if (input_ready()) { ReplyPkt(waitpkt, DOSTRUE, 0); waitpkt = NULL; }
        else if (ticks_now() >= wait_deadline) { ReplyPkt(waitpkt, DOSFALSE, 0); waitpkt = NULL; }
    }
}

static void handle_packet(struct DosPacket *p)
{
    if (p->dp_Port && p->dp_Port->mp_SigTask) shelltask = (struct Task *)p->dp_Port->mp_SigTask;
    switch (p->dp_Type) {
    case ACTION_READ:
        if (nread < 8) readq[nread++] = p;
        else ReplyPkt(p, -1, ERROR_NO_FREE_STORE);
        break;
    case ACTION_WRITE:
        if (!hungup) tn_write((const UBYTE *)p->dp_Arg2, p->dp_Arg3);
        ReplyPkt(p, p->dp_Arg3, 0);
        break;
    case ACTION_SCREEN_MODE:                        /* SetMode(): nonzero = raw */
        if (!p->dp_Arg1 && raw) linelen = 0;
        raw = p->dp_Arg1 != 0;
        ReplyPkt(p, DOSTRUE, 0);
        break;
    case ACTION_WAIT_CHAR: {
        ULONG usecs = (ULONG)p->dp_Arg1;
        if (!raw) cooked_input();
        if (input_ready()) ReplyPkt(p, DOSTRUE, 0);
        else if (usecs == 0) ReplyPkt(p, DOSFALSE, 0);
        else {
            if (waitpkt) ReplyPkt(waitpkt, DOSFALSE, 0);
            waitpkt = p;
            wait_deadline = ticks_now() + usecs / 20000 + 1;
        }
        break;
    }
    case ACTION_FINDINPUT:                          /* Open("*") / Open("CONSOLE:") */
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        struct FileHandle *fh = (struct FileHandle *)BADDR(p->dp_Arg1);
        fh->fh_Port = (struct MsgPort *)DOSTRUE;    /* interactive */
        fh->fh_Args = next_id++;
        extra_open++;
        ReplyPkt(p, DOSTRUE, 0);
        break;
    }
    case ACTION_END:
        if (p->dp_Arg1 == H_IN) in_open = FALSE;
        else if (p->dp_Arg1 == H_OUT) out_open = FALSE;
        else if (extra_open > 0) extra_open--;
        ReplyPkt(p, DOSTRUE, 0);
        break;
    case ACTION_CHANGE_SIGNAL: case ACTION_FLUSH:
        ReplyPkt(p, DOSTRUE, 0);
        break;
    case ACTION_IS_FILESYSTEM:
        ReplyPkt(p, DOSFALSE, 0);
        break;
    default:
        ReplyPkt(p, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        break;
    }
}

/*
 * The Shell is started from a helper process: while SystemTags() sets it up,
 * the new Shell already sends packets to our port and waits for the replies,
 * so this process must be free to answer them (NilBBS found that out).
 */
static struct {
    char   cmd[256], dir[256];
    LONG   rc;
    struct Task *parent;
    ULONG  donesig;
    volatile BOOL done;
} LN;

static void launcher(void)
{
    BPTR dl = LN.dir[0] ? Lock((STRPTR)LN.dir, ACCESS_READ) : 0, od = 0;
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (dl) od = CurrentDir(dl);                    /* the Shell starts here */
    LN.rc = SystemTags((STRPTR)LN.cmd, SYS_Input, in, SYS_Output, out,
                       NP_StackSize, 16384, NP_Priority, 0, TAG_END);
    if (dl) { CurrentDir(od); UnLock(dl); }
    if (in) Close(in);
    if (out) Close(out);
    Forbid();                                       /* exit inside Forbid: the parent may go now */
    LN.done = TRUE;
    Signal(LN.parent, LN.donesig);
}

static void run_shell(struct ug_passwd *pw)
{
    LONG donebit = AllocSignal(-1);
    ULONG portsig, hup_at = 0;
    struct DosList *dev = NULL;
    char devname[16];

    if (donebit < 0 || !(port = CreateMsgPort())) { tn_puts("telnetd: out of signals\n"); return; }
    portsig = 1UL << port->mp_SigBit;
    /* our console as a device of its own for this session */
    snprintf(devname, sizeof(devname), "TN%06lX", (unsigned long)(((ULONG)port >> 2) & 0xFFFFFF));
    if ((dev = MakeDosEntry((STRPTR)devname, DLT_DEVICE)) != NULL) {
        dev->dol_Task = port;
        LockDosList(LDF_DEVICES | LDF_WRITE);
        if (!AddDosEntry(dev)) { FreeDosEntry(dev); dev = NULL; }
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
    }
    if (!dev) { tn_puts("telnetd: can't add the console device\n"); goto out; }
    snprintf(LN.cmd, sizeof(LN.cmd), "NewShell %s:", devname);
    strncpy(LN.dir, pw && pw->pw_dir ? pw->pw_dir : "", sizeof(LN.dir) - 1);
    LN.parent = FindTask(NULL);
    LN.donesig = 1UL << donebit;
    LN.done = FALSE;
    if (!CreateNewProcTags(NP_Entry, (ULONG)launcher, NP_Name, (ULONG)"telnetd launcher",
                           NP_StackSize, 8192, TAG_END)) {
        tn_puts("telnetd: can't start a Shell\n");
        goto out;
    }
    /* answer the packets while NewShell opens our console and starts the Shell */
    while (!LN.done) {
        struct Message *m;
        Wait(portsig | LN.donesig);
        while ((m = GetMsg(port)))
            handle_packet((struct DosPacket *)m->mn_Node.ln_Name);
        flush_out();
    }
    if (extra_open == 0) {
        tn_puts("telnetd: can't start a Shell\n");
        goto out;
    }
    last_input = ticks_now();
    /* the session: until the Shell has closed every handle it opened on us */
    while (extra_open > 0) {
        struct Message *m;
        tn_wait(portsig, 200);
        while ((m = GetMsg(port)))
            handle_packet((struct DosPacket *)m->mn_Node.ln_Name);
        if (!hungup && (!online || ticks_now() - last_input > (ULONG)idle_secs * 50)) {
            hungup = TRUE;                          /* the caller went: reads get EOF */
            hup_at = ticks_now();
            if (shelltask) Signal(shelltask, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D);
        }
        service_queues();
        flush_out();
        if (hungup && shelltask && ticks_now() - hup_at > 30 * 50) {
            Signal(shelltask, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D);   /* nudge it again */
            hup_at = ticks_now();
        }
    }
out:
    if (dev) {                                      /* nothing uses it any more */
        LockDosList(LDF_DEVICES | LDF_WRITE);
        RemDosEntry(dev);
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
        FreeDosEntry(dev);
    }
    while (port && GetMsg(port))
        ;
    if (port) DeleteMsgPort(port);
    port = NULL;
    FreeSignal(donebit);
}

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"SOCKET/K/N,NOLOGIN/K,IDLE/K/N", args, NULL);
    struct ug_passwd *pw;
    char host[64];

    if (!rda || !args[0])
        return RETURN_FAIL;                         /* inetd starts us */
    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if ((sock = ObtainSocket(*(LONG *)args[0], AF_INET, SOCK_STREAM, 0)) < 0) {
        CloseLibrary(SocketBase);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    UserGroupBase = OpenLibrary((CONST_STRPTR)"usergroup.library", 1);
    if (args[2] && *(LONG *)args[2] > 0) idle_secs = *(LONG *)args[2];

    send_cmd(WILL, O_ECHO);                         /* we echo; character at a time */
    send_cmd(WILL, O_SGA);
    send_cmd(DO, O_SGA);
    if (gethostname(host, sizeof(host)) != 0) strcpy(host, "amiga");
    host[sizeof(host) - 1] = 0;
    tn_puts("\nPrismNet telnet - ");
    tn_puts(host);
    tn_puts("\n\n");
    tn_wait(0, 300);                                /* let the option replies arrive */
    in_head = in_tail = 0;                          /* and drop anything typed before the prompt */

    if ((pw = login((const char *)args[1])) != NULL) {
        tn_puts("\nWelcome, ");
        tn_puts(pw->pw_name);
        tn_puts(". EndCLI or ^\\ ends the session.\n\n");
        flush_out();
        run_shell(pw);
    } else if (online) {
        tn_puts("\nGoodbye.\n");
    }
    flush_out();
    CloseSocket(sock);
    if (UserGroupBase) CloseLibrary(UserGroupBase);
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return RETURN_OK;
}
