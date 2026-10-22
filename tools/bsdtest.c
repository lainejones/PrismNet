/*
 * bsdtest - PrismNet test client for bsdsocket.library.
 * GPL-3.0-only, see LICENSE.
 *
 * Uses only the standard AmiTCP API (NDK headers), so it runs against any
 * stack. Exercises: SocketBaseTags errno/h_errno, gethostbyname, blocking
 * TCP connect + HTTP GET with WaitSelect, non-blocking connect, a loopback
 * listen/accept/echo server, UDP to self, error paths.
 *
 *   bsdtest [HOST=aminet.net] [PATH=/] [PORT=80]
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <bsdsocket/socketbasetags.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

struct Library *SocketBase;
static LONG my_errno, my_herrno;
static int fails, passes;

#define CHECK(cond, ...) do { if (cond) { passes++; } else { fails++; printf("  FAIL: "); \
                         printf(__VA_ARGS__); printf(" (errno %ld)\n", (long)my_errno); } } while (0)

static void test_resolver(const char *host, struct sockaddr_in *sa, int port)
{
    struct hostent *h;

    printf("[resolver]\n");
    h = gethostbyname((STRPTR)host);
    CHECK(h != NULL, "gethostbyname(%s) h_errno %ld", host, (long)my_herrno);
    memset(sa, 0, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = AF_INET;
    sa->sin_port = port;
    if (h) {
        memcpy(&sa->sin_addr, h->h_addr_list[0], 4);
        printf("  %s -> %s\n", host, Inet_NtoA(sa->sin_addr.s_addr));
    }
    h = gethostbyname((STRPTR)"no-such-host.invalid");
    CHECK(h == NULL && my_herrno != 0, "bad name should fail with h_errno");
    h = gethostbyname((STRPTR)"10.1.2.3");
    CHECK(h && *(ULONG *)h->h_addr_list[0] == 0x0a010203UL, "numeric gethostbyname");
    CHECK(inet_addr((STRPTR)"192.168.1.1") == 0xc0a80101UL, "inet_addr");
    CHECK(getservbyname((STRPTR)"http", (STRPTR)"tcp") != NULL, "getservbyname http");
}

static void test_http(struct sockaddr_in *sa, const char *host, const char *path)
{
    char req[256], buf[1024], status[80];
    LONG s, n, total = 0, t0, ticks;
    fd_set rd;
    struct timeval tv;
    int got_status = 0;

    printf("[tcp client] GET http://%s%s\n", host, path);
    s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket");
    if (s < 0) return;
    t0 = 0;
    {
        struct DateStamp ds;
        DateStamp(&ds);
        t0 = ds.ds_Minute * 3000 + ds.ds_Tick;
    }
    n = connect(s, (struct sockaddr *)sa, sizeof(*sa));
    CHECK(n == 0, "connect");
    if (n == 0) {
        sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PrismNet-bsdtest/0.2\r\n"
                     "Connection: close\r\n\r\n", path, host);
        n = send(s, (UBYTE *)req, strlen(req), 0);
        CHECK(n == (LONG)strlen(req), "send");
        for (;;) {
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            tv.tv_secs = 10;
            tv.tv_micro = 0;
            n = WaitSelect(s + 1, &rd, NULL, NULL, (struct __timeval *)&tv, NULL);
            if (n <= 0) {
                CHECK(0, "WaitSelect returned %ld", (long)n);
                break;
            }
            n = recv(s, (UBYTE *)buf, sizeof(buf), 0);
            if (n <= 0) {
                CHECK(n == 0, "recv end");
                break;
            }
            if (!got_status) {
                int i;
                for (i = 0; i < n && i < (int)sizeof(status) - 1 && buf[i] != '\r'; i++)
                    status[i] = buf[i];
                status[i] = 0;
                got_status = 1;
            }
            total += n;
        }
        {
            struct DateStamp ds;
            DateStamp(&ds);
            ticks = ds.ds_Minute * 3000 + ds.ds_Tick - t0;
        }
        printf("  status: %s\n  %ld bytes in %ld ms\n", got_status ? status : "(none)",
               (long)total, (long)(ticks * 20));
        CHECK(got_status && strstr(status, "200") != NULL, "HTTP 200");
        CHECK(total > 1000, "received data");
    }
    CHECK(CloseSocket(s) == 0, "CloseSocket");
    CHECK(CloseSocket(s) == -1 && my_errno == EBADF, "double close -> EBADF");
}

static void test_nonblocking(struct sockaddr_in *sa)
{
    LONG s, one = 1, n, err = 0;
    socklen_t len = sizeof(err);
    fd_set wr;
    struct timeval tv;

    printf("[non-blocking connect]\n");
    s = socket(AF_INET, SOCK_STREAM, 0);
    IoctlSocket(s, FIONBIO, (char *)&one);
    n = connect(s, (struct sockaddr *)sa, sizeof(*sa));
    CHECK(n == -1 && my_errno == EINPROGRESS, "connect -> EINPROGRESS");
    FD_ZERO(&wr);
    FD_SET(s, &wr);
    tv.tv_secs = 10;
    tv.tv_micro = 0;
    n = WaitSelect(s + 1, NULL, &wr, NULL, (struct __timeval *)&tv, NULL);
    CHECK(n == 1 && FD_ISSET(s, &wr), "WaitSelect writable");
    n = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
    CHECK(n == 0 && err == 0, "SO_ERROR == 0");
    n = recv(s, (UBYTE *)&err, 4, 0);
    CHECK(n == -1 && my_errno == EWOULDBLOCK, "recv -> EWOULDBLOCK");
    CloseSocket(s);
}

static void test_loopback(void)
{
    struct sockaddr_in sa, peer;
    LONG ls, cs, as, n, one = 1;
    socklen_t len;
    char buf[64];

    printf("[loopback server]\n");
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = 7777;
    sa.sin_addr.s_addr = 0x7f000001UL;
    CHECK(bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0, "bind 127.0.0.1:7777");
    CHECK(listen(ls, 4) == 0, "listen");

    cs = socket(AF_INET, SOCK_STREAM, 0);
    IoctlSocket(cs, FIONBIO, (char *)&one);
    my_errno = 0;
    n = connect(cs, (struct sockaddr *)&sa, sizeof(sa));
    CHECK(n == 0 || (n == -1 && my_errno == EINPROGRESS), "connect to self");

    len = sizeof(peer);
    as = accept(ls, (struct sockaddr *)&peer, &len);
    CHECK(as >= 0, "accept");
    CHECK(len == sizeof(peer) && peer.sin_addr.s_addr == 0x7f000001UL, "accept peer address");
    if (as >= 0) {
        one = 0;
        IoctlSocket(cs, FIONBIO, (char *)&one);
        CHECK(send(cs, (UBYTE *)"hello prismnet", 14, 0) == 14, "client send");
        n = recv(as, (UBYTE *)buf, sizeof(buf), 0);
        CHECK(n == 14 && !memcmp(buf, "hello prismnet", 14), "server recv");
        CHECK(send(as, (UBYTE *)buf, n, 0) == n, "server echo");
        n = recv(cs, (UBYTE *)buf, sizeof(buf), 0);
        CHECK(n == 14, "client recv echo");
        len = sizeof(peer);
        CHECK(getpeername(cs, (struct sockaddr *)&peer, &len) == 0 && peer.sin_port == 7777,
              "getpeername");
        CloseSocket(as);
        n = recv(cs, (UBYTE *)buf, sizeof(buf), 0);
        CHECK(n == 0, "EOF after peer close");
    }
    CloseSocket(cs);
    CloseSocket(ls);
}

static void test_udp(void)
{
    struct sockaddr_in sa, from;
    LONG s, n;
    socklen_t len;
    char buf[32];

    printf("[udp loopback]\n");
    s = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = 7778;
    sa.sin_addr.s_addr = 0x7f000001UL;
    CHECK(bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0, "udp bind");
    n = sendto(s, (UBYTE *)"datagram", 8, 0, (struct sockaddr *)&sa, sizeof(sa));
    CHECK(n == 8, "sendto self");
    len = sizeof(from);
    n = recvfrom(s, (UBYTE *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
    CHECK(n == 8 && !memcmp(buf, "datagram", 8) && from.sin_port == 7778, "recvfrom self");
    CloseSocket(s);
}

static void test_addrinfo(const char *host, ULONG expect_addr)
{
    struct addrinfo hints, *res = NULL, *ai;
    struct sockaddr_in *sin, sa;
    char h[64], s[32];
    LONG e, n;

    printf("[getaddrinfo/getnameinfo]\n");
    /* Roadshow's newer calls (freeaddrinfo -804 ... getnameinfo -822): a
     * stack whose library stops before them - the classic AmiTCP set, e.g.
     * WinUAE's emulated bsdsocket.library - has no such entries, and the
     * call jumped past the end of its table (an illegal instruction) */
    if (SocketBase->lib_NegSize < 822) {
        printf("  skipped: this bsdsocket.library has no getaddrinfo/getnameinfo\n");
        return;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    e = getaddrinfo((STRPTR)host, (STRPTR)"http", &hints, &res);
    CHECK(e == 0 && res, "getaddrinfo(%s, http) -> %ld", host, (long)e);
    if (e == 0 && res) {
        sin = (struct sockaddr_in *)res->ai_addr;
        CHECK(res->ai_family == AF_INET && res->ai_socktype == SOCK_STREAM &&
              res->ai_protocol == IPPROTO_TCP && res->ai_addrlen == sizeof(*sin), "addrinfo fields");
        CHECK(sin->sin_port == 80 && (!expect_addr || sin->sin_addr.s_addr == expect_addr),
              "addrinfo address/port");
        CHECK(res->ai_canonname != NULL, "AI_CANONNAME");
        freeaddrinfo(res);
    }
    res = NULL;
    e = getaddrinfo((STRPTR)"10.1.2.3", (STRPTR)"8080", NULL, &res);
    for (n = 0, ai = res; ai; ai = ai->ai_next) n++;
    CHECK(e == 0 && n == 2, "numeric host, default socktypes -> 2 results (got %ld)", (long)n);
    if (res) freeaddrinfo(res);
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_DGRAM;
    res = NULL;
    e = getaddrinfo(NULL, (STRPTR)"53", &hints, &res);
    CHECK(e == 0 && res && ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr == 0 &&
          res->ai_protocol == IPPROTO_UDP, "AI_PASSIVE -> INADDR_ANY/udp");
    if (res) freeaddrinfo(res);
    res = NULL;
    CHECK(getaddrinfo((STRPTR)host, (STRPTR)"no-such-service", NULL, &res) == EAI_SERVICE,
          "unknown service -> EAI_SERVICE");
    CHECK(getaddrinfo((STRPTR)"no-such-host.invalid", NULL, NULL, &res) == EAI_NONAME,
          "unknown host -> EAI_NONAME");
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_NUMERICHOST;
    CHECK(getaddrinfo((STRPTR)"aminet.net", NULL, &hints, &res) == EAI_NONAME,
          "AI_NUMERICHOST with a name -> EAI_NONAME");
    CHECK(gai_strerror(EAI_NONAME) != NULL, "gai_strerror");

    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = 80;
    sa.sin_addr.s_addr = 0x7f000001UL;
    e = getnameinfo((struct sockaddr *)&sa, sizeof(sa), (STRPTR)h, sizeof(h), (STRPTR)s, sizeof(s), 0);
    CHECK(e == 0 && !strcmp(h, "localhost") && !strcmp(s, "http"), "getnameinfo 127.0.0.1:80 -> %s:%s", h, s);
    e = getnameinfo((struct sockaddr *)&sa, sizeof(sa), (STRPTR)h, sizeof(h), (STRPTR)s, sizeof(s),
                    NI_NUMERICHOST | NI_NUMERICSERV);
    CHECK(e == 0 && !strcmp(h, "127.0.0.1") && !strcmp(s, "80"), "getnameinfo numeric");
    sa.sin_addr.s_addr = 0x0a010203UL;
    CHECK(getnameinfo((struct sockaddr *)&sa, sizeof(sa), (STRPTR)h, sizeof(h), NULL, 0, NI_NAMEREQD)
          == EAI_NONAME, "getnameinfo NI_NAMEREQD without reverse DNS -> EAI_NONAME");
}

static BOOL wait_signal(ULONG mask, int ms)
{
    int i;
    for (i = 0; i < ms / 20; i++) {
        if (SetSignal(0, 0) & mask) {
            SetSignal(0, mask);
            return TRUE;
        }
        Delay(1);
    }
    return FALSE;
}

static void test_msg_and_events(void)
{
    struct sockaddr_in sa, from;
    struct msghdr mh;
    struct iovec iov[3];
    LONG ls, cs, as, n, one = 1, sig;
    ULONG mask, evsig;
    char a[4], b2[32];

    printf("[sendmsg/recvmsg + socket events]\n");
    sig = AllocSignal(-1);
    if (sig < 0) { CHECK(0, "AllocSignal"); return; }
    evsig = 1UL << sig;
    CHECK(SocketBaseTags(SBTM_SETVAL(SBTC_SIGEVENTMASK), evsig, TAG_END) == 0, "SBTC_SIGEVENTMASK");

    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = 7779;
    sa.sin_addr.s_addr = 0x7f000001UL;
    bind(ls, (struct sockaddr *)&sa, sizeof(sa));
    listen(ls, 2);
    mask = FD_ACCEPT;
    CHECK(setsockopt(ls, SOL_SOCKET, SO_EVENTMASK, &mask, sizeof(mask)) == 0, "SO_EVENTMASK");

    cs = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(cs, (struct sockaddr *)&sa, sizeof(sa)) == 0, "connect");
    CHECK(wait_signal(evsig, 3000), "event signal for FD_ACCEPT");
    mask = 0;
    n = GetSocketEvents(&mask);
    CHECK(n == ls && (mask & FD_ACCEPT), "GetSocketEvents -> listener, FD_ACCEPT (fd %ld mask %lx)",
          (long)n, (unsigned long)mask);
    CHECK(GetSocketEvents(&mask) == -1, "no further events");

    as = accept(ls, NULL, NULL);
    CHECK(as >= 0, "accept");
    mask = FD_READ | FD_CLOSE;
    setsockopt(as, SOL_SOCKET, SO_EVENTMASK, &mask, sizeof(mask));

    /* sendmsg with 3 pieces -> recvmsg into 2 */
    iov[0].iov_base = "hel"; iov[0].iov_len = 3;
    iov[1].iov_base = "lo ";  iov[1].iov_len = 3;
    iov[2].iov_base = "iov";  iov[2].iov_len = 3;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 3;
    CHECK(sendmsg(cs, &mh, 0) == 9, "sendmsg 3 iovecs");
    CHECK(wait_signal(evsig, 3000), "event signal for FD_READ");
    mask = 0;
    CHECK(GetSocketEvents(&mask) == as && (mask & FD_READ), "FD_READ on accepted socket");
    iov[0].iov_base = a;  iov[0].iov_len = 4;
    iov[1].iov_base = b2; iov[1].iov_len = sizeof(b2);
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    n = recvmsg(as, &mh, 0);
    CHECK(n == 9 && !memcmp(a, "hell", 4) && !memcmp(b2, "o iov", 5), "recvmsg scatter (%ld)", (long)n);

    CloseSocket(cs);
    CHECK(wait_signal(evsig, 3000), "event signal for FD_CLOSE");
    mask = 0;
    CHECK(GetSocketEvents(&mask) == as && (mask & FD_CLOSE), "FD_CLOSE after peer close");
    CloseSocket(as);
    CloseSocket(ls);

    /* recvmsg on UDP fills msg_name */
    ls = socket(AF_INET, SOCK_DGRAM, 0);
    sa.sin_port = 7780;
    bind(ls, (struct sockaddr *)&sa, sizeof(sa));
    sendto(ls, (UBYTE *)"udpmsg", 6, 0, (struct sockaddr *)&sa, sizeof(sa));
    iov[0].iov_base = b2; iov[0].iov_len = sizeof(b2);
    memset(&mh, 0, sizeof(mh));
    memset(&from, 0, sizeof(from));
    mh.msg_name = &from;
    mh.msg_namelen = sizeof(from);
    mh.msg_iov = iov;
    mh.msg_iovlen = 1;
    n = recvmsg(ls, &mh, 0);
    CHECK(n == 6 && from.sin_port == 7780 && mh.msg_namelen == sizeof(from), "UDP recvmsg + msg_name");
    CloseSocket(ls);

    SocketBaseTags(SBTM_SETVAL(SBTC_SIGEVENTMASK), 0, TAG_END);
    FreeSignal(sig);
}

int main(void)
{
    LONG args[6] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"HOST/K,PATH/K,PORT/K/N,LOOKUP/K,EXPECT/K,REVERSE/S", args, NULL);
    const char *host = args[0] ? (const char *)args[0] : "aminet.net";
    const char *path = args[1] ? (const char *)args[1] : "/";
    int port = args[2] ? (int)*(LONG *)args[2] : 80;
    struct sockaddr_in sa;
    char hn[64];

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        printf("bsdtest: cannot open bsdsocket.library 4 (is the stack running?)\n");
        return RETURN_FAIL;
    }
    printf("bsdtest: %s\n", (char *)SocketBase->lib_IdString);
    if (SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(my_errno))), (ULONG)&my_errno,
                       SBTM_SETVAL(SBTC_HERRNOLONGPTR), (ULONG)&my_herrno,
                       SBTM_SETVAL(SBTC_LOGTAGPTR), (ULONG)"bsdtest",
                       TAG_END) != 0)
        printf("  FAIL: SocketBaseTags\n"), fails++;
    gethostname((STRPTR)hn, sizeof(hn));
    printf("  hostname %s, dtablesize %ld\n", hn, (long)getdtablesize());

    test_resolver(host, &sa, port);
    if (sa.sin_addr.s_addr) {
        test_http(&sa, host, path);
        test_nonblocking(&sa);
    }
    if (args[3]) {                      /* LOOKUP=name EXPECT=ip [REVERSE] */
        struct hostent *h = gethostbyname((STRPTR)args[3]);
        ULONG want = args[4] ? inet_addr((STRPTR)args[4]) : 0;
        printf("[lookup %s]\n", (char *)args[3]);
        CHECK(h != NULL, "gethostbyname(%s)", (char *)args[3]);
        if (h) {
            printf("  -> %s (h_name %s)\n", Inet_NtoA(*(ULONG *)h->h_addr_list[0]), h->h_name);
            if (want) CHECK(*(ULONG *)h->h_addr_list[0] == want, "address is %s", (char *)args[4]);
        }
        if (args[5] && want) {
            h = gethostbyaddr((STRPTR)&want, 4, AF_INET);
            CHECK(h != NULL, "gethostbyaddr(%s)", (char *)args[4]);
            if (h) printf("  reverse %s -> %s\n", (char *)args[4], h->h_name);
        }
    }
    test_addrinfo(host, sa.sin_addr.s_addr);
    test_loopback();
    test_udp();
    test_msg_and_events();

    CloseLibrary(SocketBase);
    if (rda) FreeArgs(rda);
    printf("bsdtest: %d passed, %d failed -> %s\n", passes, fails, fails ? "FAIL" : "PASS");
    return fails ? RETURN_WARN : RETURN_OK;
}
