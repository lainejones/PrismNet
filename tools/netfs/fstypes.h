/*
 * PrismNet network filesystems: what the shared core and every protocol
 * backend (NFS, later SMB) exchange - handles, attributes, results.
 *
 * Results: 0 = OK; > 0 = a filesystem error in NFSv3 numbering (the backend
 * maps its own protocol's errors onto these); < 0 = a network or protocol
 * failure (net.h NET_*, or FS_* below). The core turns them into AmigaDOS
 * errors. GPL-3.0-only, see LICENSE.
 */
#ifndef NETFS_FSTYPES_H
#define NETFS_FSTYPES_H

#include "nettypes.h"

#define NFS3_FHSIZE   64              /* an NFSv3 handle at most */
#ifndef FS_FHSIZE
#define FS_FHSIZE     NFS3_FHSIZE     /* room in a handle; the SMB build keeps a path here */
#endif

/* object types (NFSv3 values) */
#define NF3REG  1
#define NF3DIR  2
#define NF3LNK  5

/* filesystem errors (NFSv3 values) */
#define NFS3ERR_PERM        1
#define NFS3ERR_NOENT       2
#define NFS3ERR_IO          5
#define NFS3ERR_ACCES       13
#define NFS3ERR_EXIST       17
#define NFS3ERR_NOTDIR      20
#define NFS3ERR_ISDIR       21
#define NFS3ERR_FBIG        27
#define NFS3ERR_NOSPC       28
#define NFS3ERR_ROFS        30
#define NFS3ERR_NAMETOOLONG 63
#define NFS3ERR_NOTEMPTY    66
#define NFS3ERR_STALE       70
#define NFS3ERR_BAD_COOKIE  10003
#define NFS3ERR_NOTSUPP     10004

/* protocol failures shared by the backends (the RPC values, see rpc.h) */
#define FS_EPROTO    (-20)            /* garbled reply: the connection is lost */
#define FS_EDENIED   (-21)            /* the server refuses us (authentication) */
#define FS_EUNSUPP   (-22)            /* the server lacks the service/version */
#define FS_EGARBAGE  (-23)

/* an object handle, opaque to the core */
struct Fh {
    u32 len;
    u8  data[FS_FHSIZE];
};

struct Fattr {
    u32 type, mode, nlink, uid, gid;
    u32 size_hi, size_lo;
    u32 fileid_hi, fileid_lo;
    u32 atime, mtime, ctime;         /* seconds since 1970, UTC */
    u32 dosattrs;                    /* SMB: the server's FILE_ATTRIBUTE_* bits (NFS: 0) */
};

struct Fsstat {
    u32 tbytes_hi, tbytes_lo, fbytes_hi, fbytes_lo, abytes_hi, abytes_lo;
};

/* what SETATTR/CREATE/MKDIR set; a field is set only when its flag is */
struct Sattr {
    int set_mode, set_size, set_mtime;
    u32 mode, size, mtime;           /* size: below 4 GB; mtime: UTC seconds */
};

/* one directory entry: name is not terminated; attr/fh may be NULL.
 * Return nonzero to stop early (the cookie of this entry is then not used). */
typedef int (*nfs3_entry_fn)(void *ctx, const char *name, u32 namelen,
                             const struct Fattr *attr, const struct Fh *fh,
                             u32 cookie_hi, u32 cookie_lo);

#endif
