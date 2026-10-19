/*
 * PrismNet NFS client: portmap v2, MOUNT v3 and the NFS v3 calls (RFC 1813).
 * Every call returns an NFS status (0 = NFS3_OK, > 0 = NFS3ERR_*) or a
 * negative RPC/network error (rpc.h, net.h). GPL-3.0-only, see LICENSE.
 */
#ifndef PNFS_NFS3_H
#define PNFS_NFS3_H

#include "rpc.h"

#include "fstypes.h"           /* Fh, Fattr, Fsstat, Sattr, NF3*, NFS3ERR_* */

struct Fsinfo {
    u32 rtmax, rtpref, wtmax, wtpref, dtpref;
};

long  pmap_getport(struct Rpc *r, u32 prog, u32 vers, unsigned *port);
long  mnt3_mnt(struct Rpc *r, const char *path, struct Fh *root);

long  nfs3_getattr(struct Rpc *r, const struct Fh *fh, struct Fattr *a);
long  nfs3_lookup(struct Rpc *r, const struct Fh *dir, const char *name,
                  struct Fh *out, struct Fattr *a, int *have_attr);
/* data points into the RPC receive buffer until the next call */
long  nfs3_read(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo, u32 count,
                const u8 **data, u32 *got, int *eof);
/* one READDIRPLUS round: calls fn per entry, updates cookie/verf, sets *eof */
long  nfs3_readdirplus(struct Rpc *r, const struct Fh *dir, u32 cookie[2], u8 verf[8],
                       u32 maxcount, nfs3_entry_fn fn, void *ctx, int *eof);
long  nfs3_fsstat(struct Rpc *r, const struct Fh *root, struct Fsstat *s);
long  nfs3_fsinfo(struct Rpc *r, const struct Fh *root, struct Fsinfo *i);

#define UNSTABLE   0
#define FILE_SYNC  2
#define CREATE_UNCHECKED 0
#define CREATE_GUARDED   1

long  nfs3_setattr(struct Rpc *r, const struct Fh *fh, const struct Sattr *s);
/* data is sent from the caller's buffer; *written may be less than count */
long  nfs3_write(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo,
                 const u8 *data, u32 count, int stable, u32 *written, u8 verf[8]);
long  nfs3_commit(struct Rpc *r, const struct Fh *fh, u8 verf[8]);
/* CREATE and MKDIR return the new handle (looked up if the server leaves it out) */
long  nfs3_create(struct Rpc *r, const struct Fh *dir, const char *name, int how,
                  const struct Sattr *s, struct Fh *out, struct Fattr *a, int *have_attr);
long  nfs3_mkdir(struct Rpc *r, const struct Fh *dir, const char *name,
                 const struct Sattr *s, struct Fh *out, struct Fattr *a, int *have_attr);
long  nfs3_remove(struct Rpc *r, const struct Fh *dir, const char *name);
long  nfs3_rmdir(struct Rpc *r, const struct Fh *dir, const char *name);
long  nfs3_rename(struct Rpc *r, const struct Fh *fdir, const char *fname,
                  const struct Fh *tdir, const char *tname);

/* pipelining: send a READ/WRITE now, decode its reply later. The reply
 * functions take the result of rpc_recv() for that call's XID. */
u32   nfs3_read_send(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo,
                     u32 count, long *err);
long  nfs3_read_reply(struct Rpc *r, long rpc_status, const u8 **data, u32 *got, int *eof);
u32   nfs3_write_send(struct Rpc *r, const struct Fh *fh, u32 off_hi, u32 off_lo,
                      const u8 *data, u32 count, int stable, long *err);
long  nfs3_write_reply(struct Rpc *r, long rpc_status, u32 *written, u8 verf[8]);

/* soft links: the target as the server stores it (terminated, max bytes) */
long  nfs3_readlink(struct Rpc *r, const struct Fh *fh, char *target, u32 max);
long  nfs3_symlink(struct Rpc *r, const struct Fh *dir, const char *name,
                   const char *target, struct Fh *out, struct Fattr *a, int *have_attr);

#define PMAP_PROG   100000UL
#define MOUNT_PROG  100005UL
#define NFS_PROG    100003UL

#endif
