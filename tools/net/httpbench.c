/*
 * httpbench - TCP/IP stack benchmark for any AmiTCP-compatible stack.
 * Part of PrismNet. GPL-3.0-only, see LICENSE.
 *
 *   httpbench HOST [PORT=n] [PATH=/file] [COUNT=n] [CONNECTS=n]
 *             [UPLOAD=kbytes] [SINKPORT=n] [BUFSIZE=bytes] [CALLS=n]
 *
 * Download: COUNT x "GET PATH HTTP/1.0", read to EOF, report KB/s.
 * Connect:  CONNECTS x connect()+CloseSocket(), report connections/s.
 * Upload:   send UPLOAD KB to a discard server on SINKPORT, report KB/s.
 * Calls:    CALLS x getsockopt(SO_TYPE) and x recv(MSG_DONTWAIT) on an idle
 *           UDP socket: the stack's fixed cost per library call, in us.
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

static const char ver[] __attribute__((used)) = "$VER: httpbench 1.0 (16.10.2026) PrismNet";

struct Library *SocketBase;

#define TEMPLATE "HOST/A,PORT/K/N,PATH/K,COUNT/K/N,CONNECTS/K/N,UPLOAD/K/N,SINKPORT/K/N,BUFSIZE/K/N,CALLS/K/N"
enum { A_HOST, A_PORT, A_PATH, A_COUNT, A_CONNECTS, A_UPLOAD, A_SINKPORT, A_BUFSIZE, A_CALLS, A_N };

#define MAXBUF 32768
static UBYTE buf[MAXBUF];
static struct sockaddr_in srv;

static BOOL broken(void)
{
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

static LONG tcp_connect(UWORD port)
{
    struct sockaddr_in a = srv;
    LONG s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    a.sin_port = port;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        CloseSocket(s);
        return -1;
    }
    return s;
}

/* KB/s with one decimal from bytes and microseconds. */
static void rate(unsigned long long bytes, unsigned long long us, char *out)
{
    unsigned long long r;
    if (!us) us = 1;
    r = bytes * 10000000ULL / 1024ULL / us;     /* tenths of KB/s */
    sprintf(out, "%lu.%lu", (unsigned long)(r / 10), (unsigned long)(r % 10));
}

/* One HTTP download; returns body bytes or -1. */
static LONG download(UWORD port, const char *host, const char *path, LONG bsz, unsigned long long *us)
{
    LONG s, n, total = 0, hdr = -1;
    ULONG tail = 0;
    unsigned long long t0 = nt_now_us();
    int len;

    if ((s = tcp_connect(port)) < 0) return -1;
    len = sprintf((char *)buf, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: httpbench/1.0\r\n\r\n", path, host);
    if (send(s, buf, len, 0) != len) { CloseSocket(s); return -1; }
    while ((n = recv(s, buf, bsz, 0)) > 0) {
        if (hdr < 0) {                    /* end of headers, also across chunk edges */
            LONG i;
            for (i = 0; i < n; i++) {
                tail = (tail << 8) | buf[i];
                if (tail == 0x0d0a0d0aUL) {
                    hdr = total + i + 1;
                    break;
                }
            }
        }
        total += n;
        if (broken()) break;
    }
    CloseSocket(s);
    *us = nt_now_us() - t0;
    return hdr < 0 ? total : total - hdr;
}

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda;
    struct hostent *h;
    LONG port = 80, count = 3, connects = 0, upload = 0, sinkport = 8081, bsz = 16384, calls = 0, i;
    const char *path = "/";
    char r1[24], t[16];
    int rc = RETURN_OK;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"httpbench");
        return RETURN_FAIL;
    }
    if (args[A_PORT])     port = *(LONG *)args[A_PORT];
    if (args[A_PATH])     path = (const char *)args[A_PATH];
    if (args[A_COUNT])    count = *(LONG *)args[A_COUNT];
    if (args[A_CONNECTS]) connects = *(LONG *)args[A_CONNECTS];
    if (args[A_UPLOAD])   upload = *(LONG *)args[A_UPLOAD];
    if (args[A_SINKPORT]) sinkport = *(LONG *)args[A_SINKPORT];
    if (args[A_BUFSIZE])  bsz = *(LONG *)args[A_BUFSIZE];
    if (args[A_CALLS])    calls = *(LONG *)args[A_CALLS];
    if (bsz < 512) bsz = 512;
    if (bsz > MAXBUF) bsz = MAXBUF;

    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("httpbench: no TCP/IP stack (bsdsocket.library)\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (!nt_clock_init()) { rc = RETURN_FAIL; goto out; }
    {
        unsigned long long t0 = nt_now_us();
        h = gethostbyname((STRPTR)args[A_HOST]);
        if (!h) {
            printf("httpbench: unknown host %s\n", (char *)args[A_HOST]);
            rc = RETURN_ERROR;
            goto out;
        }
        memset(&srv, 0, sizeof(srv));
        srv.sin_len = sizeof(srv);
        srv.sin_family = AF_INET;
        memcpy(&srv.sin_addr, h->h_addr_list[0], 4);
        nt_ipstr(srv.sin_addr.s_addr, r1);
        printf("httpbench: %s (%s), resolved in %s ms, buffer %ld\n", (char *)args[A_HOST], r1,
               nt_ms(nt_now_us() - t0, t), (long)bsz);
        fflush(stdout);
    }

    if (count > 0) {
        unsigned long long tb = 0, tus = 0;
        for (i = 1; i <= count && !broken(); i++) {
            unsigned long long us;
            LONG n = download((UWORD)port, (char *)args[A_HOST], path, bsz, &us);
            if (n < 0) {
                printf("download %ld: failed (errno %ld)\n", (long)i, (long)Errno());
                rc = RETURN_WARN;
                break;
            }
            rate(n, us, r1);
            printf("download %ld: %ld bytes in %s ms = %s KB/s\n", (long)i, (long)n, nt_ms(us, t), r1);
            fflush(stdout);
            tb += n; tus += us;
        }
        if (tus) {
            rate(tb, tus, r1);
            printf("download average: %s KB/s\n", r1);
        }
    }

    if (connects > 0) {
        unsigned long long t0 = nt_now_us(), us;
        LONG ok = 0;
        for (i = 0; i < connects && !broken(); i++) {
            LONG s = tcp_connect((UWORD)port);
            if (s < 0) break;
            CloseSocket(s);
            ok++;
        }
        us = nt_now_us() - t0;
        if (!us) us = 1;
        printf("connect: %ld/%ld in %s ms = %lu.%lu conn/s\n", (long)ok, (long)connects, nt_ms(us, t),
               (unsigned long)(ok * 1000000ULL / us), (unsigned long)((ok * 10000000ULL / us) % 10));
        if (ok < connects) rc = RETURN_WARN;
    }

    if (upload > 0) {
        LONG s = tcp_connect((UWORD)sinkport);
        if (s < 0) {
            printf("upload: cannot connect to port %ld\n", (long)sinkport);
            rc = RETURN_WARN;
        } else {
            unsigned long long left = (unsigned long long)upload * 1024ULL, sent = 0, t0 = nt_now_us(), us;
            memset(buf, 'x', bsz);
            while (left > 0 && !broken()) {
                LONG n = send(s, buf, left > (unsigned long long)bsz ? bsz : (LONG)left, 0);
                if (n <= 0) break;
                sent += n; left -= n;
            }
            shutdown(s, 1);
            while (recv(s, buf, bsz, 0) > 0)     /* wait for the sink to confirm */
                ;
            us = nt_now_us() - t0;
            CloseSocket(s);
            rate(sent, us, r1);
            printf("upload: %lu bytes in %s ms = %s KB/s\n", (unsigned long)sent, nt_ms(us, t), r1);
        }
    }
    if (calls > 0) {
        LONG s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            printf("calls: socket failed (errno %ld)\n", (long)Errno());
            rc = RETURN_WARN;
        } else {
            unsigned long long t0 = nt_now_us(), t1, t2;
            LONG v, vl;
            for (i = 0; i < calls; i++) {
                vl = sizeof(v);
                getsockopt(s, SOL_SOCKET, SO_TYPE, &v, (socklen_t *)&vl);
            }
            t1 = nt_now_us();
            for (i = 0; i < calls; i++)
                recv(s, buf, 16, MSG_DONTWAIT);
            t2 = nt_now_us();
            printf("calls: getsockopt %lu us, recv %lu us per call\n",
                   (unsigned long)((t1 - t0) / calls), (unsigned long)((t2 - t1) / calls));
            CloseSocket(s);
        }
    }
out:
    nt_clock_exit();
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
