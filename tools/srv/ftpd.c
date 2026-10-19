/*
 * ftpd - PrismNet's FTP server (RFC 959, with PASV/EPSV, SIZE, MDTM, REST).
 *
 * Started by inetd for each connection: "ftpd SOCKET=<id> [options]".
 * The Amiga looks like a Unix tree to FTP clients: "/" lists the volumes,
 * /Work/Music/x.mod is Work:Music/x.mod, so FileZilla, WinSCP, Windows
 * Explorer and the command line "ftp" all find their way around.
 *
 * Accounts are PrismNet's users (ENV:PrismNet/passwd, PrismNet Prefs >
 * Users): name, password (usergroup.library crypt()) and home directory.
 * An account without a password can't log in over the network.
 *
 *   ftpd SOCKET/N   the connection, from inetd
 *        ANONYMOUS=dir   allow "anonymous"/"ftp" logins, read-only, inside dir
 *        PASVPORTS=a-b   ports for passive data connections (default: any)
 *        PASVADDRESS=ip  address to announce for PASV (behind NAT)
 *        READONLY        nobody may change anything
 *        IDLE=s          drop an idle connection after s seconds (default 600)
 *
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
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
/* libnix has its own internal inline/usergroup.h that shadows the NDK one */
#include <../ndk-include/inline/usergroup.h>
struct ug_passwd { char *pw_name, *pw_passwd; LONG pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };

static const char version[] __attribute__((used)) = "$VER: ftpd 1.0 (18.10.2026) PrismNet";

struct Library *SocketBase, *UserGroupBase;

#define BUFSIZE  16384
#define PATHMAX  512

static LONG ctrl = -1;              /* the control connection */
static LONG pasv_fd = -1;           /* listening for a passive data connection */
static struct sockaddr_in port_sa;  /* PORT/EPRT target */
static BOOL have_port;
static ULONG local_ip;              /* our address on the control connection */
static char  linebuf[600];
static LONG  linelen;
static char  rbuf[1024];
static LONG  rlen, rpos;
static UBYTE *xbuf;                 /* transfer buffer */

static BOOL  logged_in, anon, readonly, binary = TRUE;
static char  user[64];
static char  vcwd[PATHMAX] = "/";   /* the client's view: "/", "/Work/dir" */
static char  anonroot[PATHMAX];     /* anonymous: what "/" is */
static char  rnfr[PATHMAX];         /* RNFR: the Amiga path to rename */
static ULONG rest;                  /* REST: start offset of the next RETR/STOR */
static UWORD pasv_lo, pasv_hi;
static ULONG pasv_addr;
static LONG  idle_secs = 600;

static const char *month[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };

/* ---- the control connection --------------------------------------------- */

static void reply(int code, const char *fmt, ...)
{
    char buf[600];
    int n;
    va_list ap;
    n = snprintf(buf, sizeof(buf), "%d ", code);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    send(ctrl, buf, n, 0);
}

static void sendstr(LONG fd, const char *s)
{
    send(fd, (APTR)s, strlen(s), 0);
}

/* one command line, without CR LF; FALSE when the client went away or idled out */
static BOOL get_line(void)
{
    linelen = 0;
    for (;;) {
        while (rpos < rlen) {
            char c = rbuf[rpos++];
            if (c == '\n') {
                if (linelen && linebuf[linelen - 1] == '\r') linelen--;
                linebuf[linelen] = 0;
                return TRUE;
            }
            if ((UBYTE)c == 255 && rpos < rlen) {   /* telnet IAC: drop the command */
                UBYTE k = (UBYTE)rbuf[rpos++];
                if (k >= 251 && rpos < rlen) rpos++;
                continue;
            }
            if (linelen < (LONG)sizeof(linebuf) - 1)
                linebuf[linelen++] = c;
        }
        {
            fd_set rd;
            struct timeval tv;
            ULONG sigs = SIGBREAKF_CTRL_C;
            FD_ZERO(&rd); FD_SET(ctrl, &rd);
            tv.tv_secs = idle_secs; tv.tv_micro = 0;
            if (WaitSelect(ctrl + 1, &rd, NULL, NULL, &tv, &sigs) <= 0) {
                if (!(sigs & SIGBREAKF_CTRL_C))
                    reply(421, "Idle for too long, closing.");
                return FALSE;
            }
        }
        rpos = 0;
        if ((rlen = recv(ctrl, rbuf, sizeof(rbuf), 0)) <= 0)
            return FALSE;
    }
}

/* ---- paths: the client's Unix view <-> Amiga paths ----------------------- */

/* make an absolute, normalised client path from cwd + arg (no . or .. left) */
static void vjoin(const char *arg, char *out)
{
    char tmp[PATHMAX * 2], *parts[64], *p;
    int n = 0, i;

    if (!arg || !*arg) arg = ".";
    if (arg[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", arg);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", vcwd, arg);
    for (p = strtok(tmp, "/"); p && n < 64; p = strtok(NULL, "/")) {
        if (!strcmp(p, ".") || !*p) continue;
        if (!strcmp(p, "..")) { if (n) n--; continue; }
        parts[n++] = p;
    }
    out[0] = 0;
    for (i = 0; i < n; i++) {
        if (strlen(out) + strlen(parts[i]) + 2 >= PATHMAX) break;
        strcat(out, "/");
        strcat(out, parts[i]);
    }
    if (!out[0]) strcpy(out, "/");
}

/* client path -> Amiga path. "" means the volume list (not a real directory). */
static void amiga_path(const char *v, char *out)
{
    const char *rest_;
    int n;

    if (anon) {
        strcpy(out, anonroot);
        if (strcmp(v, "/"))
            AddPart((STRPTR)out, (STRPTR)(v + 1), PATHMAX);
        return;
    }
    if (!strcmp(v, "/")) { out[0] = 0; return; }
    v++;                                    /* "Work/dir/file" */
    rest_ = strchr(v, '/');
    n = rest_ ? (int)(rest_ - v) : (int)strlen(v);
    if (n > PATHMAX - 3) n = PATHMAX - 3;
    memcpy(out, v, n);
    out[n++] = ':';
    out[n] = 0;
    if (rest_ && rest_[1])
        strncat(out, rest_ + 1, PATHMAX - n - 1);
}

/* an Amiga path (a home directory) -> client path */
static void client_path(const char *a, char *out)
{
    const char *colon = strchr(a, ':');
    if (!colon) { strcpy(out, "/"); return; }
    snprintf(out, PATHMAX, "/%.*s%s%s", (int)(colon - a), a, colon[1] ? "/" : "", colon + 1);
    if (out[strlen(out) - 1] == '/' && strlen(out) > 1) out[strlen(out) - 1] = 0;
}

static BOOL is_dir(const char *a)
{
    struct FileInfoBlock *fib;
    BPTR lk;
    BOOL d = FALSE;

    if (!a[0]) return TRUE;                 /* the volume list */
    if (!(lk = Lock((CONST_STRPTR)a, ACCESS_READ))) return FALSE;
    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (Examine(lk, fib)) d = fib->fib_DirEntryType > 0;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lk);
    return d;
}

/* ---- dates ---------------------------------------------------------------- */

static void ymd(LONG days, LONG *y, LONG *m, LONG *d)     /* days since 1.1.1978 */
{
    LONG z = days + 2922 + 719468, era, doe, yoe, doy, mp;  /* -> days since 1.3.0000 */
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = doy - (153 * mp + 2) / 5 + 1;
    *m  = mp < 10 ? mp + 3 : mp - 9;
    *y  = yoe + era * 400 + (*m <= 2);
}

static LONG today(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ds.ds_Days;
}

/* ---- data connections ---------------------------------------------------- */

static void close_pasv(void)
{
    if (pasv_fd >= 0) { CloseSocket(pasv_fd); pasv_fd = -1; }
}

/* the data connection for one transfer: accept the passive one or connect to PORT */
static LONG open_data(void)
{
    LONG fd = -1;

    if (pasv_fd >= 0) {
        fd_set rd;
        struct timeval tv;
        FD_ZERO(&rd); FD_SET(pasv_fd, &rd);
        tv.tv_secs = 30; tv.tv_micro = 0;
        if (WaitSelect(pasv_fd + 1, &rd, NULL, NULL, &tv, NULL) > 0)
            fd = accept(pasv_fd, NULL, NULL);
        close_pasv();
    } else if (have_port) {
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) >= 0 &&
            connect(fd, (struct sockaddr *)&port_sa, sizeof(port_sa)) < 0) {
            CloseSocket(fd);
            fd = -1;
        }
        have_port = FALSE;
    } else {
        reply(425, "Use PASV or PORT first.");
        return -1;
    }
    if (fd < 0)
        reply(425, "Can't open the data connection.");
    return fd;
}

static void do_pasv(BOOL extended)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    LONG port;
    ULONG a;

    close_pasv();
    have_port = FALSE;
    if ((pasv_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { reply(425, "No socket."); return; }
    for (port = pasv_lo; ; port++) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_len = sizeof(sa); sa.sin_family = AF_INET;
        sa.sin_port = pasv_lo ? (UWORD)port : 0;
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(pasv_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
        if (!pasv_lo || port >= pasv_hi) { close_pasv(); reply(425, "No free port."); return; }
    }
    listen(pasv_fd, 1);
    getsockname(pasv_fd, (struct sockaddr *)&sa, &len);
    port = sa.sin_port;
    if (extended) {
        reply(229, "Entering Extended Passive Mode (|||%ld|)", (long)port);
        return;
    }
    a = pasv_addr ? pasv_addr : local_ip;
    reply(227, "Entering Passive Mode (%lu,%lu,%lu,%lu,%ld,%ld)", (unsigned long)(a >> 24),
          (unsigned long)(a >> 16) & 255, (unsigned long)(a >> 8) & 255, (unsigned long)a & 255,
          (long)(port >> 8), (long)(port & 255));
}

static void do_port(const char *arg)
{
    unsigned long h[6];
    if (sscanf(arg, "%lu,%lu,%lu,%lu,%lu,%lu", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6) {
        reply(501, "Bad PORT.");
        return;
    }
    close_pasv();
    memset(&port_sa, 0, sizeof(port_sa));
    port_sa.sin_len = sizeof(port_sa); port_sa.sin_family = AF_INET;
    port_sa.sin_addr.s_addr = (h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
    port_sa.sin_port = (UWORD)((h[4] << 8) | h[5]);
    have_port = TRUE;
    reply(200, "PORT command successful.");
}

static void do_eprt(const char *arg)
{
    char d = arg[0], ip[64];
    unsigned long port;
    const char *p = arg + 1;
    int i;
    /* |1|132.235.1.2|6275| */
    if (!d || *p != '1' || p[1] != d) { reply(522, "Only IPv4 (1)."); return; }
    p += 2;
    for (i = 0; *p && *p != d && i < 63; ) ip[i++] = *p++;
    ip[i] = 0;
    if (*p != d || sscanf(p + 1, "%lu", &port) != 1) { reply(501, "Bad EPRT."); return; }
    close_pasv();
    memset(&port_sa, 0, sizeof(port_sa));
    port_sa.sin_len = sizeof(port_sa); port_sa.sin_family = AF_INET;
    port_sa.sin_addr.s_addr = inet_addr((STRPTR)ip);
    port_sa.sin_port = (UWORD)port;
    have_port = TRUE;
    reply(200, "EPRT command successful.");
}

/* ---- listings -------------------------------------------------------------- */

static void list_line(LONG fd, struct FileInfoBlock *fib, BOOL full)
{
    char line[400], when[16];
    LONG y, m, d;

    if (!full) {
        snprintf(line, sizeof(line), "%s\r\n", (char *)fib->fib_FileName);
        sendstr(fd, line);
        return;
    }
    ymd(fib->fib_Date.ds_Days, &y, &m, &d);
    if (today() - fib->fib_Date.ds_Days < 180)
        snprintf(when, sizeof(when), "%02ld:%02ld", (long)(fib->fib_Date.ds_Minute / 60),
                 (long)(fib->fib_Date.ds_Minute % 60));
    else
        snprintf(when, sizeof(when), " %ld", (long)y);
    snprintf(line, sizeof(line), "%c%s 1 %-8s %-8s %10lu %s %2ld %5s %s\r\n",
             fib->fib_DirEntryType > 0 ? 'd' : '-',
             (fib->fib_Protection & FIBF_WRITE) ? "r-xr-xr-x" : "rwxr-xr-x",
             anon ? "ftp" : user, anon ? "ftp" : user, (unsigned long)fib->fib_Size,
             month[(m - 1) % 12], (long)d, when, (char *)fib->fib_FileName);
    sendstr(fd, line);
}

/* "/" for a normal user: one directory entry per mounted volume */
static void list_volumes(LONG fd, BOOL full)
{
    struct DosList *dl = LockDosList(LDF_VOLUMES | LDF_READ);
    char line[160], name[64];

    while ((dl = NextDosEntry(dl, LDF_VOLUMES)) != NULL) {
        UBYTE *b = (UBYTE *)BADDR(dl->dol_Name);
        int n = b[0] < sizeof(name) - 1 ? b[0] : sizeof(name) - 1;
        if (!dl->dol_Task) continue;        /* not in a drive */
        memcpy(name, b + 1, n);
        name[n] = 0;
        if (full)
            snprintf(line, sizeof(line), "drwxr-xr-x 1 %-8s %-8s %10lu Jan  1  1978 %s\r\n",
                     user, user, 0UL, name);
        else
            snprintf(line, sizeof(line), "%s\r\n", name);
        sendstr(fd, line);
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
}

static void do_list(const char *arg, BOOL full)
{
    char v[PATHMAX], a[PATHMAX];
    struct FileInfoBlock *fib;
    BPTR lk = 0;
    LONG fd;

    while (arg && *arg == '-') {            /* "LIST -la": options from Unix clients */
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    vjoin(arg, v);
    amiga_path(v, a);
    if (a[0] && !(lk = Lock((CONST_STRPTR)a, ACCESS_READ))) {
        reply(550, "%s: no such file or directory.", v);
        return;
    }
    reply(150, "Here comes the listing.");
    if ((fd = open_data()) < 0) { if (lk) UnLock(lk); return; }
    if (!a[0]) {
        list_volumes(fd, full);
    } else if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (Examine(lk, fib)) {
            if (fib->fib_DirEntryType > 0) {
                while (ExNext(lk, fib))
                    list_line(fd, fib, full);
            } else {
                list_line(fd, fib, full);
            }
        }
        FreeDosObject(DOS_FIB, fib);
    }
    if (lk) UnLock(lk);
    CloseSocket(fd);
    reply(226, "Directory send OK.");
}

/* ---- transfers -------------------------------------------------------------- */

static void do_retr(const char *arg)
{
    char v[PATHMAX], a[PATHMAX];
    BPTR fh;
    LONG fd, n, total = 0;
    BOOL ok = TRUE;

    vjoin(arg, v);
    amiga_path(v, a);
    if (!a[0] || is_dir(a) || !(fh = Open((CONST_STRPTR)a, MODE_OLDFILE))) {
        reply(550, "%s: can't open it.", v);
        rest = 0;
        return;
    }
    if (rest) Seek(fh, rest, OFFSET_BEGINNING);
    rest = 0;
    reply(150, "Opening %s mode data connection for %s.", binary ? "BINARY" : "ASCII", v);
    if ((fd = open_data()) < 0) { Close(fh); return; }
    while ((n = Read(fh, xbuf, binary ? BUFSIZE : BUFSIZE / 2)) > 0) {
        LONG len = n, sent = 0;
        if (!binary) {                      /* LF -> CR LF, in place from the end */
            LONG i, lf = 0;
            for (i = 0; i < n; i++) if (xbuf[i] == '\n') lf++;
            len = n + lf;
            for (i = n - 1; i >= 0; i--) {
                xbuf[i + lf] = xbuf[i];
                if (xbuf[i] == '\n') { lf--; xbuf[i + lf] = '\r'; }
            }
        }
        while (sent < len) {
            LONG s = send(fd, xbuf + sent, len - sent, 0);
            if (s <= 0) { ok = FALSE; break; }
            sent += s;
        }
        if (!ok) break;
        total += n;
    }
    Close(fh);
    CloseSocket(fd);
    if (ok && n >= 0) reply(226, "Transfer complete (%ld bytes).", (long)total);
    else reply(426, "Connection closed; transfer aborted.");
}

static void do_stor(const char *arg, BOOL append)
{
    char v[PATHMAX], a[PATHMAX];
    BPTR fh;
    LONG fd, n, total = 0;
    BOOL ok = TRUE, cr = FALSE;

    if (readonly) { reply(550, "Read-only."); return; }
    vjoin(arg, v);
    amiga_path(v, a);
    if (!a[0] || is_dir(a)) { reply(553, "%s: not a file name.", v); return; }
    if (append || rest) {
        if (!(fh = Open((CONST_STRPTR)a, MODE_READWRITE))) { reply(553, "%s: can't open it.", v); return; }
        Seek(fh, rest ? (LONG)rest : 0, rest ? OFFSET_BEGINNING : OFFSET_END);
    } else if (!(fh = Open((CONST_STRPTR)a, MODE_NEWFILE))) {
        reply(553, "%s: can't create it.", v);
        return;
    }
    rest = 0;
    reply(150, "Ok to send data.");
    if ((fd = open_data()) < 0) { Close(fh); return; }
    while ((n = recv(fd, xbuf, BUFSIZE, 0)) > 0) {
        LONG len = n;
        if (!binary) {                      /* CR LF -> LF */
            LONG i, o = 0;
            for (i = 0; i < n; i++) {
                if (cr && xbuf[i] != '\n') xbuf[o++] = '\r';
                cr = xbuf[i] == '\r';
                if (!cr) xbuf[o++] = xbuf[i];
            }
            len = o;
        }
        if (Write(fh, xbuf, len) != len) { ok = FALSE; break; }
        total += len;
    }
    if (cr && ok) Write(fh, (APTR)"\r", 1);
    Close(fh);
    CloseSocket(fd);
    if (ok && n == 0) reply(226, "Transfer complete (%ld bytes).", (long)total);
    else reply(451, "Write failed (disk full?) or connection lost.");
}

/* ---- logging in -------------------------------------------------------------- */

static void set_home(const char *amiga_dir)
{
    if (amiga_dir && amiga_dir[0] && is_dir(amiga_dir))
        client_path(amiga_dir, vcwd);
    else
        strcpy(vcwd, "/");
}

static void do_pass(const char *pass)
{
    struct ug_passwd *pw;

    if (!user[0]) { reply(503, "Login with USER first."); return; }
    if (anon) {
        logged_in = TRUE;
        readonly = TRUE;
        strcpy(vcwd, "/");
        reply(230, "Anonymous access granted, read-only.");
        return;
    }
    pw = UserGroupBase ? (struct ug_passwd *)getpwnam((STRPTR)user) : NULL;
    if (pw && (!pw->pw_passwd || !pw->pw_passwd[0]))
        pw = NULL;                          /* no password: no network login */
    if (pw) {
        STRPTR c = crypt((STRPTR)pass, (STRPTR)pw->pw_passwd);
        if (!c || strcmp((char *)c, pw->pw_passwd))
            pw = NULL;
    }
    if (!pw) {
        Delay(100);                         /* slow down guessing */
        reply(530, "Login incorrect.");
        user[0] = 0;
        return;
    }
    logged_in = TRUE;
    set_home(pw->pw_dir);
    reply(230, "User %s logged in.", user);
}

/* ---- the command loop -------------------------------------------------------- */

static BOOL need_login(void)
{
    if (!logged_in) reply(530, "Please login with USER and PASS.");
    return !logged_in;
}

static BOOL need_write(void)
{
    if (readonly) reply(550, "Read-only.");
    return readonly;
}

int main(void)
{
    LONG args[6] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"SOCKET/K/N,ANONYMOUS/K,PASVPORTS/K,PASVADDRESS/K,READONLY/S,IDLE/K/N",
                                  args, NULL);
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);

    if (!rda || !args[0])
        return RETURN_FAIL;                 /* inetd starts us; no console to talk to */
    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if ((ctrl = ObtainSocket(*(LONG *)args[0], AF_INET, SOCK_STREAM, 0)) < 0) {
        CloseLibrary(SocketBase);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    UserGroupBase = OpenLibrary((CONST_STRPTR)"usergroup.library", 1);
    if (args[1]) strncpy(anonroot, (const char *)args[1], sizeof(anonroot) - 1);
    if (args[2]) {
        unsigned long lo = 0, hi = 0;
        if (sscanf((const char *)args[2], "%lu-%lu", &lo, &hi) == 2 && lo > 0 && hi >= lo && hi < 65536) {
            pasv_lo = (UWORD)lo; pasv_hi = (UWORD)hi;
        }
    }
    if (args[3]) pasv_addr = inet_addr((STRPTR)args[3]);
    readonly = args[4] != 0;
    if (args[5] && *(LONG *)args[5] > 0) idle_secs = *(LONG *)args[5];
    getsockname(ctrl, (struct sockaddr *)&sa, &len);
    local_ip = sa.sin_addr.s_addr;
    xbuf = AllocVec(BUFSIZE * 2, MEMF_ANY);

    if (!xbuf) {
        reply(421, "Out of memory.");
    } else {
        reply(220, "PrismNet FTP server ready.");
        while (get_line()) {
            char *cmd = linebuf, *arg = linebuf;
            int i;
            while (*arg && *arg != ' ') arg++;
            if (*arg) *arg++ = 0;
            for (i = 0; cmd[i]; i++) if (cmd[i] >= 'a' && cmd[i] <= 'z') cmd[i] -= 32;

            if (!strcmp(cmd, "USER")) {
                logged_in = FALSE;
                strncpy(user, arg, sizeof(user) - 1);
                anon = anonroot[0] && (!strcmp(arg, "anonymous") || !strcmp(arg, "ftp"));
                reply(331, anon ? "Anonymous login ok, send your e-mail as password."
                                : "Password required for %s.", user);
            } else if (!strcmp(cmd, "PASS")) {
                do_pass(arg);
            } else if (!strcmp(cmd, "QUIT")) {
                reply(221, "Goodbye.");
                break;
            } else if (!strcmp(cmd, "SYST")) {
                reply(215, "UNIX Type: L8");
            } else if (!strcmp(cmd, "FEAT")) {
                sendstr(ctrl, "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n PASV\r\n EPSV\r\n EPRT\r\n");
                reply(211, "End");
            } else if (!strcmp(cmd, "NOOP")) {
                reply(200, "NOOP ok.");
            } else if (!strcmp(cmd, "OPTS")) {
                reply(200, "OK.");
            } else if (need_login()) {
                continue;
            } else if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
                reply(257, "\"%s\" is the current directory.", vcwd);
            } else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD") || !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
                char v[PATHMAX], a[PATHMAX];
                BOOL up = !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP");
                vjoin(up ? ".." : arg, v);
                amiga_path(v, a);
                if (is_dir(a)) { strcpy(vcwd, v); reply(250, "Directory now \"%s\".", vcwd); }
                else reply(550, "%s: no such directory.", v);
            } else if (!strcmp(cmd, "TYPE")) {
                binary = !(arg[0] == 'A' || arg[0] == 'a');
                reply(200, "Type set to %s.", binary ? "I" : "A");
            } else if (!strcmp(cmd, "MODE")) {
                reply(arg[0] == 'S' || arg[0] == 's' ? 200 : 504, "Mode S only.");
            } else if (!strcmp(cmd, "STRU")) {
                reply(arg[0] == 'F' || arg[0] == 'f' ? 200 : 504, "Structure F only.");
            } else if (!strcmp(cmd, "PASV")) {
                do_pasv(FALSE);
            } else if (!strcmp(cmd, "EPSV")) {
                do_pasv(TRUE);
            } else if (!strcmp(cmd, "PORT")) {
                do_port(arg);
            } else if (!strcmp(cmd, "EPRT")) {
                do_eprt(arg);
            } else if (!strcmp(cmd, "LIST")) {
                do_list(arg, TRUE);
            } else if (!strcmp(cmd, "NLST")) {
                do_list(arg, FALSE);
            } else if (!strcmp(cmd, "REST")) {
                rest = strtoul(arg, NULL, 10);
                reply(350, "Restarting at %lu.", (unsigned long)rest);
            } else if (!strcmp(cmd, "RETR")) {
                do_retr(arg);
            } else if (!strcmp(cmd, "STOR")) {
                do_stor(arg, FALSE);
            } else if (!strcmp(cmd, "APPE")) {
                do_stor(arg, TRUE);
            } else if (!strcmp(cmd, "SIZE") || !strcmp(cmd, "MDTM")) {
                char v[PATHMAX], a[PATHMAX];
                struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
                BPTR lk;
                vjoin(arg, v);
                amiga_path(v, a);
                if (fib && a[0] && (lk = Lock((CONST_STRPTR)a, ACCESS_READ)) != 0) {
                    if (Examine(lk, fib) && fib->fib_DirEntryType < 0) {
                        if (cmd[0] == 'S') {
                            reply(213, "%lu", (unsigned long)fib->fib_Size);
                        } else {
                            LONG y, m, d, mi = fib->fib_Date.ds_Minute;
                            ymd(fib->fib_Date.ds_Days, &y, &m, &d);
                            reply(213, "%04ld%02ld%02ld%02ld%02ld%02ld", (long)y, (long)m, (long)d,
                                  (long)(mi / 60), (long)(mi % 60), (long)(fib->fib_Date.ds_Tick / 50));
                        }
                    } else {
                        reply(550, "%s: not a plain file.", v);
                    }
                    UnLock(lk);
                } else {
                    reply(550, "%s: no such file.", v);
                }
                if (fib) FreeDosObject(DOS_FIB, fib);
            } else if (!strcmp(cmd, "DELE")) {
                char v[PATHMAX], a[PATHMAX];
                if (need_write()) continue;
                vjoin(arg, v); amiga_path(v, a);
                if (a[0] && !is_dir(a) && DeleteFile((CONST_STRPTR)a)) reply(250, "Deleted %s.", v);
                else reply(550, "%s: can't delete it.", v);
            } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
                char v[PATHMAX], a[PATHMAX];
                if (need_write()) continue;
                vjoin(arg, v); amiga_path(v, a);
                if (a[0] && is_dir(a) && DeleteFile((CONST_STRPTR)a)) reply(250, "Removed %s.", v);
                else reply(550, "%s: can't remove it (not empty?).", v);
            } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
                char v[PATHMAX], a[PATHMAX];
                BPTR lk;
                if (need_write()) continue;
                vjoin(arg, v); amiga_path(v, a);
                if (a[0] && (lk = CreateDir((CONST_STRPTR)a)) != 0) { UnLock(lk); reply(257, "\"%s\" created.", v); }
                else reply(550, "%s: can't create it.", v);
            } else if (!strcmp(cmd, "RNFR")) {
                char v[PATHMAX];
                BPTR lk;
                if (need_write()) continue;
                vjoin(arg, v); amiga_path(v, rnfr);
                if (rnfr[0] && (lk = Lock((CONST_STRPTR)rnfr, ACCESS_READ)) != 0) { UnLock(lk); reply(350, "Ready for RNTO."); }
                else { rnfr[0] = 0; reply(550, "%s: no such file.", v); }
            } else if (!strcmp(cmd, "RNTO")) {
                char v[PATHMAX], a[PATHMAX];
                if (need_write()) continue;
                vjoin(arg, v); amiga_path(v, a);
                if (rnfr[0] && a[0] && Rename((CONST_STRPTR)rnfr, (CONST_STRPTR)a)) reply(250, "Renamed.");
                else reply(550, "Rename failed.");
                rnfr[0] = 0;
            } else if (!strcmp(cmd, "ABOR")) {
                close_pasv();
                reply(226, "Nothing to abort.");
            } else if (!strcmp(cmd, "ALLO")) {
                reply(202, "No need.");
            } else if (!strcmp(cmd, "STAT")) {
                reply(211, "PrismNet ftpd, %s, %s.", logged_in ? user : "not logged in", binary ? "binary" : "ASCII");
            } else if (!strcmp(cmd, "HELP")) {
                reply(214, "USER PASS QUIT PWD CWD CDUP TYPE PASV EPSV PORT EPRT LIST NLST RETR STOR APPE REST "
                           "SIZE MDTM DELE MKD RMD RNFR RNTO SYST FEAT NOOP STAT");
            } else {
                reply(502, "%s not implemented.", cmd);
            }
        }
    }
    close_pasv();
    if (xbuf) FreeVec(xbuf);
    CloseSocket(ctrl);
    if (UserGroupBase) CloseLibrary(UserGroupBase);
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return RETURN_OK;
}
