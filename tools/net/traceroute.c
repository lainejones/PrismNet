/*
 * traceroute - route tracing for any AmiTCP-compatible stack. Part of PrismNet.
 * GPL-3.0-only, see LICENSE.
 *
 *   traceroute HOST [MAXHOPS=n] [QUERIES=n] [WAIT=s] [UDP]
 *
 * Default: ICMP echo probes with rising TTL (like Windows tracert).
 * UDP: classic UDP probes to port 33434+ (answered by "port unreachable").
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#include "nettools.h"

static const char ver[] __attribute__((used)) = "$VER: traceroute 1.0 (16.10.2026) PrismNet";

struct Library *SocketBase;

#define TEMPLATE "HOST/A,MAXHOPS=M/K/N,QUERIES=Q/K/N,WAIT=W/K/N,UDP/S"
enum { A_HOST, A_MAXHOPS, A_QUERIES, A_WAIT, A_UDP, A_N };

#define BASEPORT 33434

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda;
    struct hostent *h;
    struct sockaddr_in to;
    LONG icmp, udp = -1, maxhops = 30, queries = 3, wait = 3, ttl;
    UWORD id = (UWORD)((ULONG)FindTask(NULL) >> 4), seq = 0;
    BOOL use_udp, reached = FALSE, stop = FALSE;
    char ip[20], t[16];
    int rc = RETURN_OK;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"traceroute");
        return RETURN_FAIL;
    }
    if (args[A_MAXHOPS]) maxhops = *(LONG *)args[A_MAXHOPS];
    if (args[A_QUERIES]) queries = *(LONG *)args[A_QUERIES];
    if (args[A_WAIT])    wait = *(LONG *)args[A_WAIT];
    use_udp = args[A_UDP] != 0;
    if (maxhops < 1 || maxhops > 64) maxhops = 30;
    if (wait < 1) wait = 1;
    if (queries < 1 || queries > 5) queries = 3;

    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("traceroute: no TCP/IP stack (bsdsocket.library)\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (!nt_clock_init()) { rc = RETURN_FAIL; goto out; }
    if (!(h = gethostbyname((STRPTR)args[A_HOST]))) {
        printf("traceroute: unknown host %s\n", (char *)args[A_HOST]);
        rc = RETURN_ERROR;
        goto out;
    }
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    memcpy(&to.sin_addr, h->h_addr_list[0], 4);
    nt_ipstr(to.sin_addr.s_addr, ip);

    if ((icmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
        printf("traceroute: cannot open an ICMP socket (errno %ld)\n", (long)Errno());
        rc = RETURN_FAIL;
        goto out;
    }
    if (use_udp && (udp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("traceroute: cannot open a UDP socket\n");
        rc = RETURN_FAIL;
        goto out2;
    }
    printf("traceroute to %s (%s), %ld hops max, %s probes\n", (char *)args[A_HOST], ip,
           (long)maxhops, use_udp ? "UDP" : "ICMP");

    for (ttl = 1; ttl <= maxhops && !reached && !stop; ttl++) {
        ULONG lasthop = 0;
        LONG q;

        printf("%2ld ", (long)ttl);
        Flush(Output());
        for (q = 0; q < queries && !stop; q++) {
            unsigned long long t0, deadline;
            UWORD myport = 0;
            BOOL got = FALSE;

            seq++;
            if (use_udp) {
                struct sockaddr_in u = to;
                UBYTE payload[12];
                memset(payload, 0, sizeof(payload));
                setsockopt(udp, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
                u.sin_port = BASEPORT + seq;
                t0 = nt_now_us();
                sendto(udp, payload, sizeof(payload), 0, (struct sockaddr *)&u, sizeof(u));
                {
                    struct sockaddr_in me;
                    socklen_t ml = sizeof(me);
                    if (getsockname(udp, (struct sockaddr *)&me, &ml) == 0) myport = me.sin_port;
                }
            } else {
                UBYTE pkt[8 + 32];
                LONG c;
                memset(pkt, 0, sizeof(pkt));
                pkt[0] = 8;
                pkt[4] = id >> 8; pkt[5] = id; pkt[6] = seq >> 8; pkt[7] = seq;
                c = nt_cksum(pkt, sizeof(pkt));
                pkt[2] = c >> 8; pkt[3] = c;
                setsockopt(icmp, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
                t0 = nt_now_us();
                sendto(icmp, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
            }
            deadline = t0 + (unsigned long long)wait * 1000000ULL;

            while (!got) {
                unsigned long long now = nt_now_us();
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                struct timeval tv;
                fd_set rd;
                ULONG sigs = SIGBREAKF_CTRL_C;
                static UBYTE buf[600];
                LONG n, hl, type, code, ih;

                if (now >= deadline) break;
                tv.tv_secs = (deadline - now) / 1000000;
                tv.tv_micro = (deadline - now) % 1000000;
                FD_ZERO(&rd);
                FD_SET(icmp, &rd);
                n = WaitSelect(icmp + 1, &rd, NULL, NULL, (struct __timeval *)&tv, &sigs);
                if (sigs & SIGBREAKF_CTRL_C) { stop = TRUE; break; }
                if (n <= 0) continue;
                n = recvfrom(icmp, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                if (n < 28) continue;
                hl = (buf[0] & 15) * 4;
                if (n < hl + 8) continue;
                type = buf[hl]; code = buf[hl + 1];
                if (type == 0 && !use_udp) {                       /* echo reply */
                    if (((buf[hl + 4] << 8) | buf[hl + 5]) != id || ((buf[hl + 6] << 8) | buf[hl + 7]) != seq)
                        continue;
                    reached = TRUE;
                } else if (type == 11 || type == 3) {            /* time exceeded / unreachable */
                    ih = hl + 8;                                 /* quoted original IP header */
                    if (n < ih + 20 + 8) continue;
                    {
                        LONG qhl = (buf[ih] & 15) * 4, qp = buf[ih + 9];
                        const UBYTE *o = buf + ih + qhl;
                        if (n < ih + qhl + 8) continue;     /* quoted header with options */
                        if (use_udp) {
                            /* source port and the probe's own destination port: a late
                             * answer to an earlier probe must not count for this one */
                            if (qp != IPPROTO_UDP || ((o[0] << 8) | o[1]) != myport ||
                                ((o[2] << 8) | o[3]) != (UWORD)(BASEPORT + seq)) continue;
                        } else {
                            if (qp != IPPROTO_ICMP || ((o[4] << 8) | o[5]) != id || ((o[6] << 8) | o[7]) != seq)
                                continue;
                        }
                    }
                    if (type == 3) reached = TRUE;   /* port/host unreachable: end of the line */
                    (void)code;
                } else {
                    continue;
                }
                got = TRUE;
                now = nt_now_us() - t0;
                if (from.sin_addr.s_addr != lasthop) {
                    char hip[20];
                    nt_ipstr(from.sin_addr.s_addr, hip);
                    printf(" %s ", hip);
                    lasthop = from.sin_addr.s_addr;
                }
                printf(" %s ms", nt_ms(now, t));
            }
            if (!got && !stop) printf(" *");
            Flush(Output());
        }
        printf("\n");
    }
    if (use_udp) CloseSocket(udp);
out2:
    CloseSocket(icmp);
out:
    nt_clock_exit();
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
