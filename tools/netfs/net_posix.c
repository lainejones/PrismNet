/*
 * PrismNet NFS client: socket shim for host tests on Linux/WSL.
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <time.h>
#include "net.h"
#include "nbname.h"

int net_up(void) { return 0; }
void net_down(void) { }

/* NetBIOS name query: broadcast, or to PNFS_NBNS=<address> (host tests:
 * WSL sits behind NAT, so its broadcasts never reach the LAN) */
static u32 nb_resolve(const char *name)
{
    const char *srv = getenv("PNFS_NBNS");
    struct sockaddr_in sa;
    unsigned char q[64], r[576];
    int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1, tries;
    u32 id = ((u32)getpid() ^ (u32)time(NULL)) & 0xffff, ip = 0, qlen;

    if (s < 0)
        return 0;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(137);
    sa.sin_addr.s_addr = srv ? inet_addr(srv) : htonl(INADDR_BROADCAST);
    qlen = nb_query(q, name, !srv, id);
    for (tries = 0; tries < 3 && !ip; tries++) {
        struct pollfd pf;
        sendto(s, q, qlen, 0, (struct sockaddr *)&sa, sizeof(sa));
        pf.fd = s; pf.events = POLLIN;
        while (!ip && poll(&pf, 1, 1000) > 0) {
            long n = recv(s, r, sizeof(r), 0);
            if (n <= 0) break;
            ip = nb_answer(r, (u32)n, id);
        }
    }
    close(s);
    return ip;
}

u32 net_resolve(const char *host)
{
    struct in_addr a;
    struct hostent *h;

    if (inet_aton(host, &a))
        return ntohl(a.s_addr);
    if (getenv("PNFS_NBNS") && nb_wanted(host))
        return nb_resolve(host);    /* test the NetBIOS path even when DNS knows */
    if (!(h = gethostbyname(host)) || h->h_addrtype != AF_INET)
        return nb_wanted(host) ? nb_resolve(host) : 0;
    memcpy(&a, h->h_addr_list[0], 4);
    return ntohl(a.s_addr);
}

long net_connect(u32 ip, unsigned port, int resvport, unsigned timeout_s)
{
    struct sockaddr_in sa;
    int s = socket(AF_INET, SOCK_STREAM, 0);

    (void)timeout_s;
    if (s < 0)
        return NET_EDOWN;
    if (resvport) {
        unsigned p;
        for (p = 1023; p >= 600; p--) {
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons((unsigned short)p);
            if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0)
                break;
            if (errno == EACCES)
                break;               /* not root: fall back to any port */
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl((uint32_t)ip);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = errno;
        close(s);
        return e == ECONNREFUSED ? NET_EREFUSED : NET_ETIMEDOUT;
    }
    return s;
}

long net_send(long s, const u8 *buf, u32 len)
{
    while (len) {
        ssize_t n = send((int)s, buf, len, 0);
        if (n <= 0)
            return NET_ECLOSED;
        buf += n;
        len -= (u32)n;
    }
    return 0;
}

long net_recv(long s, u8 *buf, u32 len, unsigned timeout_s)
{
    while (len) {
        struct pollfd p;
        ssize_t n;

        p.fd = (int)s;
        p.events = POLLIN;
        if (poll(&p, 1, (int)timeout_s * 1000) <= 0)
            return NET_ETIMEDOUT;
        n = recv((int)s, buf, len, 0);
        if (n <= 0)
            return NET_ECLOSED;
        buf += n;
        len -= (u32)n;
    }
    return 0;
}

void net_close(long s)
{
    close((int)s);
}

void net_random(u8 *buf, u32 n)
{
    static int seeded;
    if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); seeded = 1; }
    while (n--) *buf++ = (u8)(rand() >> 7);
}

u32 net_time(void)
{
    return (u32)time(NULL);
}
