/*
 * PrismNet network filesystems: the shared handler core (core.c) and the
 * table each protocol backend fills in (nfs: tools/nfs/backend.c; later
 * smb). One backend is linked per handler binary. See docs/SMBCLIENT.md,
 * "Stage 0". GPL-3.0-only, see LICENSE.
 */
#ifndef NETFS_NETFS_H
#define NETFS_NETFS_H

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <devices/timer.h>
#include "fstypes.h"
#include "net.h"

/* the core's state; backends read the settings and set root/rsize/wsize */
struct NetState {
    char   host[64], export[256], volname[32];   /* export: path or share */
    u32    uid, gid;
    long   tz_east;                     /* seconds east of UTC */
    int    tz_set, raw, readonly;
    unsigned timeout, retry, idle, depth;
    u32    rsize, wsize;

    struct MsgPort    *port;
    struct DeviceNode *dn;
    struct DosList    *vol;
    u8    *cbuf;
    int    mounted;                     /* MOUNTed once: root and rootattr valid */
    struct Fh root;
    struct Fattr rootattr;
    long   nlocks, nfiles;
    ULONG  last_act;                    /* when the network was last used */
    struct MsgPort *tport;              /* IDLE= timer */
    struct timerequest *treq;
    int    tpending;

    u32    cfileid, coff, clen;         /* last READ chunk, for small reads */
    struct Fsstat fs;
    ULONG  fs_secs;
};
extern struct NetState M;

/* Every call returns 0, a positive NFS3ERR_*, or a negative NET_ / FS_ error
 * (fstypes.h). Calls are made only with the connection up (connect() == 0);
 * a negative result means the connection is gone and the core reconnects. */
struct Backend {
    const char *name;                   /* "NFS" */
    /* Startup: the first word ("host:/export", "host/share"): fill M.host,
     * M.export and a default M.volname; 0 if it is not valid */
    int  (*parse_target)(const char *tok);
    /* an option the core does not know (KEY=value or a switch): 1 if taken */
    int  (*option)(const char *tok);
    /* buffers for M.rsize / M.wsize; 0 on success */
    int  (*init)(void);
    void (*cleanup)(void);

    /* connect, log in and find the root (M.root); while !M.mounted it may
     * also lower M.rsize/M.wsize to what the server allows */
    long (*connect)(void);
    void (*drop)(void);                 /* close the connection, keep the stack */
    int  (*connected)(void);

    long (*getattr)(const struct Fh *fh, struct Fattr *a);
    long (*lookup)(const struct Fh *dir, const char *name, struct Fh *out,
                   struct Fattr *a, int *have_attr);
    /* data points into the backend's receive buffer until the next call */
    long (*read)(const struct Fh *fh, u32 off_hi, u32 off_lo, u32 count,
                 const u8 **data, u32 *got, int *eof);
    long (*readdir)(const struct Fh *dir, u32 cookie[2], u8 verf[8], u32 maxcount,
                    nfs3_entry_fn fn, void *ctx, int *eof);
    long (*fsstat)(const struct Fh *root, struct Fsstat *s);
    long (*setattr)(const struct Fh *fh, const struct Sattr *s);
    /* written data may stay unstable until commit(); verf changes when the
     * server lost unstable data */
    long (*write)(const struct Fh *fh, u32 off_hi, u32 off_lo, const u8 *data,
                  u32 count, u32 *written, u8 verf[8]);
    long (*commit)(const struct Fh *fh, u8 verf[8]);
    long (*create)(const struct Fh *dir, const char *name, int guarded,
                   const struct Sattr *s, struct Fh *out, struct Fattr *a, int *have_attr);
    long (*mkdir)(const struct Fh *dir, const char *name, const struct Sattr *s,
                  struct Fh *out, struct Fattr *a, int *have_attr);
    long (*remove)(const struct Fh *dir, const char *name);
    long (*rmdir)(const struct Fh *dir, const char *name);
    long (*rename)(const struct Fh *fdir, const char *fname,
                   const struct Fh *tdir, const char *tname);
    long (*readlink)(const struct Fh *fh, char *target, u32 max);
    long (*symlink)(const struct Fh *dir, const char *name, const char *target,
                    struct Fh *out, struct Fattr *a, int *have_attr);

    /* optional (NULL): several requests in flight (DEPTH=). read_pipe
     * returns the bytes now in buf from the start; write_pipe the bytes
     * acknowledged, or -1 when the write verifier changed. Both stop early
     * on any trouble and leave the rest to the plain calls. */
    u32  (*read_pipe)(const struct Fh *fh, u32 off, u8 *buf, u32 want);
    long (*write_pipe)(const struct Fh *fh, u32 off, const u8 *buf, u32 want,
                       u8 verf[8], int *verf_set);
    /* optional (NULL): a file the core read or wrote is closed (SMB keeps
     * an open handle on the server per file) */
    void (*release)(const struct Fh *fh);
};
extern const struct Backend Backend_ops;

ULONG now_secs(void);

#ifdef PNFS_DEBUG
void pnfs_dbg(const char *fmt, ...);
#define DBG(...) pnfs_dbg(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

#endif
