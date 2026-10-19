/*
 * PrismNet NFS client: ONC RPC v2 (RFC 5531) over TCP with record marking,
 * AUTH_UNIX credentials. One call in flight per connection.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PNFS_RPC_H
#define PNFS_RPC_H

#include "xdr.h"

/* rpc_call() results besides the NET_ errors (net.h) */
#define RPC_OK          0
#define RPC_EPROTO     (-20)         /* garbled reply */
#define RPC_EDENIED    (-21)         /* MSG_DENIED (auth) */
#define RPC_EPROG      (-22)         /* program/version/procedure unavailable */
#define RPC_EGARBAGE   (-23)         /* server could not decode our args */

struct Rpc {
    long   sock;                     /* -1 when not connected */
    u32    ip;
    unsigned port;
    int    resvport;
    unsigned timeout;                /* seconds per call */
    u32    xid;
    u32    uid, gid;
    char   machine[32];              /* AUTH_UNIX machine name */
    u8    *tx, *rx;                  /* tx: header + args; rx: one reply */
    u32    txsize, rxsize;
    struct Xdr args;                 /* for the caller to fill after rpc_begin */
    struct Xdr res;                  /* positioned at the results after rpc_call */
};

/* buffers are the caller's (tx/rx sizes bound the largest call/reply) */
void  rpc_init(struct Rpc *r, u8 *tx, u32 txsize, u8 *rx, u32 rxsize);
long  rpc_connect(struct Rpc *r, u32 ip, unsigned port);
void  rpc_close(struct Rpc *r);
/* start a call; then encode the arguments into r->args */
struct Xdr *rpc_begin(struct Rpc *r, u32 prog, u32 vers, u32 proc, int auth_unix);
/* send it and wait for the matching reply; on RPC_OK r->res holds the results */
long  rpc_call(struct Rpc *r);
/* the two halves of rpc_call, for several calls in flight on one connection */
u32   rpc_send(struct Rpc *r, long *err);
long  rpc_recv(struct Rpc *r, u32 *xid);

#endif
