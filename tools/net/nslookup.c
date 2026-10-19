/*
 * nslookup - DNS queries for any AmiTCP-compatible stack. Part of PrismNet.
 * GPL-3.0-only, see LICENSE.
 *
 *   nslookup NAME [SERVER=a.b.c.d] [TYPE=A|AAAA|MX|NS|CNAME|PTR|TXT|SOA|ANY]
 *
 * Sends a real DNS query over UDP. Server: SERVER=, else PrismNet's first
 * nameserver, else the "nameserver" line of ENV:PrismNet/dns or Roadshow's
 * DEVS:Internet/name_resolution. NAME a.b.c.d does a reverse (PTR) lookup.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#include "prismnet/pnclient.h"

static const char ver[] __attribute__((used)) = "$VER: nslookup 1.0 (16.10.2026) PrismNet";

struct Library *SocketBase;

#define TEMPLATE "NAME/A,SERVER/K,TYPE/K,TIMEOUT/K/N,PORT/K/N"
enum { A_NAME, A_SERVER, A_TYPE, A_TIMEOUT, A_PORT, A_N };

static const struct { const char *name; UWORD code; } types[] = {
    { "A", 1 }, { "NS", 2 }, { "CNAME", 5 }, { "SOA", 6 }, { "PTR", 12 }, { "MX", 15 },
    { "TXT", 16 }, { "AAAA", 28 }, { "ANY", 255 },
};

static int ieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

static BOOL parse_ip(const char *s, ULONG *out)
{
    ULONG a = 0, part;
    int n = 0;
    while (n < 4) {
        if (!isdigit((unsigned char)*s)) return FALSE;
        for (part = 0; isdigit((unsigned char)*s); s++) part = part * 10 + (*s - '0');
        if (part > 255) return FALSE;
        a = (a << 8) | part;
        if (++n < 4 && *s++ != '.') return FALSE;
    }
    if (*s) return FALSE;
    *out = a;
    return TRUE;
}

/* "nameserver x" / "NAMESERVER x" from a config file */
static BOOL server_from_file(const char *path, ULONG *out)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    char buf[512], *p;
    LONG n;
    if (!fh) return FALSE;
    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    buf[n > 0 ? n : 0] = 0;
    for (p = buf; *p; p++) {
        if ((p == buf || p[-1] == '\n') && strnicmp(p, "nameserver", 10) == 0) {
            char ip[20];
            int i = 0;
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            while (*p && !isspace((unsigned char)*p) && i < 19) ip[i++] = *p++;
            ip[i] = 0;
            if (parse_ip(ip, out)) return TRUE;
        }
    }
    return FALSE;
}

/* Append NAME as DNS labels at q; returns new end. */
static UBYTE *put_name(UBYTE *q, const char *name)
{
    while (*name) {
        const char *dot = strchr(name, '.');
        int len = dot ? dot - name : (int)strlen(name);
        if (len > 63) len = 63;
        *q++ = (UBYTE)len;
        memcpy(q, name, len);
        q += len;
        name += len;
        if (*name == '.') name++;
    }
    *q++ = 0;
    return q;
}

/* Decode a (possibly compressed) name at off into out; returns offset after it. */
static int get_name(const UBYTE *msg, int len, int off, char *out, int size)
{
    int jumped = 0, end = -1, n = 0, hops = 0;
    out[0] = 0;
    while (off < len && hops++ < 64) {
        int l = msg[off];
        if (l == 0) { off++; break; }
        if ((l & 0xc0) == 0xc0) {
            if (off + 1 >= len) break;
            if (!jumped) end = off + 2;
            off = ((l & 0x3f) << 8) | msg[off + 1];
            jumped = 1;
            continue;
        }
        off++;
        if (n && n < size - 1) out[n++] = '.';
        while (l-- > 0 && off < len) {
            if (n < size - 1) out[n++] = msg[off];
            off++;
        }
        out[n] = 0;
    }
    if (!n && size > 1) { out[0] = '.'; out[1] = 0; }
    return jumped ? end : off;
}

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda;
    static char qname[300], name[300], tmp[300];   /* static: keep off the 4 KB shell stack */
    char sbuf[20];
    ULONG server = 0, ip;
    UWORD qtype = 1, qid, port = 53;
    BOOL mdns = FALSE;
    LONG s, timeout = 5, n = -1, try;
    static UBYTE q[512], r[1500];
    UBYTE *e;
    struct sockaddr_in sa;
    int rc = RETURN_OK, i;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"nslookup"); return RETURN_FAIL; }
    if (args[A_TIMEOUT]) timeout = *(LONG *)args[A_TIMEOUT];
    if (timeout < 1) timeout = 1;
    if (timeout > 60) timeout = 60;

    /* query name and type */
    if (parse_ip((const char *)args[A_NAME], &ip) && !args[A_TYPE]) {
        sprintf(qname, "%lu.%lu.%lu.%lu.in-addr.arpa", (unsigned long)ip & 255, (unsigned long)(ip >> 8) & 255,
                (unsigned long)(ip >> 16) & 255, (unsigned long)(ip >> 24) & 255);
        qtype = 12;
    } else {
        strncpy(qname, (const char *)args[A_NAME], sizeof(qname) - 1);
        qname[sizeof(qname) - 1] = 0;
    }
    if (args[A_TYPE]) {
        for (i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++)
            if (ieq((const char *)args[A_TYPE], types[i].name)) { qtype = types[i].code; break; }
        if (i == (int)(sizeof(types) / sizeof(types[0]))) {
            printf("nslookup: unknown TYPE %s\n", (char *)args[A_TYPE]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    /* which server */
    if (args[A_SERVER] && !parse_ip((const char *)args[A_SERVER], &server)) {
        printf("nslookup: SERVER must be an IP address\n");
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (args[A_PORT]) port = (UWORD)*(LONG *)args[A_PORT];
    i = strlen(qname);
    if (!server && i > 6 && ieq(qname + i - 6, ".local")) {
        /* name.local: ask the LAN by mDNS (224.0.0.251:5353). From a port
         * other than 5353 this is a "legacy" query: answers come back to us. */
        server = 0xe00000fbUL;
        port = 5353;
        mdns = TRUE;
    }
    if (!server && pn_core_running()) {
        struct PnStatus *st = AllocVec(sizeof(*st), MEMF_PUBLIC | MEMF_CLEAR);
        LONG res = st ? pn_core_request(PNOP_STATUS, (ULONG)st, 0, 0, 100) : PNC_NOT_RUNNING;
        if (res >= 0)
            server = st->dns[0];
        if (res != PNC_TIMEOUT)         /* after a timeout the core may still write it */
            FreeVec(st);
    }
    if (!server) server_from_file("ENV:PrismNet/dns", &server);
    if (!server) server_from_file("DEVS:Internet/name_resolution", &server);
    if (!server) {
        printf("nslookup: no DNS server known - use SERVER=a.b.c.d\n");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("nslookup: no TCP/IP stack (bsdsocket.library)\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    sprintf(sbuf, "%lu.%lu.%lu.%lu", (unsigned long)(server >> 24) & 255, (unsigned long)(server >> 16) & 255,
            (unsigned long)(server >> 8) & 255, (unsigned long)server & 255);
    printf("Server:  %s%s\nAddress: %s#%u\n\n", sbuf, mdns ? " (mDNS)" : "", sbuf, (unsigned)port);

    /* build the query */
    qid = (UWORD)(((ULONG)FindTask(NULL) >> 3) ^ (ULONG)rda);
    memset(q, 0, 12);
    q[0] = qid >> 8; q[1] = qid; q[2] = 0x01;            /* RD */
    q[5] = 1;                                           /* QDCOUNT */
    e = put_name(q + 12, qname);
    *e++ = qtype >> 8; *e++ = qtype; *e++ = 0; *e++ = 1; /* class IN */

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        printf("nslookup: cannot open a UDP socket (errno %ld)\n", (long)Errno());
        rc = RETURN_FAIL;
        goto out;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = port;
    sa.sin_addr.s_addr = server;
    for (try = 0; try < 2 && n < 0; try++) {
        struct timeval tv;
        fd_set rd;
        ULONG sigs = SIGBREAKF_CTRL_C;
        sendto(s, q, e - q, 0, (struct sockaddr *)&sa, sizeof(sa));
        tv.tv_secs = timeout;
        tv.tv_micro = 0;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        if (WaitSelect(s + 1, &rd, NULL, NULL, (struct __timeval *)&tv, &sigs) > 0) {
            n = recv(s, r, sizeof(r), 0);
            if (n >= 12 && (((r[0] << 8) | r[1]) != qid)) n = -1;   /* not ours */
        }
        if (sigs & SIGBREAKF_CTRL_C) break;
    }
    CloseSocket(s);
    if (n < 12) {
        printf(";; connection timed out; no servers could be reached\n");
        rc = RETURN_ERROR;
        goto out;
    }

    {
        int rcode = r[3] & 15, an = (r[6] << 8) | r[7], ns = (r[8] << 8) | r[9];
        int off = 12, qd = (r[4] << 8) | r[5], k;
        static const char *const rcodes[] = { "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED" };

        if (rcode) {
            printf("** server can't find %s: %s\n", qname, rcode < 6 ? rcodes[rcode] : "error");
            rc = RETURN_WARN;
            goto out;
        }
        for (k = 0; k < qd; k++) {                  /* skip the question */
            off = get_name(r, n, off, tmp, sizeof(tmp));
            off += 4;
        }
        if (r[2] & 0x02) printf(";; Truncated answer (only part of it fits in UDP)\n");
        if (!(r[2] & 0x04)) printf("Non-authoritative answer:\n");
        if (an == 0) printf("*** No answer for %s (%d authority records)\n", qname, ns);
        for (k = 0; k < an && off + 10 <= n; k++) {
            int type, rdlen, rd;
            off = get_name(r, n, off, name, sizeof(name));
            if (off + 10 > n) break;
            type = (r[off] << 8) | r[off + 1];
            rdlen = (r[off + 8] << 8) | r[off + 9];
            rd = off + 10;
            if (rd + rdlen > n) break;
            switch (type) {
            case 1:
                if (rdlen < 4) break;
                printf("Name:    %s\nAddress: %d.%d.%d.%d\n", name, r[rd], r[rd + 1], r[rd + 2], r[rd + 3]);
                break;
            case 28:
                if (rdlen < 16) break;
                printf("Name:    %s\nAddress: ", name);
                for (i = 0; i < 16; i += 2)
                    printf("%x%s", (r[rd + i] << 8) | r[rd + i + 1], i < 14 ? ":" : "\n");
                break;
            case 5:  get_name(r, n, rd, tmp, sizeof(tmp)); printf("%s\tcanonical name = %s.\n", name, tmp); break;
            case 2:  get_name(r, n, rd, tmp, sizeof(tmp)); printf("%s\tnameserver = %s.\n", name, tmp); break;
            case 12: get_name(r, n, rd, tmp, sizeof(tmp)); printf("%s\tname = %s.\n", name, tmp); break;
            case 15:
                if (rdlen < 3) break;
                get_name(r, n, rd + 2, tmp, sizeof(tmp));
                printf("%s\tmail exchanger = %d %s.\n", name, (r[rd] << 8) | r[rd + 1], tmp);
                break;
            case 16:
                printf("%s\ttext = \"", name);
                for (i = rd + 1; i < rd + rdlen && i < rd + 1 + r[rd]; i++) putchar(r[i]);
                printf("\"\n");
                break;
            case 6:
                get_name(r, n, rd, tmp, sizeof(tmp));
                printf("%s\torigin = %s\n", name, tmp);
                break;
            default:
                printf("%s\ttype %d, %d bytes\n", name, type, rdlen);
            }
            off = rd + rdlen;
        }
    }
out:
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
