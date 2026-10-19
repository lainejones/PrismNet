/*
 * PrismNet NFS client: the NFSv3 backend of the shared network-filesystem
 * core (tools/netfs/core.c). Connection: portmap -> MOUNT3 -> NFS over TCP,
 * AUTH_UNIX, reserved source port. See docs/NFSCLIENT.md.
 * GPL-3.0-only, see LICENSE.
 *
 * Startup:  "host:/export [options]"; NFS-only options: PORT=n MOUNTPORT=n
 * RESVPORT=YES|NO (the rest are the core's).
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "netfs.h"
#include "nfs3.h"

static struct Rpc rpc;
static u8 *tx, *rx;
static u32 ip;
static unsigned nfsport, mountport;
static int resvport;

static u32 num(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* "host:/export" - also resets this backend's state: after ACTION_DIE the
 * next access starts a new process on the same loaded segment */
static int nfs_parse_target(const char *tok)
{
    const char *colon = strchr(tok, ':'), *leaf;
    u32 n;

    nfsport = mountport = 0;
    resvport = 1;
    ip = 0;
    tx = rx = NULL;
    memset(&rpc, 0, sizeof(rpc));
    rpc.sock = -1;
    if (!colon || colon == tok || colon[1] != '/')
        return 0;
    n = colon - tok;
    if (n >= sizeof(M.host)) n = sizeof(M.host) - 1;
    memcpy(M.host, tok, n);
    M.host[n] = 0;
    strncpy(M.export, colon + 1, sizeof(M.export) - 1);
    leaf = strrchr(M.export, '/');
    strncpy(M.volname, leaf ? leaf + 1 : M.export, sizeof(M.volname) - 1);
    if (!M.volname[0]) strcpy(M.volname, "NFS");
    return 1;
}

static int nfs_option(const char *tok)
{
    if (!strncmp(tok, "PORT=", 5))      { nfsport = num(tok + 5); return 1; }
    if (!strncmp(tok, "MOUNTPORT=", 10)) { mountport = num(tok + 10); return 1; }
    if (!strncmp(tok, "RESVPORT=", 9))  { resvport = strcmp(tok + 9, "NO") != 0; return 1; }
    return 0;
}

static int nfs_init(void)
{
    tx = AllocVec(M.wsize + 1024, MEMF_PUBLIC | MEMF_CLEAR);
    rx = AllocVec(M.rsize + 1024, MEMF_PUBLIC | MEMF_CLEAR);
    if (!tx || !rx) {
        if (tx) FreeVec(tx);
        if (rx) FreeVec(rx);
        tx = rx = NULL;
        return -1;
    }
    rpc_init(&rpc, tx, M.wsize + 1024, rx, M.rsize + 1024);
    rpc.uid = M.uid;
    rpc.gid = M.gid;
    rpc.timeout = M.timeout;
    rpc.resvport = resvport;
    return 0;
}

static void nfs_cleanup(void)
{
    rpc_close(&rpc);
    if (tx) FreeVec(tx);
    if (rx) FreeVec(rx);
    tx = rx = NULL;
}

/* portmap -> MOUNT -> NFS: 0, a negative network/RPC error, or the
 * server's positive MOUNT status when it refuses us (e.g. 13 = access) */
static long nfs_connect(void)
{
    unsigned mport = mountport, nport = nfsport;
    long e;

    DBG("nfs: resolve %s", M.host);
    if (!(ip = net_resolve(M.host)))
        return NET_EHOST;
    if (!mport || !nport) {
        if ((e = rpc_connect(&rpc, ip, 111)))
            return e;
        if (!mport && (e = pmap_getport(&rpc, MOUNT_PROG, 3, &mport)))
            goto fail;
        if (!nport && (e = pmap_getport(&rpc, NFS_PROG, 3, &nport)))
            goto fail;
    }
    DBG("nfs: ports mount %ld nfs %ld", (long)mport, (long)nport);
    if ((e = rpc_connect(&rpc, ip, mport)) || (e = mnt3_mnt(&rpc, M.export, &M.root)))
        goto fail;
    if ((e = rpc_connect(&rpc, ip, nport)))
        goto fail;
    if (!M.mounted) {                   /* first time: the server's sizes */
        struct Fsinfo fi;
        if (nfs3_fsinfo(&rpc, &M.root, &fi) == 0) {
            if (fi.rtmax && fi.rtmax < M.rsize)
                M.rsize = fi.rtmax < 4096 ? 4096 : fi.rtmax;
            if (fi.wtmax && fi.wtmax < M.wsize)
                M.wsize = fi.wtmax < 4096 ? 4096 : fi.wtmax;
        }
    }
    return 0;
fail:
    DBG("nfs: connect failed %ld", e);
    rpc_close(&rpc);
    return e;
}

static void nfs_drop(void)      { rpc_close(&rpc); }
static int  nfs_connected(void) { return rpc.sock >= 0; }

static long nfs_getattr(const struct Fh *fh, struct Fattr *a)
{ return nfs3_getattr(&rpc, fh, a); }
static long nfs_lookup(const struct Fh *dir, const char *name, struct Fh *out,
                       struct Fattr *a, int *ha)
{ return nfs3_lookup(&rpc, dir, name, out, a, ha); }
static long nfs_read(const struct Fh *fh, u32 hi, u32 lo, u32 count,
                     const u8 **data, u32 *got, int *eof)
{ return nfs3_read(&rpc, fh, hi, lo, count, data, got, eof); }
static long nfs_readdir(const struct Fh *dir, u32 cookie[2], u8 verf[8], u32 maxcount,
                        nfs3_entry_fn fn, void *ctx, int *eof)
{ return nfs3_readdirplus(&rpc, dir, cookie, verf, maxcount, fn, ctx, eof); }
static long nfs_fsstat(const struct Fh *root, struct Fsstat *s)
{ return nfs3_fsstat(&rpc, root, s); }
static long nfs_setattr(const struct Fh *fh, const struct Sattr *s)
{ return nfs3_setattr(&rpc, fh, s); }
static long nfs_write(const struct Fh *fh, u32 hi, u32 lo, const u8 *data, u32 count,
                      u32 *written, u8 verf[8])
{ return nfs3_write(&rpc, fh, hi, lo, data, count, UNSTABLE, written, verf); }
static long nfs_commit(const struct Fh *fh, u8 verf[8])
{ return nfs3_commit(&rpc, fh, verf); }
static long nfs_create(const struct Fh *dir, const char *name, int guarded,
                       const struct Sattr *s, struct Fh *out, struct Fattr *a, int *ha)
{ return nfs3_create(&rpc, dir, name, guarded ? CREATE_GUARDED : CREATE_UNCHECKED, s, out, a, ha); }
static long nfs_mkdir(const struct Fh *dir, const char *name, const struct Sattr *s,
                      struct Fh *out, struct Fattr *a, int *ha)
{ return nfs3_mkdir(&rpc, dir, name, s, out, a, ha); }
static long nfs_remove(const struct Fh *dir, const char *name)
{ return nfs3_remove(&rpc, dir, name); }
static long nfs_rmdir(const struct Fh *dir, const char *name)
{ return nfs3_rmdir(&rpc, dir, name); }
static long nfs_rename(const struct Fh *fdir, const char *fname,
                       const struct Fh *tdir, const char *tname)
{ return nfs3_rename(&rpc, fdir, fname, tdir, tname); }
static long nfs_readlink(const struct Fh *fh, char *target, u32 max)
{ return nfs3_readlink(&rpc, fh, target, max); }
static long nfs_symlink(const struct Fh *dir, const char *name, const char *target,
                        struct Fh *out, struct Fattr *a, int *ha)
{ return nfs3_symlink(&rpc, dir, name, target, out, a, ha); }

/* ---------------------------------------------------------------------- */
/* pipelined transfers (DEPTH=2..4)                                       */
/* ---------------------------------------------------------------------- */

struct Slot { u32 xid, k; int busy; };

/* read want bytes (a multiple of RSIZE) at off with up to DEPTH READs in
 * flight. Returns the bytes now in buf from the start, contiguous; on any
 * trouble it stops early (closing the connection if replies are still owed)
 * and the core carries on one READ at a time. */
static u32 nfs_read_pipe(const struct Fh *fh, u32 off, u8 *buf, u32 want)
{
    struct Slot sl[4];
    u32 nchunks = want / M.rsize, base = 0, next = 0, i, outstanding = 0, ok = 0;

    if (rpc.sock < 0)
        return 0;
    memset(sl, 0, sizeof(sl));
    while (base < nchunks) {
        while (next < nchunks && next < base + M.depth) {
            long err;
            u32 x = nfs3_read_send(&rpc, fh, 0, off + next * M.rsize, M.rsize, &err);
            if (!x) return ok;          /* connection closed by rpc_send */
            sl[next % M.depth].xid = x;
            sl[next % M.depth].k = next;
            sl[next % M.depth].busy = 1;
            next++;
            outstanding++;
        }
        if (!outstanding)
            break;
        {
            const u8 *d;
            u32 xid, got;
            int eof;
            long e = rpc_recv(&rpc, &xid);
            struct Slot *s = NULL;
            if (rpc.sock < 0)
                return ok;
            for (i = 0; i < M.depth; i++)
                if (sl[i].busy && sl[i].xid == xid) s = &sl[i];
            if (!s)
                continue;               /* a stale reply */
            s->busy = 0;
            outstanding--;
            if (nfs3_read_reply(&rpc, e, &d, &got, &eof) != 0 || got > M.rsize) {
                if (outstanding) rpc_close(&rpc);
                return ok;
            }
            CopyMem((APTR)d, buf + s->k * M.rsize, got);
            if (got < M.rsize) {        /* short (end of file): nothing after it counts */
                if (s->k == base) ok = base * M.rsize + got;
                if (outstanding) rpc_close(&rpc);
                return ok;
            }
            s->xid = 0xffffffffUL;      /* mark done */
            while (base < next) {
                struct Slot *b = &sl[base % M.depth];
                if (b->busy || b->k != base || b->xid != 0xffffffffUL) break;
                b->xid = 0;
                base++;
            }
            ok = base * M.rsize;
        }
    }
    return ok;
}

/* write want bytes (a multiple of WSIZE) at off, UNSTABLE, up to DEPTH
 * WRITEs in flight. Returns the bytes acknowledged from the start, or -1 if
 * the server's verifier changed. */
static long nfs_write_pipe(const struct Fh *fh, u32 off, const u8 *buf, u32 want,
                           u8 verf[8], int *verf_set)
{
    struct Slot sl[4];
    u32 nchunks = want / M.wsize, base = 0, next = 0, i, outstanding = 0;

    if (rpc.sock < 0)
        return 0;
    memset(sl, 0, sizeof(sl));
    while (base < nchunks) {
        while (next < nchunks && next < base + M.depth) {
            long err;
            u32 x = nfs3_write_send(&rpc, fh, 0, off + next * M.wsize,
                                    buf + next * M.wsize, M.wsize, UNSTABLE, &err);
            if (!x) return base * M.wsize;
            sl[next % M.depth].xid = x;
            sl[next % M.depth].k = next;
            sl[next % M.depth].busy = 1;
            next++;
            outstanding++;
        }
        {
            u32 xid, w = 0;
            u8 v[8];
            long e = rpc_recv(&rpc, &xid);
            struct Slot *s = NULL;
            if (rpc.sock < 0)
                return base * M.wsize;
            for (i = 0; i < M.depth; i++)
                if (sl[i].busy && sl[i].xid == xid) s = &sl[i];
            if (!s)
                continue;
            s->busy = 0;
            outstanding--;
            if (nfs3_write_reply(&rpc, e, &w, v) != 0 || w != M.wsize) {
                if (outstanding) rpc_close(&rpc);
                return base * M.wsize;  /* the plain path writes the rest again */
            }
            if (!*verf_set) {
                memcpy(verf, v, 8);
                *verf_set = 1;
            } else if (memcmp(verf, v, 8)) {
                if (outstanding) rpc_close(&rpc);
                return -1;              /* verifier changed */
            }
            s->xid = 0xffffffffUL;
            while (base < next) {
                struct Slot *b = &sl[base % M.depth];
                if (b->busy || b->k != base || b->xid != 0xffffffffUL) break;
                b->xid = 0;
                base++;
            }
        }
    }
    return base * M.wsize;
}

const struct Backend Backend_ops = {
    "NFS",
    nfs_parse_target, nfs_option, nfs_init, nfs_cleanup,
    nfs_connect, nfs_drop, nfs_connected,
    nfs_getattr, nfs_lookup, nfs_read, nfs_readdir, nfs_fsstat, nfs_setattr,
    nfs_write, nfs_commit, nfs_create, nfs_mkdir, nfs_remove, nfs_rmdir, nfs_rename,
    nfs_readlink, nfs_symlink,
    nfs_read_pipe, nfs_write_pipe,
    NULL,
};
