/*
 * PrismNet NFS client: ONC RPC v2 over TCP (RFC 5531). GPL-3.0-only, see LICENSE.
 *
 * tx layout: [record mark 4][call header][args]. A reply may arrive in
 * several record fragments; they are joined in rx. A reply whose XID is not
 * ours (left over from a call that timed out) is read and thrown away.
 */
#include <string.h>
#include "rpc.h"
#include "net.h"

#define CALL        0
#define REPLY       1
#define MSG_ACCEPTED 0
#define AUTH_NONE   0
#define AUTH_UNIX   1
#define LAST_FRAG   0x80000000UL

void rpc_init(struct Rpc *r, u8 *tx, u32 txsize, u8 *rx, u32 rxsize)
{
    memset(r, 0, sizeof(*r));
    r->sock = -1;
    r->tx = tx;
    r->txsize = txsize;
    r->rx = rx;
    r->rxsize = rxsize;
    r->timeout = 15;
    r->uid = r->gid = 1000;
    r->resvport = 1;
    strcpy(r->machine, "amiga");
    r->xid = 0x504e0000UL;           /* "PN" + counter */
}

long rpc_connect(struct Rpc *r, u32 ip, unsigned port)
{
    long s;

    rpc_close(r);
    s = net_connect(ip, port, r->resvport, r->timeout);
    if (s < 0)
        return s;
    r->sock = s;
    r->ip = ip;
    r->port = port;
    return RPC_OK;
}

void rpc_close(struct Rpc *r)
{
    if (r->sock >= 0)
        net_close(r->sock);
    r->sock = -1;
}

struct Xdr *rpc_begin(struct Rpc *r, u32 prog, u32 vers, u32 proc, int auth_unix)
{
    struct Xdr *x = &r->args;

    xdr_init(x, r->tx, r->txsize);
    xdr_put_u32(x, 0);               /* record mark, filled in by rpc_call */
    xdr_put_u32(x, ++r->xid);
    xdr_put_u32(x, CALL);
    xdr_put_u32(x, 2);               /* RPC version */
    xdr_put_u32(x, prog);
    xdr_put_u32(x, vers);
    xdr_put_u32(x, proc);
    if (auth_unix) {
        u32 mlen = (u32)strlen(r->machine);
        xdr_put_u32(x, AUTH_UNIX);
        xdr_put_u32(x, 4 + 4 + XDR_PAD(mlen) + 4 + 4 + 4 + 4);
        xdr_put_u32(x, 0);           /* stamp */
        xdr_put_string(x, r->machine);
        xdr_put_u32(x, r->uid);
        xdr_put_u32(x, r->gid);
        xdr_put_u32(x, 1);           /* aux gids */
        xdr_put_u32(x, r->gid);
    } else {
        xdr_put_u32(x, AUTH_NONE);
        xdr_put_u32(x, 0);
    }
    xdr_put_u32(x, AUTH_NONE);       /* verifier */
    xdr_put_u32(x, 0);
    return x;
}

/* read one whole record (all fragments) into rx; returns its length or < 0 */
static long read_record(struct Rpc *r)
{
    u32 total = 0;

    for (;;) {
        u8 hdr[4];
        u32 mark, n;
        long e = net_recv(r->sock, hdr, 4, r->timeout);

        if (e < 0)
            return e;
        mark = ((u32)hdr[0] << 24) | ((u32)hdr[1] << 16) | ((u32)hdr[2] << 8) | hdr[3];
        n = mark & ~LAST_FRAG;
        if (total + n > r->rxsize)
            return RPC_EPROTO;       /* too big: the stream is lost, caller drops it */
        if ((e = net_recv(r->sock, r->rx + total, n, r->timeout)) < 0)
            return e;
        total += n;
        if (mark & LAST_FRAG)
            return (long)total;
    }
}

/* send the call built by rpc_begin(); returns its XID (0 on error: the
 * connection is then closed) */
u32 rpc_send(struct Rpc *r, long *err)
{
    struct Xdr *a = &r->args;
    u32 len;
    long e;

    *err = 0;
    if (a->err) { *err = RPC_EGARBAGE; return 0; }   /* arguments did not fit tx */
    if (r->sock < 0) { *err = NET_ECLOSED; return 0; }
    len = a->pos - 4;
    r->tx[0] = (u8)((LAST_FRAG | len) >> 24);
    r->tx[1] = (u8)(len >> 16);
    r->tx[2] = (u8)(len >> 8);
    r->tx[3] = (u8)len;
    if ((e = net_send(r->sock, r->tx, a->pos)) < 0) {
        rpc_close(r);
        *err = e;
        return 0;
    }
    return r->xid;
}

/* receive the next reply, whichever call it answers: *xid says which.
 * On RPC_OK r->res is positioned at the results. A negative network or
 * protocol error closes the connection; DENIED/PROG/GARBAGE do not. */
long rpc_recv(struct Rpc *r, u32 *xid)
{
    struct Xdr *x = &r->res;
    u32 stat;
    long e;

    if ((e = read_record(r)) < 0)
        goto lost;
    xdr_init(x, r->rx, (u32)e);
    *xid = xdr_get_u32(x);
    if (xdr_get_u32(x) != REPLY) { e = RPC_EPROTO; goto lost; }
    if (xdr_get_u32(x) != MSG_ACCEPTED)
        return RPC_EDENIED;
    xdr_get_u32(x);                  /* verifier flavour */
    xdr_skip(x, xdr_get_u32(x));     /* verifier body */
    stat = xdr_get_u32(x);
    if (x->err) { e = RPC_EPROTO; goto lost; }
    switch (stat) {
    case 0:  return RPC_OK;
    case 4:  return RPC_EGARBAGE;
    case 1: case 2: case 3:
             return RPC_EPROG;
    default: return RPC_EPROTO;
    }
lost:
    rpc_close(r);                    /* the stream position is unknown now */
    return e;
}

long rpc_call(struct Rpc *r)
{
    u32 want, got;
    long e;

    if (!(want = rpc_send(r, &e)))
        return e;
    for (;;) {
        e = rpc_recv(r, &got);
        if (got == want || r->sock < 0)
            return e;                /* else a stale reply: read the next */
    }
}
