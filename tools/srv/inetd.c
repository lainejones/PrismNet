/*
 * inetd - PrismNet's service dispatcher.
 *
 * Listens on the ports listed in ENV:PrismNet/services and, for every
 * connection, starts that service's program as a process of its own and
 * hands it the connected socket: ReleaseSocket(fd, UNIQUE_ID) here, then
 * "<program> SOCKET=<id> <args>", and the program calls ObtainSocket(id).
 *
 *   ENV:PrismNet/services, one service a line ('#' starts a comment):
 *     <name> <port> tcp <program> [arguments...]
 *     ftp  21  tcp  SYS:PrismNet/Servers/ftpd PASVPORTS=50000-50019
 *
 *   inetd [FROM=file] [QUIET]          (CTRL-C stops it)
 *
 * Any bsdsocket.library will do. GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <bsdsocket/socketbasetags.h>
#include <proto/bsdsocket.h>

static const char version[] __attribute__((used)) = "$VER: inetd 1.0 (18.10.2026) PrismNet";

#define MAXSVC  16
#define DEFFILE "ENV:PrismNet/services"

struct Library *SocketBase;

struct Service {
    char  name[16];
    char  program[128];
    char  args[160];
    UWORD port;
    LONG  fd;
    ULONG started;
};

static struct Service svc[MAXSVC];
static int nsvc;
static BOOL quiet;

static void say(const char *fmt, const char *a, long n)
{
    if (!quiet) {
        printf("inetd: ");
        printf(fmt, a, n);
        printf("\n");
    }
}

/* split one line into words; returns the count */
static int words(char *s, char **w, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\n' || *s == '\r') break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static BOOL load(const char *file)
{
    char line[400];
    BPTR fh = Open((CONST_STRPTR)file, MODE_OLDFILE);
    if (!fh) {
        printf("inetd: cannot open %s\n", file);
        return FALSE;
    }
    while (FGets(fh, (STRPTR)line, sizeof(line)) && nsvc < MAXSVC) {
        char *w[24];
        int n = words(line, w, 24), i;
        struct Service *v = &svc[nsvc];
        if (n == 0)
            continue;
        if (n < 4 || atoi(w[1]) <= 0 || atoi(w[1]) > 65535 || strcmp(w[2], "tcp")) {
            printf("inetd: %s: skipped a line (want: name port tcp program [args])\n", file);
            continue;
        }
        memset(v, 0, sizeof(*v));
        strncpy(v->name, w[0], sizeof(v->name) - 1);
        v->port = (UWORD)atoi(w[1]);
        strncpy(v->program, w[3], sizeof(v->program) - 1);
        for (i = 4; i < n; i++) {
            if (strlen(v->args) + strlen(w[i]) + 2 >= sizeof(v->args))
                break;
            strcat(v->args, " ");
            strcat(v->args, w[i]);
        }
        v->fd = -1;
        nsvc++;
    }
    Close(fh);
    return TRUE;
}

static BOOL open_listener(struct Service *v)
{
    struct sockaddr_in sa;
    LONG one = 1;

    if ((v->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return FALSE;
    setsockopt(v->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = v->port;
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(v->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(v->fd, 16) < 0) {
        CloseSocket(v->fd);
        v->fd = -1;
        return FALSE;
    }
    return TRUE;
}

/* a connection on v: start its program with the socket */
static void dispatch(struct Service *v)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    LONG fd, id;
    char cmd[320];
    BPTR in, out;

    if ((fd = accept(v->fd, (struct sockaddr *)&peer, &len)) < 0)
        return;
    if ((id = ReleaseSocket(fd, UNIQUE_ID)) < 0) {
        CloseSocket(fd);
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s SOCKET=%ld%s", v->program, (long)id, v->args);
    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out ||
        SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_Name, (ULONG)v->name, NP_StackSize, 16384, NP_Priority, 0, TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        fd = ObtainSocket(id, AF_INET, SOCK_STREAM, 0);     /* take it back and drop it */
        if (fd >= 0)
            CloseSocket(fd);
        say("%s: cannot start the program (%ld)", v->name, (long)IoErr());
        return;
    }
    v->started++;
    if (!quiet) {
        ULONG a = peer.sin_addr.s_addr;
        printf("inetd: %s from %lu.%lu.%lu.%lu\n", v->name, (unsigned long)(a >> 24),
               (unsigned long)(a >> 16) & 255, (unsigned long)(a >> 8) & 255, (unsigned long)a & 255);
    }
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"FROM/K,QUIET/S", args, NULL);
    const char *from = DEFFILE;
    int i, up = 0;

    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"inetd");
        return RETURN_FAIL;
    }
    if (args[0]) from = (const char *)args[0];
    quiet = args[1] != 0;
    if (!load(from) || nsvc == 0) {
        if (nsvc == 0) printf("inetd: no services in %s\n", from);
        FreeArgs(rda);
        return RETURN_WARN;
    }
    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("inetd: no TCP/IP stack (bsdsocket.library)\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    for (i = 0; i < nsvc; i++) {
        if (open_listener(&svc[i])) {
            up++;
            say("%s on port %ld", svc[i].name, svc[i].port);
        } else {
            say("%s: port %ld is in use or refused", svc[i].name, svc[i].port);
        }
    }
    while (up) {
        fd_set rd;
        LONG maxfd = -1, n;
        ULONG sigs = SIGBREAKF_CTRL_C;

        FD_ZERO(&rd);
        for (i = 0; i < nsvc; i++)
            if (svc[i].fd >= 0) {
                FD_SET(svc[i].fd, &rd);
                if (svc[i].fd > maxfd) maxfd = svc[i].fd;
            }
        n = WaitSelect(maxfd + 1, &rd, NULL, NULL, NULL, &sigs);
        if (sigs & SIGBREAKF_CTRL_C)
            break;
        if (n < 0) {                    /* the stack went away (PrismNet STOP) */
            say("the TCP/IP stack stopped%s", "", 0);
            break;
        }
        if (n == 0)
            continue;
        for (i = 0; i < nsvc; i++)
            if (svc[i].fd >= 0 && FD_ISSET(svc[i].fd, &rd))
                dispatch(&svc[i]);
    }
    for (i = 0; i < nsvc; i++)
        if (svc[i].fd >= 0)
            CloseSocket(svc[i].fd);
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    say("stopped%s", "", 0);
    return RETURN_OK;
}
