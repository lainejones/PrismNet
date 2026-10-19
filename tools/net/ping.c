/*
 * ping - ICMP echo for any AmiTCP-compatible stack. Part of PrismNet.
 * GPL-3.0-only, see LICENSE.
 *
 *   ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=s] [WAIT=s] [TTL=n] [QUIET]
 *
 * COUNT=0 pings until Ctrl-C. Default 4 pings of 56 data bytes, 1 s apart.
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

static const char ver[] __attribute__((used)) = "$VER: ping 1.0 (16.10.2026) PrismNet";

struct Library *SocketBase;

#define TEMPLATE "HOST/A,COUNT=C/K/N,SIZE=S/K/N,INTERVAL=I/K/N,WAIT=W/K/N,TTL/K/N,QUIET=Q/S"
enum { A_HOST, A_COUNT, A_SIZE, A_INTERVAL, A_WAIT, A_TTL, A_QUIET, A_N };

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda;
    struct hostent *h;
    struct sockaddr_in to, from;
    LONG s, count = 4, size = 56, interval = 1, wait = 2, sent = 0, recvd = 0, seq;
    static UBYTE pkt[1500], buf[1600];
    UWORD id = (UWORD)((ULONG)FindTask(NULL) >> 4);
    unsigned long long tmin = ~0ULL, tmax = 0, tsum = 0;
    char ip[20], t1[16], t2[16], t3[16];
    BOOL quiet, stop = FALSE;
    int rc = RETURN_OK;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"ping");
        return RETURN_FAIL;
    }
    if (args[A_COUNT])    count = *(LONG *)args[A_COUNT];
    if (args[A_SIZE])     size = *(LONG *)args[A_SIZE];
    if (args[A_INTERVAL]) interval = *(LONG *)args[A_INTERVAL];
    if (args[A_WAIT])     wait = *(LONG *)args[A_WAIT];
    quiet = args[A_QUIET] != 0;
    if (size < 8) size = 8;
    if (size > 1400) size = 1400;
    if (interval < 1) interval = 1;
    if (wait < 1) wait = 1;

    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("ping: no TCP/IP stack (bsdsocket.library)\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (!nt_clock_init()) {
        printf("ping: cannot open timer.device\n");
        rc = RETURN_FAIL;
        goto out;
    }
    h = gethostbyname((STRPTR)args[A_HOST]);
    if (!h) {
        printf("ping: unknown host %s\n", (char *)args[A_HOST]);
        rc = RETURN_ERROR;
        goto out;
    }
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    memcpy(&to.sin_addr, h->h_addr_list[0], 4);
    nt_ipstr(to.sin_addr.s_addr, ip);

    s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) {
        printf("ping: cannot open an ICMP socket (errno %ld)\n", (long)Errno());
        rc = RETURN_FAIL;
        goto out;
    }
    if (args[A_TTL]) {
        LONG ttl = *(LONG *)args[A_TTL];
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
    }
    printf("PING %s (%s): %ld data bytes\n", (char *)args[A_HOST], ip, (long)size);

    for (seq = 1; !stop && (count == 0 || seq <= count); seq++) {
        unsigned long long t0, deadline, next;
        LONG i;

        /* ICMP echo: type 8, code 0, cksum, id, seq, then a timestamp + pattern */
        memset(pkt, 0, 8 + size);
        pkt[0] = 8;
        pkt[4] = id >> 8; pkt[5] = id; pkt[6] = seq >> 8; pkt[7] = seq;
        for (i = 8; i < 8 + size; i++) pkt[i] = (UBYTE)i;
        t0 = nt_now_us();
        memcpy(pkt + 8, &t0, size >= 8 ? 8 : size);
        i = nt_cksum(pkt, 8 + size);
        pkt[2] = i >> 8; pkt[3] = i;
        if (sendto(s, pkt, 8 + size, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            printf("ping: sendto failed (errno %ld)\n", (long)Errno());
            rc = RETURN_WARN;
            break;
        }
        sent++;
        deadline = t0 + (unsigned long long)wait * 1000000ULL;
        next = t0 + (unsigned long long)interval * 1000000ULL;

        for (;;) {                          /* wait for our reply or the timeout */
            unsigned long long now = nt_now_us();
            struct timeval tv;
            fd_set rd;
            ULONG sigs = SIGBREAKF_CTRL_C;
            LONG n, hl;
            socklen_t fl = sizeof(from);

            if (now >= deadline) {
                if (!quiet) printf("Request timeout for icmp_seq %ld\n", (long)seq);
                break;
            }
            tv.tv_secs = (deadline - now) / 1000000;
            tv.tv_micro = (deadline - now) % 1000000;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            n = WaitSelect(s + 1, &rd, NULL, NULL, (struct __timeval *)&tv, &sigs);
            if (sigs & SIGBREAKF_CTRL_C) { stop = TRUE; break; }
            if (n <= 0) continue;
            n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n < 20) continue;
            hl = (buf[0] & 15) * 4;         /* raw sockets deliver the IP header */
            if (n < hl + 8 || buf[hl] != 0) continue;            /* echo reply only */
            if (((buf[hl + 4] << 8) | buf[hl + 5]) != id ||
                ((buf[hl + 6] << 8) | buf[hl + 7]) != (UWORD)seq)   /* 16-bit on the wire */
                continue;
            now = nt_now_us() - t0;
            recvd++;
            tsum += now;
            if (now < tmin) tmin = now;
            if (now > tmax) tmax = now;
            if (!quiet) {
                char fip[20];
                nt_ipstr(from.sin_addr.s_addr, fip);
                printf("%ld bytes from %s: icmp_seq=%ld ttl=%d time=%s ms\n", (long)(n - hl), fip,
                       (long)seq, buf[8], nt_ms(now, t1));
            }
            break;
        }
        while (!stop && (count == 0 || seq < count)) {   /* keep the interval */
            unsigned long long now = nt_now_us();
            if (now >= next) break;
            if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) stop = TRUE;
            else Delay(2);
        }
    }

    printf("\n--- %s ping statistics ---\n", (char *)args[A_HOST]);
    printf("%ld packets transmitted, %ld packets received, %ld%% packet loss\n", (long)sent, (long)recvd,
           sent ? (long)((sent - recvd) * 100 / sent) : 0L);
    if (recvd)
        printf("round-trip min/avg/max = %s/%s/%s ms\n", nt_ms(tmin, t1), nt_ms(tsum / recvd, t2),
               nt_ms(tmax, t3));
    if (recvd == 0) rc = RETURN_WARN;
    CloseSocket(s);
out:
    nt_clock_exit();
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
