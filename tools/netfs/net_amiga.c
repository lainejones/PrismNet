/*
 * PrismNet NFS client: socket shim over bsdsocket.library.
 * The handler opens its own base (bases are per task) with net_up() and
 * drops it with net_down() when the stack has gone away (ENETDOWN), so the
 * next access opens it again - which starts PrismNet on demand.
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <hardware/custom.h>
#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#include "net.h"
#include "nbname.h"

struct Library *SocketBase;

#ifdef PNFS_DEBUG
void pnfs_dbg(const char *fmt, ...);
#define DBG(...) pnfs_dbg(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

#define E_INPROGRESS  36
#define E_ADDRINUSE   48
#define E_NETDOWN     50
#define E_NETUNREACH  51
#define E_CONNREFUSED 61
#define E_INTR        4

int net_up(void)
{
    if (!SocketBase)
        SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    return SocketBase ? 0 : NET_EDOWN;
}

void net_down(void)
{
    if (SocketBase)
        CloseLibrary(SocketBase);
    SocketBase = NULL;
}

static long err(void)
{
    long e = Errno();
    return e == E_NETDOWN ? NET_EDOWN : e == E_INTR ? NET_EBREAK :
           e == E_CONNREFUSED ? NET_EREFUSED : NET_ECLOSED;
}

static u32 nb_resolve(const char *name);

u32 net_resolve(const char *host)
{
    struct hostent *h;
    u32 ip;

    if (net_up())
        return 0;
    ip = inet_addr((STRPTR)host);
    if (ip != 0xffffffffUL)
        return ip;                   /* m68k: network order is host order */
    if (!(h = gethostbyname((STRPTR)host)) || h->h_length != 4)
        return nb_wanted(host) ? nb_resolve(host) : 0;
    memcpy(&ip, h->h_addr_list[0], 4);
    return ip;
}

/* wait until s is readable (rd) or writable; 1 ready, 0 timeout, < 0 error */
static long wait_sock(long s, int rd, unsigned timeout_s)
{
    fd_set set;
    struct timeval tv;
    long n;

    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    n = WaitSelect(s + 1, rd ? &set : NULL, rd ? NULL : &set, NULL,
                   (struct __timeval *)&tv, NULL);
    return n < 0 ? err() : n;
}

/* NetBIOS name query, broadcast on the LAN: three tries a second apart */
static u32 nb_resolve(const char *name)
{
    struct sockaddr_in sa;
    u8 q[64], r[576];
    long s, one = 1, n, tries;
    u32 id, ip = 0, qlen;

    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        return 0;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = 137;
    sa.sin_addr.s_addr = 0xffffffffUL;
    net_random(q, 2);
    id = (u32)q[0] << 8 | q[1];
    qlen = nb_query(q, name, 1, id);
    for (tries = 0; tries < 3 && !ip; tries++) {
        sendto(s, q, qlen, 0, (struct sockaddr *)&sa, sizeof(sa));
        while (!ip && wait_sock(s, 1, 1) > 0) {
            if ((n = recv(s, r, sizeof(r), 0)) <= 0)
                break;
            ip = nb_answer(r, (u32)n, id);
        }
    }
    CloseSocket(s);
    return ip;                       /* m68k: host order is network order */
}

long net_connect(u32 ip, unsigned port, int resvport, unsigned timeout_s)
{
    struct sockaddr_in sa;
    long s, one = 1, zero = 0, n;

    if (net_up())
        return NET_EDOWN;
    DBG("connect: socket");
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return err();
    DBG("connect: socket %ld, bind", s);
    if (resvport) {                  /* lets a "secure" export rule accept us */
        unsigned p;
        for (p = 1023; p >= 600; p--) {
            memset(&sa, 0, sizeof(sa));
            sa.sin_len = sizeof(sa);
            sa.sin_family = AF_INET;
            sa.sin_port = p;
            if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 || Errno() != E_ADDRINUSE)
                break;
        }
    }
    DBG("connect: nodelay");
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    DBG("connect: connect %lx:%ld", ip, (long)port);
    IoctlSocket(s, FIONBIO, (char *)&one);
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = port;
    sa.sin_addr.s_addr = ip;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        long e = Errno(), soerr = 0;
        socklen_t len = sizeof(soerr);

        if (e != E_INPROGRESS) {
            CloseSocket(s);
            return e == E_CONNREFUSED ? NET_EREFUSED : e == E_NETDOWN ? NET_EDOWN : NET_ETIMEDOUT;
        }
        n = wait_sock(s, 0, timeout_s);
        if (n <= 0) {
            CloseSocket(s);
            return n < 0 ? n : NET_ETIMEDOUT;
        }
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr) {
            CloseSocket(s);
            return soerr == E_CONNREFUSED ? NET_EREFUSED : NET_ETIMEDOUT;
        }
    }
    IoctlSocket(s, FIONBIO, (char *)&zero);
    DBG("connect: done");
    return s;
}

long net_send(long s, const u8 *buf, u32 len)
{
    while (len) {
        long n = send(s, (APTR)buf, len, 0);
        if (n <= 0)
            return n < 0 ? err() : NET_ECLOSED;
        buf += n;
        len -= n;
    }
    return 0;
}

long net_recv(long s, u8 *buf, u32 len, unsigned timeout_s)
{
    while (len) {
        long n = wait_sock(s, 1, timeout_s);

        if (n <= 0)
            return n < 0 ? n : NET_ETIMEDOUT;
        n = recv(s, buf, len, 0);
        if (n <= 0)
            return n < 0 ? err() : NET_ECLOSED;
        buf += n;
        len -= n;
    }
    return 0;
}

void net_close(long s)
{
    if (SocketBase)
        CloseSocket(s);
}

/* no hardware random source on a classic Amiga: mix the raster beam
 * position, the time and a running state through a simple LCG */
void net_random(u8 *buf, u32 n)
{
    static u32 state;
    volatile UWORD *vhposr = (volatile UWORD *)0xdff006;
    struct DateStamp ds;

    DateStamp(&ds);
    state ^= ((u32)ds.ds_Tick << 16) ^ ds.ds_Minute ^ ((u32)ds.ds_Days << 8);
    while (n--) {
        state = state * 1103515245UL + 12345UL + *vhposr;
        *buf++ = (u8)(state >> 16);
    }
}

/* the Amiga clock is local time; good enough where the server gives none */
u32 net_time(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (u32)ds.ds_Days * 86400UL + ds.ds_Minute * 60UL + ds.ds_Tick / 50 + 252460800UL;
}
