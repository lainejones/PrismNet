/*
 * PrismNet NFS client: portmap, MOUNT3 and NFS3 calls (RFC 1833, 1813).
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include "nfs3.h"

#define PMAPPROC_GETPORT   3
#define MOUNTPROC3_MNT     1
#define MOUNTPROC3_UMNT    3
#define NFSPROC3_GETATTR   1
#define NFSPROC3_LOOKUP    3
#define NFSPROC3_READ      6
#define NFSPROC3_READDIRPLUS 17
#define NFSPROC3_FSSTAT    18
#define NFSPROC3_FSINFO    19
#define NFSPROC3_READLINK  5
#define NFSPROC3_SYMLINK   10
#define NFSPROC3_SETATTR   2
#define NFSPROC3_WRITE     7
#define NFSPROC3_CREATE    8
#define NFSPROC3_MKDIR     9
#define NFSPROC3_REMOVE    12
#define NFSPROC3_RMDIR     13
#define NFSPROC3_RENAME    14
#define NFSPROC3_COMMIT    21
#define IPPROTO_TCP_       6

static void put_fh(struct Xdr *x, const struct Fh *fh)
{
    xdr_put_opaque(x, fh->data, fh->len);
}

static void get_fh(struct Xdr *x, struct Fh *fh)
{
    fh->len = xdr_get_opaque(x, fh->data, NFS3_FHSIZE);
}

static void get_fattr(struct Xdr *x, struct Fattr *a)
{
    a->type  = xdr_get_u32(x);
    a->mode  = xdr_get_u32(x);
    a->nlink = xdr_get_u32(x);
    a->uid   = xdr_get_u32(x);
    a->gid   = xdr_get_u32(x);
    xdr_get_u64(x, &a->size_hi, &a->size_lo);
    xdr_get_u64(x, 0, 0);            /* used */
    xdr_get_u64(x, 0, 0);            /* rdev */
    xdr_get_u64(x, 0, 0);            /* fsid */
    xdr_get_u64(x, &a->fileid_hi, &a->fileid_lo);
    a->atime = xdr_get_u32(x); xdr_get_u32(x);
    a->mtime = xdr_get_u32(x); xdr_get_u32(x);
    a->ctime = xdr_get_u32(x); xdr_get_u32(x);
}

/* post_op_attr: returns 1 if attributes followed */
static int get_post_attr(struct Xdr *x, struct Fattr *a)
{
    struct Fattr dummy;

    if (!xdr_get_u32(x))
        return 0;
    get_fattr(x, a ? a : &dummy);
    return 1;
}

static long done(struct Rpc *r, long e)
{
    if (e != RPC_OK)
        return e;
    return r->res.err ? RPC_EPROTO : 0;
}

/* run the call; on success return the NFS status word that starts the result */
static long call_status(struct Rpc *r)
{
    long e = rpc_call(r);
    u32 st;

    if (e != RPC_OK)
        return e;
    st = xdr_get_u32(&r->res);
    return r->res.err ? RPC_EPROTO : (long)st;
}

long pmap_getport(struct Rpc *r, u32 prog, u32 vers, unsigned *port)
{
    struct Xdr *x = rpc_begin(r, PMAP_PROG, 2, PMAPPROC_GETPORT, 0);
    long e;

    xdr_put_u32(x, prog);
    xdr_put_u32(x, vers);
    xdr_put_u32(x, IPPROTO_TCP_);
    xdr_put_u32(x, 0);
    if ((e = rpc_call(r)) != RPC_OK)
        return e;
    *port = (unsigned)xdr_get_u32(&r->res);
    if ((e = done(r, e)) != 0)
        return e;
    return *port ? 0 : RPC_EPROG;    /* 0: not registered */
}

long mnt3_mnt(struct Rpc *r, const char *path, struct Fh *root)
{
    struct Xdr *x = rpc_begin(r, MOUNT_PROG, 3, MOUNTPROC3_MNT, 1);
    long st;

    xdr_put_string(x, path);
    if ((st = call_status(r)) != 0)
        return st;
    get_fh(&r->res, root);           /* auth flavours that follow are ignored */
    return done(r, RPC_OK);
}

long mnt3_umnt(struct Rpc *r, const char *path)
{
    struct Xdr *x = rpc_begin(r, MOUNT_PROG, 3, MOUNTPROC3_UMNT, 1);

    xdr_put_string(x, path);
    return rpc_call(r);
}

long nfs3_getattr(struct Rpc *r, const struct Fh *fh, struct Fattr *a)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_GETATTR, 1);
    long st;

    put_fh(x, fh);
    if ((st = call_status(r)) != 0)
        return st;
    get_fattr(&r->res, a);
    return done(r, RPC_OK);
}

long nfs3_lookup(struct Rpc *r, const struct Fh *dir, const char *name,
                 struct Fh *out, struct Fattr *a, int *have_attr)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_LOOKUP, 1);
    long st;
    int h;

    put_fh(x, dir);
    xdr_put_string(x, name);
    if ((st = call_status(r)) != 0)
        return st;
    get_fh(&r->res, out);
    h = get_post_attr(&r->res, a);
    if (have_attr) *have_attr = h;
    return done(r, RPC_OK);
}

long nfs3_read(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo, u32 count,
               const u8 **data, u32 *got, int *eof)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_READ, 1);
    long st;

    put_fh(x, fh);
    xdr_put_u64(x, off_hi, off_lo);
    xdr_put_u32(x, count);
    if ((st = call_status(r)) != 0)
        return st;
    get_post_attr(&r->res, 0);
    xdr_get_u32(&r->res);            /* count (the opaque carries it too) */
    *eof = (int)xdr_get_u32(&r->res);
    *data = xdr_get_opaque_ref(&r->res, got);
    if (*got > count)                   /* more than asked: not a real reply */
        return done(r, RPC_EPROTO);
    return done(r, RPC_OK);
}

long nfs3_readdirplus(struct Rpc *r, const struct Fh *dir, u32 cookie[2], u8 verf[8],
                      u32 maxcount, nfs3_entry_fn fn, void *ctx, int *eof)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_READDIRPLUS, 1);
    struct Xdr *res = &r->res;
    long st;

    put_fh(x, dir);
    xdr_put_u64(x, cookie[0], cookie[1]);
    xdr_put_u32(x, ((u32)verf[0] << 24) | ((u32)verf[1] << 16) | ((u32)verf[2] << 8) | verf[3]);
    xdr_put_u32(x, ((u32)verf[4] << 24) | ((u32)verf[5] << 16) | ((u32)verf[6] << 8) | verf[7]);
    xdr_put_u32(x, maxcount / 4);    /* dircount: names + cookies only */
    xdr_put_u32(x, maxcount);
    if ((st = call_status(r)) != 0)
        return st;
    get_post_attr(res, 0);
    xdr_get_fixed(res, verf, 8);
    while (xdr_get_u32(res) && !res->err) {
        struct Fattr a;
        struct Fh fh;
        u32 nlen, chi, clo;
        const char *name;
        int ha, hf;

        xdr_get_u64(res, 0, 0);      /* fileid */
        name = (const char *)xdr_get_opaque_ref(res, &nlen);
        xdr_get_u64(res, &chi, &clo);
        ha = get_post_attr(res, &a);
        hf = (int)xdr_get_u32(res);
        if (hf) get_fh(res, &fh);
        if (res->err)
            break;
        if (fn(ctx, name, nlen, ha ? &a : 0, hf ? &fh : 0, chi, clo)) {
            *eof = 0;
            return done(r, RPC_OK);
        }
        cookie[0] = chi;
        cookie[1] = clo;
    }
    *eof = (int)xdr_get_u32(res);
    return done(r, RPC_OK);
}

long nfs3_fsstat(struct Rpc *r, const struct Fh *root, struct Fsstat *s)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_FSSTAT, 1);
    long st;

    put_fh(x, root);
    if ((st = call_status(r)) != 0)
        return st;
    get_post_attr(&r->res, 0);
    xdr_get_u64(&r->res, &s->tbytes_hi, &s->tbytes_lo);
    xdr_get_u64(&r->res, &s->fbytes_hi, &s->fbytes_lo);
    xdr_get_u64(&r->res, &s->abytes_hi, &s->abytes_lo);
    return done(r, RPC_OK);
}

long nfs3_fsinfo(struct Rpc *r, const struct Fh *root, struct Fsinfo *i)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_FSINFO, 1);
    long st;

    put_fh(x, root);
    if ((st = call_status(r)) != 0)
        return st;
    get_post_attr(&r->res, 0);
    i->rtmax  = xdr_get_u32(&r->res);
    i->rtpref = xdr_get_u32(&r->res);
    xdr_get_u32(&r->res);            /* rtmult */
    i->wtmax  = xdr_get_u32(&r->res);
    i->wtpref = xdr_get_u32(&r->res);
    xdr_get_u32(&r->res);            /* wtmult */
    i->dtpref = xdr_get_u32(&r->res);
    return done(r, RPC_OK);
}

/* ---- write side (stage 2) -------------------------------------------- */

static void put_sattr(struct Xdr *x, const struct Sattr *s)
{
    xdr_put_u32(x, s && s->set_mode);
    if (s && s->set_mode) xdr_put_u32(x, s->mode);
    xdr_put_u32(x, 0);               /* uid: leave */
    xdr_put_u32(x, 0);               /* gid: leave */
    xdr_put_u32(x, s && s->set_size);
    if (s && s->set_size) xdr_put_u64(x, 0, s->size);
    xdr_put_u32(x, 0);               /* atime: don't change */
    if (s && s->set_mtime) {
        xdr_put_u32(x, 2);           /* SET_TO_CLIENT_TIME */
        xdr_put_u32(x, s->mtime);
        xdr_put_u32(x, 0);
    } else {
        xdr_put_u32(x, 0);
    }
}

/* wcc_data: pre_op_attr (optional size/mtime/ctime) + post_op_attr */
static void skip_wcc(struct Xdr *x)
{
    if (xdr_get_u32(x))
        xdr_skip(x, 24);
    get_post_attr(x, 0);
}

long nfs3_setattr(struct Rpc *r, const struct Fh *fh, const struct Sattr *s)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_SETATTR, 1);

    put_fh(x, fh);
    put_sattr(x, s);
    xdr_put_u32(x, 0);               /* no ctime guard */
    return call_status(r);
}

long nfs3_write(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo,
                const u8 *data, u32 count, int stable, u32 *written, u8 verf[8])
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_WRITE, 1);
    long st;

    put_fh(x, fh);
    xdr_put_u64(x, off_hi, off_lo);
    xdr_put_u32(x, count);
    xdr_put_u32(x, (u32)stable);
    xdr_put_opaque(x, data, count);
    if ((st = call_status(r)) != 0)
        return st;
    skip_wcc(&r->res);
    *written = xdr_get_u32(&r->res);
    xdr_get_u32(&r->res);            /* committed: how stable it is now */
    xdr_get_fixed(&r->res, verf, 8);
    return done(r, RPC_OK);
}

long nfs3_commit(struct Rpc *r, const struct Fh *fh, u8 verf[8])
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_COMMIT, 1);
    long st;

    put_fh(x, fh);
    xdr_put_u64(x, 0, 0);
    xdr_put_u32(x, 0);               /* to the end of the file */
    if ((st = call_status(r)) != 0)
        return st;
    skip_wcc(&r->res);
    xdr_get_fixed(&r->res, verf, 8);
    return done(r, RPC_OK);
}

/* the result shared by CREATE and MKDIR */
static long new_object(struct Rpc *r, const struct Fh *dir, const char *name,
                       struct Fh *out, struct Fattr *a, int *have_attr)
{
    long st = call_status(r);
    int hf;

    if (st != 0)
        return st;
    hf = (int)xdr_get_u32(&r->res);
    if (hf) get_fh(&r->res, out);
    *have_attr = get_post_attr(&r->res, a);
    skip_wcc(&r->res);
    if ((st = done(r, RPC_OK)) != 0)
        return st;
    if (!hf)                         /* server did not say: ask */
        return nfs3_lookup(r, dir, name, out, a, have_attr);
    return 0;
}

long nfs3_create(struct Rpc *r, const struct Fh *dir, const char *name, int how,
                 const struct Sattr *s, struct Fh *out, struct Fattr *a, int *have_attr)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_CREATE, 1);

    put_fh(x, dir);
    xdr_put_string(x, name);
    xdr_put_u32(x, (u32)how);
    put_sattr(x, s);
    return new_object(r, dir, name, out, a, have_attr);
}

long nfs3_mkdir(struct Rpc *r, const struct Fh *dir, const char *name,
                const struct Sattr *s, struct Fh *out, struct Fattr *a, int *have_attr)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_MKDIR, 1);

    put_fh(x, dir);
    xdr_put_string(x, name);
    put_sattr(x, s);
    return new_object(r, dir, name, out, a, have_attr);
}

static long dir_op(struct Rpc *r, u32 proc, const struct Fh *dir, const char *name)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, proc, 1);

    put_fh(x, dir);
    xdr_put_string(x, name);
    return call_status(r);
}

long nfs3_remove(struct Rpc *r, const struct Fh *dir, const char *name)
{
    return dir_op(r, NFSPROC3_REMOVE, dir, name);
}

long nfs3_rmdir(struct Rpc *r, const struct Fh *dir, const char *name)
{
    return dir_op(r, NFSPROC3_RMDIR, dir, name);
}

long nfs3_rename(struct Rpc *r, const struct Fh *fdir, const char *fname,
                 const struct Fh *tdir, const char *tname)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_RENAME, 1);

    put_fh(x, fdir);
    xdr_put_string(x, fname);
    put_fh(x, tdir);
    xdr_put_string(x, tname);
    return call_status(r);
}

/* ---- pipelining halves and links (stage 3) ----------------------------- */

u32 nfs3_read_send(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo,
                   u32 count, long *err)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_READ, 1);

    put_fh(x, fh);
    xdr_put_u64(x, off_hi, off_lo);
    xdr_put_u32(x, count);
    return rpc_send(r, err);
}

long nfs3_read_reply(struct Rpc *r, long rpc_status, const u8 **data, u32 *got, int *eof)
{
    u32 st;

    if (rpc_status != RPC_OK)
        return rpc_status;
    st = xdr_get_u32(&r->res);
    if (r->res.err)
        return RPC_EPROTO;
    if (st)
        return (long)st;
    get_post_attr(&r->res, 0);
    xdr_get_u32(&r->res);
    *eof = (int)xdr_get_u32(&r->res);
    *data = xdr_get_opaque_ref(&r->res, got);
    return done(r, RPC_OK);
}

u32 nfs3_write_send(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo,
                    const u8 *data, u32 count, int stable, long *err)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_WRITE, 1);

    put_fh(x, fh);
    xdr_put_u64(x, off_hi, off_lo);
    xdr_put_u32(x, count);
    xdr_put_u32(x, (u32)stable);
    xdr_put_opaque(x, data, count);
    return rpc_send(r, err);
}

long nfs3_write_reply(struct Rpc *r, long rpc_status, u32 *written, u8 verf[8])
{
    u32 st;

    if (rpc_status != RPC_OK)
        return rpc_status;
    st = xdr_get_u32(&r->res);
    if (r->res.err)
        return RPC_EPROTO;
    if (st)
        return (long)st;
    skip_wcc(&r->res);
    *written = xdr_get_u32(&r->res);
    xdr_get_u32(&r->res);
    xdr_get_fixed(&r->res, verf, 8);
    return done(r, RPC_OK);
}

long nfs3_readlink(struct Rpc *r, const struct Fh *fh, char *target, u32 max)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_READLINK, 1);
    long st;
    u32 n;

    put_fh(x, fh);
    if ((st = call_status(r)) != 0)
        return st;
    get_post_attr(&r->res, 0);
    n = xdr_get_opaque(&r->res, target, max - 1);
    target[r->res.err ? 0 : n] = 0;
    return done(r, RPC_OK);
}

long nfs3_symlink(struct Rpc *r, const struct Fh *dir, const char *name,
                  const char *target, struct Fh *out, struct Fattr *a, int *have_attr)
{
    struct Xdr *x = rpc_begin(r, NFS_PROG, 3, NFSPROC3_SYMLINK, 1);
    struct Sattr s;

    put_fh(x, dir);
    xdr_put_string(x, name);
    s.set_mode = 1; s.mode = 0777;
    s.set_size = s.set_mtime = 0;
    put_sattr(x, &s);
    xdr_put_string(x, target);
    return new_object(r, dir, name, out, a, have_attr);
}
