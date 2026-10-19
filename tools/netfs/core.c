/*
 * PrismNet network filesystems: the shared DOS handler core. Everything
 * AmigaDOS-facing lives here - packets, locks and paths, ExNext/ExAll, soft
 * links, write-behind, IDLE/RETRY, the volume - and the protocol is a backend
 * (netfs.h: Backend_ops; NFS in tools/nfs/backend.c). Built into
 * L:prismnfs-handler today. See docs/NFSCLIENT.md, docs/SMBCLIENT.md.
 * GPL-3.0-only, see LICENSE.
 *
 * DOSDriver:
 *   Handler   = L:prismnfs-handler
 *   StackSize = 8192
 *   GlobVec   = -1
 *   Startup   = "host:/export [VOLUME=name] [UID=n] [GID=n] [TZ=+H[:MM]]
 *                [RSIZE=n] [WSIZE=n] [CHARSET=UTF8|RAW] [READONLY]
 *                [PORT=n] [MOUNTPORT=n]
 *                [TIMEOUT=s] [RETRY=s] [IDLE=s] [DEPTH=1..4] [RESVPORT=YES|NO]"
 *
 * One process per mounted device. Nothing touches the network until a
 * packet needs it; the first access opens bsdsocket.library, which starts
 * PrismNet on demand. A lost connection is re-made (name resolved again,
 * MOUNT again) and the call retried once.
 *
 * Each Mount loads its own copy of this handler, so the globals are per
 * device.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/locale.h>
#include <devices/timer.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <proto/utility.h>

#include "netfs.h"
#include "names.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct LocaleBase *LocaleBase;
/* libnix's 32-bit divide/multiply helpers (__divsi3 ...) call utility.library
 * through this; with no startup code nobody opens it unless we do */
extern struct Library *UtilityBase;

#define DIR_BATCH   32
#define PATHMAX     512
#define EPOCH_DIFF  252460800L          /* 1970-01-01 .. 1978-01-01 in seconds */
#define ID_NFS_DISK ID_DOS_DISK

/* ---------------------------------------------------------------------- */

struct DEnt {
    u32  type, mode, size, mtime, uid, gid, fileid;
    char name[AMIGA_NAMEMAX + 1];
};

struct Dir {                            /* ExNext state of a directory lock */
    u32  cookie[2];
    u8   verf[8];
    int  eof, n, i;
    struct DEnt e[DIR_BATCH];
};

struct NLock {
    struct FileLock fl;                 /* first: DOS sees this */
    struct Fh    fh;
    u32          type;
    char        *path;                  /* from the export root, Amiga names, "" = root */
    struct Dir  *dir;
};

struct NFile {
    struct NLock *lk;                   /* the file's own lock */
    u32 pos, size;
    u8 *wbuf;                           /* write-behind: WSIZE bytes, made on first write */
    u32 woff, wlen;                     /* file offset and length of what wbuf holds */
    int unstable;                       /* UNSTABLE data the server has not committed */
    int verf_set;
    u8  verf[8];                        /* write verifier of this file's WRITEs */
    LONG werr;                          /* sticky error from a failed write-behind */
};

struct NetState M;
#define B (&Backend_ops)

#ifdef PNFS_DEBUG
/* append one line to DH0:PrismNet/pnfs.log (debug builds only) */
static const ULONG putch[] = { 0x16c04e75 };    /* move.b d0,(a3)+ ; rts */
void pnfs_dbg(const char *fmt, ...)
{
    char line[160];
    BPTR f;

    RawDoFmt((STRPTR)fmt, (APTR)(&fmt + 1), (void (*)())putch, line);
    if ((f = Open((CONST_STRPTR)"DH0:PrismNet/pnfs.log", MODE_READWRITE))) {
        struct DateStamp ds;
        char ts[16];
        ULONG t[3];
        DateStamp(&ds);
        t[0] = ds.ds_Minute / 60; t[1] = ds.ds_Minute % 60; t[2] = ds.ds_Tick / 50;
        RawDoFmt((STRPTR)"%02ld:%02ld:%02ld ", (APTR)t, (void (*)())putch, ts);
        Seek(f, 0, OFFSET_END);
        Write(f, ts, strlen(ts));
        Write(f, line, strlen(line));
        Write(f, "\n", 1);
        Close(f);
    }
}
#endif

/* ---------------------------------------------------------------------- */
/* small helpers                                                          */
/* ---------------------------------------------------------------------- */

static void *alloc(u32 n)
{
    return AllocVec(n, MEMF_PUBLIC | MEMF_CLEAR);
}

static char *strdup_(const char *s)
{
    char *p = alloc(strlen(s) + 1);
    if (p) strcpy(p, s);
    return p;
}

static void bstr_to_c(BSTR b, char *dst, u32 max)
{
    u8 *p = BADDR(b);
    u32 n = p ? p[0] : 0;

    if (n >= max) n = max - 1;
    if (n) memcpy(dst, p + 1, n);
    dst[n] = 0;
}

ULONG now_secs(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / 50;
}

static void to_datestamp(u32 unix_secs, struct DateStamp *ds)
{
    long s = (long)unix_secs + M.tz_east - EPOCH_DIFF;

    if (s < 0) s = 0;
    ds->ds_Days   = s / 86400;
    s %= 86400;
    ds->ds_Minute = s / 60;
    ds->ds_Tick   = (s % 60) * 50;
}

static LONG protection(u32 mode)
{
    LONG p = 0;

    if (!(mode & 0400)) p |= FIBF_READ;     /* owner bits are "denied" when set */
    if (!(mode & 0200)) p |= FIBF_WRITE | FIBF_DELETE;
    if (!(mode & 0100)) p |= FIBF_EXECUTE;
    if (mode & 040) p |= FIBF_GRP_READ;
    if (mode & 020) p |= FIBF_GRP_WRITE | FIBF_GRP_DELETE;
    if (mode & 010) p |= FIBF_GRP_EXECUTE;
    if (mode & 04)  p |= FIBF_OTR_READ;
    if (mode & 02)  p |= FIBF_OTR_WRITE | FIBF_OTR_DELETE;
    if (mode & 01)  p |= FIBF_OTR_EXECUTE;
    return p;
}

static LONG ioerr(long e)
{
    if (e <= -100)
        return -e;                      /* already an AmigaDOS error, negated */
    if (e > 0) {
        switch (e) {
        case NFS3ERR_NOENT: case NFS3ERR_STALE: return ERROR_OBJECT_NOT_FOUND;
        case NFS3ERR_NOTDIR:      return ERROR_DIR_NOT_FOUND;
        case NFS3ERR_ISDIR:       return ERROR_OBJECT_WRONG_TYPE;
        case NFS3ERR_PERM: case NFS3ERR_ACCES: return ERROR_READ_PROTECTED;
        case NFS3ERR_ROFS:        return ERROR_DISK_WRITE_PROTECTED;
        case NFS3ERR_NAMETOOLONG: return ERROR_INVALID_COMPONENT_NAME;
        case NFS3ERR_NOSPC:       return ERROR_DISK_FULL;
        case NFS3ERR_EXIST:       return ERROR_OBJECT_EXISTS;
        case NFS3ERR_NOTEMPTY:    return ERROR_DIRECTORY_NOT_EMPTY;
        case NFS3ERR_IO:          return ERROR_SEEK_ERROR;   /* incl. data lost in a server restart */
        case NFS3ERR_FBIG:        return ERROR_DISK_FULL;
        default:                  return ERROR_OBJECT_NOT_FOUND;
        }
    }
    if (e == FS_EDENIED || e == FS_EUNSUPP)
        return ERROR_NOT_A_DOS_DISK;    /* the server will not talk to us */
    return ERROR_NO_DISK;               /* unreachable: DOS offers Retry */
}

static const char *last_part(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---------------------------------------------------------------------- */
/* connection                                                             */
/* ---------------------------------------------------------------------- */

static void add_volume(void)
{
    struct DosList *v;

    DBG("vol: make %s", M.volname);
    if (M.vol || !(v = MakeDosEntry((CONST_STRPTR)M.volname, DLT_VOLUME)))
        return;
    DBG("vol: made %lx", (ULONG)v);
    v->dol_Task = M.port;
    to_datestamp(M.rootattr.mtime, &v->dol_misc.dol_volume.dol_VolumeDate);
    v->dol_misc.dol_volume.dol_DiskType = ID_NFS_DISK;
    DBG("vol: lock list");
    LockDosList(LDF_VOLUMES | LDF_WRITE);
    DBG("vol: add");
    if (!AddDosEntry(v)) {              /* same name already there */
        UnLockDosList(LDF_VOLUMES | LDF_WRITE);
        FreeDosEntry(v);
        DBG("vol: add failed");
        return;
    }
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    DBG("vol: added");
    M.vol = v;
}

static void remove_volume(void)
{
    if (!M.vol)
        return;
    LockDosList(LDF_VOLUMES | LDF_WRITE);
    RemDosEntry(M.vol);
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    FreeDosEntry(M.vol);
    M.vol = NULL;
}

static void time_zone(void)
{
    struct Locale *loc;

    if (M.tz_set)
        return;
    M.tz_set = 1;
    if ((LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR)"locale.library", 38))) {
        if ((loc = OpenLocale(NULL))) {
            M.tz_east = -loc->loc_GMTOffset * 60L;   /* minutes west -> seconds east */
            CloseLocale(loc);
        }
        CloseLibrary((struct Library *)LocaleBase);
        LocaleBase = NULL;
    }
}

/* connect (and log in / MOUNT) if needed: 0, a negative network or
 * protocol error, or the server's positive refusal (e.g. 13 = access) */
static long ensure(void)
{
    long e;

    if (B->connected() && M.mounted)
        return 0;
    if (net_up())
        return NET_EDOWN;
    if ((e = B->connect()))
        return e;
    if (!M.mounted) {                   /* first time on this mount */
        time_zone();
        if ((e = B->getattr(&M.root, &M.rootattr))) {
            B->drop();
            return e;
        }
        M.mounted = 1;
        add_volume();
    }
    return 0;
}

static void lost(long e)
{
    DBG("lost: %ld", e);
    B->drop();
    if (e == NET_EDOWN)
        net_down();                     /* reopen later: restarts PrismNet */
}

/* after a failed attempt: retry at once the first time, then every 2 s
 * until RETRY= seconds have passed (a NAS restarting its NFS service) */
static int retry_wait(ULONG start, int attempt)
{
    if (attempt == 0)
        return 1;
    /* with files open (a copy in progress) be four times as patient: a
     * network card being reset should not cost the user the transfer */
    if (now_secs() - start >= (M.nfiles ? 4 * M.retry : M.retry))
        return 0;
    Delay(100);
    return 1;
}

/* run expr with the connection up; on a broken connection reconnect and
 * retry, for up to RETRY= seconds. NFS and RPC-level refusals are final. */
#define CALL(expr) ({                                                   \
    long _e = 0; int _t; ULONG _st = now_secs();                        \
    for (_t = 0; ; _t++) {                                              \
        if ((_e = ensure()) == 0) {                                     \
            _e = (expr);                                                \
            if (_e >= 0) M.last_act = now_secs();                       \
            if (_e >= 0 || _e == FS_EDENIED || _e == FS_EUNSUPP) break; \
        } else if (_e > 0 || _e == FS_EDENIED || _e == FS_EUNSUPP) {    \
            break;              /* MOUNT refused: retrying won't help */\
        }                                                               \
        lost(_e);                                                       \
        if (!retry_wait(_st, _t)) break;                                \
    }                                                                   \
    _e; })

/* ---------------------------------------------------------------------- */
/* locks and names                                                        */
/* ---------------------------------------------------------------------- */

static struct NLock *new_lock(const struct Fh *fh, u32 type, u32 fileid, const char *path, LONG mode)
{
    struct NLock *l = alloc(sizeof(*l));

    if (!l)
        return NULL;
    if (!(l->path = strdup_(path))) {
        FreeVec(l);
        return NULL;
    }
    l->fh = *fh;
    l->type = type;
    l->fl.fl_Key = fileid;
    l->fl.fl_Access = mode;
    l->fl.fl_Task = M.port;
    l->fl.fl_Volume = MKBADDR(M.vol);
    M.nlocks++;
    return l;
}

static void free_lock(struct NLock *l)
{
    if (!l)
        return;
    if (l->dir) FreeVec(l->dir);
    FreeVec(l->path);
    FreeVec(l);
    M.nlocks--;
}

struct Scan {                           /* case-insensitive search of a directory */
    const char *want;
    struct Fh  *fh;
    struct Fattr *a;
    char       *name;
    int         found, have_attr;
};

static int scan_fn(void *ctx, const char *name, u32 nlen, const struct Fattr *a,
                   const struct Fh *fh, u32 chi, u32 clo)
{
    struct Scan *s = ctx;
    char am[AMIGA_NAMEMAX + 1];

    (void)chi; (void)clo;
    name_to_amiga(name, nlen, am, sizeof(am), M.raw);
    if (!fh || !name_equal(am, s->want))
        return 0;
    *s->fh = *fh;
    if (a) { *s->a = *a; s->have_attr = 1; }
    strcpy(s->name, am);
    s->found = 1;
    return 1;
}

/* look one name up in dir: exact first, then without regard to case.
 * realname gets the name as the server spells it (Amiga form). */
static long lookup_name(const struct Fh *dir, const char *name, struct Fh *out,
                        struct Fattr *a, int *have_attr, char *realname)
{
    char srv[AMIGA_NAMEMAX * 2 + 2];
    long e;

    if (!name_to_server(name, srv, sizeof(srv), M.raw))
        return NFS3ERR_NAMETOOLONG;
    e = CALL(B->lookup(dir, srv, out, a, have_attr));
    if (e == 0) {
        strcpy(realname, name);
        return 0;
    }
    if (e == NFS3ERR_NOENT) {
        struct Scan s;
        u32 cookie[2] = { 0, 0 };
        u8 verf[8] = { 0 };
        int eof = 0;

        s.want = name; s.fh = out; s.a = a; s.name = realname;
        s.found = s.have_attr = 0;
        while (!eof && !s.found) {
            long r = CALL(B->readdir(dir, cookie, verf, 4096, scan_fn, &s, &eof));
            if (r) return r;
        }
        if (s.found) {
            *have_attr = s.have_attr;
            return 0;
        }
    }
    return e;
}

/* resolve an AmigaDOS name relative to a lock (NULL = root) */
static long resolve(struct NLock *base, const char *name, struct Fh *fh,
                    struct Fattr *a, char *path)
{
    const char *p = strchr(name, ':');
    char part[AMIGA_NAMEMAX + 1], real[AMIGA_NAMEMAX + 1];
    int have_attr = 0;
    long e;

    if ((e = CALL(0L)))                 /* the root handle comes from MOUNT */
        return e;
    if (p || !base) {
        *fh = M.root;
        path[0] = 0;
        p = p ? p + 1 : name;
    } else {
        *fh = base->fh;
        strcpy(path, base->path);
        p = name;
    }
    while (*p) {
        struct Fh next;
        u32 n;

        if (*p == '/') {                /* parent */
            char *cut;
            if (!path[0])
                return NFS3ERR_NOENT;   /* nothing above the root */
            if ((e = CALL(B->lookup(fh, "..", &next, a, &have_attr))))
                return e;
            *fh = next;
            cut = strrchr(path, '/');
            if (cut) *cut = 0; else path[0] = 0;
            p++;
            continue;
        }
        for (n = 0; p[n] && p[n] != '/'; n++)
            ;
        if (n > AMIGA_NAMEMAX)
            return NFS3ERR_NAMETOOLONG;
        memcpy(part, p, n);
        part[n] = 0;
        p += n;
        if (*p == '/') p++;             /* the separator */
        if ((e = lookup_name(fh, part, &next, a, &have_attr, real)))
            return e;
        if (!have_attr && (e = CALL(B->getattr(&next, a))))
            return e;
        have_attr = 1;
        if (a->type == NF3LNK)
            return -ERROR_IS_SOFT_LINK;     /* DOS asks READ_LINK and tries again */
        if (strlen(path) + strlen(real) + 2 > PATHMAX)
            return NFS3ERR_NAMETOOLONG;
        if (path[0]) strcat(path, "/");
        strcat(path, real);
        *fh = next;
    }
    if (!have_attr && (e = CALL(B->getattr(fh, a))))
        return e;
    return 0;
}

static void fill_fib(struct FileInfoBlock *fib, const char *name, u32 type, u32 mode,
                     u32 size_hi, u32 size_lo, u32 mtime, u32 uid, u32 gid, u32 fileid)
{
    u32 n = strlen(name), size = size_hi ? 0xffffffffUL : size_lo;
    LONG st = type == NF3DIR ? ST_USERDIR : type == NF3LNK ? ST_SOFTLINK : ST_FILE;

    memset(fib, 0, sizeof(*fib));
    fib->fib_DiskKey = fileid;
    fib->fib_DirEntryType = fib->fib_EntryType = st;
    if (n > AMIGA_NAMEMAX) n = AMIGA_NAMEMAX;
    fib->fib_FileName[0] = (char)n;     /* BCPL string at packet level */
    memcpy(fib->fib_FileName + 1, name, n);
    fib->fib_Protection = protection(mode);
    fib->fib_Size = size;
    fib->fib_NumBlocks = size / 512 + (size % 512 ? 1 : 0);
    to_datestamp(mtime, &fib->fib_Date);
    fib->fib_OwnerUID = (UWORD)uid;
    fib->fib_OwnerGID = (UWORD)gid;
}

static long examine_lock(struct NLock *l, struct FileInfoBlock *fib)
{
    struct Fattr a;
    long e = CALL(B->getattr(&l->fh, &a));

    if (e)
        return e;
    l->type = a.type;
    fill_fib(fib, l->path[0] ? last_part(l->path) : M.volname, a.type, a.mode,
             a.size_hi, a.size_lo, a.mtime, a.uid, a.gid, a.fileid_lo);
    return 0;
}

static int dir_fn(void *ctx, const char *name, u32 nlen, const struct Fattr *a,
                  const struct Fh *fh, u32 chi, u32 clo)
{
    struct Dir *d = ctx;
    struct DEnt *e;

    (void)fh; (void)chi; (void)clo;
    if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    if (memchr(name, '/', nlen) || memchr(name, ':', nlen))
        return 0;                       /* no Amiga spelling for these */
    if (d->n >= DIR_BATCH)
        return 1;                       /* full: resume from the previous cookie */
    e = &d->e[d->n++];
    name_to_amiga(name, nlen, e->name, sizeof(e->name), M.raw);
    if (a) {
        e->type = a->type; e->mode = a->mode; e->mtime = a->mtime;
        e->uid = a->uid; e->gid = a->gid; e->fileid = a->fileid_lo;
        e->size = a->size_hi ? 0xffffffffUL : a->size_lo;
    } else {
        e->type = 0;                    /* filled in by a LOOKUP afterwards */
    }
    return 0;
}

/* the next entry of a directory lock; restart: from the beginning.
 * Returns 0 with *out set, -ERROR_NO_MORE_ENTRIES at the end, or an error. */
static long dir_next(struct NLock *l, int restart, struct DEnt **out)
{
    struct Dir *d = l->dir;

    if (l->type != NF3DIR)
        return -ERROR_NO_MORE_ENTRIES;
    if (!d) {
        if (!(d = l->dir = alloc(sizeof(*d))))
            return -ERROR_NO_FREE_STORE;
        restart = 1;
    }
    if (restart) {
        memset(d, 0, sizeof(*d) - sizeof(d->e));
        d->n = d->i = 0;
    }
    while (d->i >= d->n) {
        long r;
        int k;

        if (d->eof)
            return -ERROR_NO_MORE_ENTRIES;
        d->n = d->i = 0;
        r = CALL(B->readdir(&l->fh, d->cookie, d->verf, 4096, dir_fn, d, &d->eof));
        if (r == NFS3ERR_BAD_COOKIE)
            return -ERROR_NO_MORE_ENTRIES;
        if (r)
            return r;
        for (k = 0; k < d->n; k++) {   /* entries the server sent without attributes */
            struct DEnt *x = &d->e[k];
            struct Fh fh;
            struct Fattr a;
            int ha;
            char real[AMIGA_NAMEMAX + 1];
            if (x->type)
                continue;
            if (lookup_name(&l->fh, x->name, &fh, &a, &ha, real) == 0 &&
                (ha || CALL(B->getattr(&fh, &a)) == 0)) {
                x->type = a.type; x->mode = a.mode; x->mtime = a.mtime;
                x->uid = a.uid; x->gid = a.gid; x->fileid = a.fileid_lo;
                x->size = a.size_hi ? 0xffffffffUL : a.size_lo;
            } else {
                x->type = NF3REG;
            }
        }
    }
    *out = &d->e[d->i++];
    return 0;
}

static long examine_next(struct NLock *l, struct FileInfoBlock *fib)
{
    struct DEnt *e;
    long r = dir_next(l, (u32)fib->fib_DiskKey == (u32)l->fl.fl_Key || !l->dir, &e);

    if (r)
        return r;
    fill_fib(fib, e->name, e->type, e->mode, 0, e->size, e->mtime, e->uid, e->gid, e->fileid);
    if ((u32)fib->fib_DiskKey == (u32)l->fl.fl_Key)
        fib->fib_DiskKey ^= 0x80000000UL;   /* never looks like "restart" */
    return 0;
}

/* ACTION_EXAMINE_ALL: as many entries as fit, of the size type asks for.
 * Returns DOSTRUE (call again), or 0 with *err (ERROR_NO_MORE_ENTRIES at the
 * end - entries may still have been filled in by this call). */
static LONG examine_all(struct NLock *l, u8 *buf, LONG size, LONG type,
                        struct ExAllControl *eac, LONG *err)
{
    static const UBYTE recsize[8] = { 0, 8, 12, 16, 20, 32, 36, 40 };
    struct ExAllData *prev = NULL;
    u8 *end = buf + size;
    long r;

    eac->eac_Entries = 0;
    if (type < ED_NAME || type > ED_OWNER) { *err = ERROR_BAD_NUMBER; return 0; }
    if (!l || l->type != NF3DIR)           { *err = ERROR_OBJECT_WRONG_TYPE; return 0; }
    r = 0;
    for (;;) {
        struct DEnt *e;
        struct ExAllData *ed;
        u32 rec, nlen;
        int restart = eac->eac_LastKey == 0;

        eac->eac_LastKey = 1;
        if ((r = dir_next(l, restart, &e)))
            break;
        nlen = strlen(e->name) + 1;
        rec = (recsize[type] + nlen + 1 + 3) & ~3UL;  /* + empty comment */
        if (buf + rec > end) {
            l->dir->i--;                /* give it back for the next call */
            if (!eac->eac_Entries) { *err = ERROR_NO_FREE_STORE; return 0; }
            return DOSTRUE;
        }
        ed = (struct ExAllData *)buf;
        memset(ed, 0, recsize[type]);
        ed->ed_Name = (UBYTE *)buf + recsize[type];
        strcpy((char *)ed->ed_Name, e->name);
        if (type >= ED_TYPE)  ed->ed_Type = e->type == NF3DIR ? ST_USERDIR : e->type == NF3LNK ? ST_SOFTLINK : ST_FILE;
        if (type >= ED_SIZE)  ed->ed_Size = e->size;
        if (type >= ED_PROTECTION) ed->ed_Prot = protection(e->mode);
        if (type >= ED_DATE) {
            struct DateStamp ds;
            to_datestamp(e->mtime, &ds);
            ed->ed_Days = ds.ds_Days; ed->ed_Mins = ds.ds_Minute; ed->ed_Ticks = ds.ds_Tick;
        }
        if (type >= ED_COMMENT) {
            ed->ed_Comment = ed->ed_Name + nlen;
            ed->ed_Comment[0] = 0;
        }
        if (type >= ED_OWNER) { ed->ed_OwnerUID = (UWORD)e->uid; ed->ed_OwnerGID = (UWORD)e->gid; }
        if (eac->eac_MatchString && !MatchPatternNoCase(eac->eac_MatchString, ed->ed_Name))
            continue;
        if (eac->eac_MatchFunc && !CallHookPkt(eac->eac_MatchFunc, &type, ed))
            continue;
        if (prev) prev->ed_Next = ed;
        prev = ed;
        buf += rec;
        eac->eac_Entries++;
    }
    *err = ioerr(r);
    return 0;
}

static void fill_info(struct InfoData *id)
{
    memset(id, 0, sizeof(*id));
    id->id_DiskState = M.readonly ? ID_WRITE_PROTECTED : ID_VALIDATED;
    id->id_BytesPerBlock = 512;
    if (!M.mounted) {
        id->id_DiskType = ID_NO_DISK_PRESENT;
        return;
    }
    /* Workbench asks for this every few seconds: refresh only over a live
     * connection, never connect for it - that would keep the NAS awake and
     * start PrismNet again right after the user stopped it. */
    if (now_secs() - M.fs_secs > 5 && B->connected()) {
        long e = B->fsstat(&M.root, &M.fs);
        if (e == 0)
            M.fs_secs = now_secs();
        else if (e < 0)
            lost(e);
    }
    {
        /* pick a block size that keeps the block counts within 31 bits */
        unsigned long long total = ((unsigned long long)M.fs.tbytes_hi << 32) | M.fs.tbytes_lo;
        unsigned long long free_ = ((unsigned long long)M.fs.fbytes_hi << 32) | M.fs.fbytes_lo;
        u32 bpb = 512;
        while (bpb < 0x1000000UL && total / bpb > 0x7fffffffULL)
            bpb <<= 1;
        id->id_BytesPerBlock = bpb;
        id->id_NumBlocks     = (LONG)(total / bpb);
        id->id_NumBlocksUsed = (LONG)((total - free_) / bpb);
    }
    id->id_DiskType = ID_NFS_DISK;
    id->id_VolumeNode = MKBADDR(M.vol);
    id->id_InUse = M.nlocks > 0;
}

/* ---------------------------------------------------------------------- */
/* reading                                                                */
/* ---------------------------------------------------------------------- */

static LONG do_read(struct NFile *f, u8 *buf, LONG len, LONG *err)
{
    LONG total = 0;

    while (len > 0) {
        const u8 *d;
        u32 got, n;
        int eof;
        long e;

        if (f->lk->fl.fl_Key == (LONG)M.cfileid && f->pos >= M.coff && f->pos < M.coff + M.clen) {
            n = M.coff + M.clen - f->pos;
            if (n > (u32)len) n = len;
            CopyMem(M.cbuf + (f->pos - M.coff), buf, n);
            buf += n; len -= n; total += n; f->pos += n;
            continue;
        }
        if (M.depth > 1 && (u32)len >= 2 * M.rsize) {
            u32 want = ((u32)len / M.rsize) * M.rsize, k = 0;
            if (B->read_pipe && CALL(0L) == 0 &&
                (k = B->read_pipe(&f->lk->fh, f->pos, buf, want)))
                M.last_act = now_secs();
            buf += k; len -= k; total += k; f->pos += k;
            if (k == want) continue;
            if (k && k % M.rsize) break;    /* a short chunk: end of file */
        }
        if (f->pos > 0xffffffffUL - M.rsize)
            break;                      /* AmigaDOS positions end at 4 GB: stop, don't wrap */
        e = CALL(B->read(&f->lk->fh, 0, f->pos, M.rsize, &d, &got, &eof));
        if (e) {
            *err = ioerr(e);
            return total ? total : -1;
        }
        if (!got)
            break;
        if (got > M.rsize) {            /* more than asked for: never trust it */
            *err = ioerr(FS_EPROTO);
            return total ? total : -1;
        }
        if ((u32)len >= got) {          /* whole chunk wanted: straight to the caller */
            CopyMem((APTR)d, buf, got);
            buf += got; len -= got; total += got; f->pos += got;
        } else {                        /* keep it for the next small read */
            CopyMem((APTR)d, M.cbuf, got);
            M.cfileid = f->lk->fl.fl_Key;
            M.coff = f->pos;
            M.clen = got;
        }
        if (eof && !len)
            break;
    }
    return total;
}

/* ---------------------------------------------------------------------- */
/* soft links                                                             */
/* ---------------------------------------------------------------------- */

/* append a server link target (UTF-8, Unix syntax) to out as Amiga syntax:
 * ".." becomes a parent step "/", "." vanishes */
static int unix_to_amiga(char *out, u32 max, const char *t)
{
    u32 n = strlen(out);
    int last_name = 0;

    while (*t) {
        const char *e = strchr(t, '/');
        u32 len = e ? (u32)(e - t) : strlen(t);
        char am[AMIGA_NAMEMAX + 1];
        if (len == 2 && t[0] == '.' && t[1] == '.') {
            if (last_name) out[n - 1] = '/';     /* "x/" + parent = "x//" */
            if (n + 1 >= max) return 0;
            out[n++] = '/';
            last_name = 0;
        } else if (len && !(len == 1 && t[0] == '.')) {
            u32 k = name_to_amiga(t, len, am, sizeof(am), M.raw);
            if (n + k + 2 >= max) return 0;
            memcpy(out + n, am, k);
            n += k;
            out[n++] = '/';
            last_name = 1;
        }
        t += len + (e ? 1 : 0);
    }
    if (last_name) n--;                          /* no separator after the last name */
    out[n] = 0;
    return 1;
}

/* ACTION_READ_LINK: walk path to the first soft link and return the path
 * with that link replaced by its target, for DOS to try again */
static LONG read_link(struct NLock *base, const char *path, char *out, LONG size, LONG *err)
{
    const char *colon = strchr(path, ':'), *p;
    char part[AMIGA_NAMEMAX + 1], real[AMIGA_NAMEMAX + 1], target[512];
    struct Fh fh, next;
    struct Fattr a;
    int ha;
    long e;

    if ((e = CALL(0L))) { *err = ioerr(e); return -1; }
    fh = (colon || !base) ? M.root : base->fh;
    p = colon ? colon + 1 : path;
    while (*p) {
        const char *start = p;
        u32 n;
        if (*p == '/') {
            if ((e = CALL(B->lookup(&fh, "..", &next, &a, &ha)))) { *err = ioerr(e); return -1; }
            fh = next;
            p++;
            continue;
        }
        for (n = 0; p[n] && p[n] != '/'; n++)
            ;
        if (n > AMIGA_NAMEMAX) { *err = ERROR_INVALID_COMPONENT_NAME; return -1; }
        memcpy(part, p, n);
        part[n] = 0;
        p += n;
        if ((e = lookup_name(&fh, part, &next, &a, &ha, real)) ||
            (!ha && (e = CALL(B->getattr(&next, &a))))) { *err = ioerr(e); return -1; }
        if (a.type == NF3LNK) {
            u32 elen = strlen(M.export), k = (u32)(start - path);
            if ((e = CALL(B->readlink(&next, target, sizeof(target))))) { *err = ioerr(e); return -1; }
            if (target[0] == '/') {     /* absolute: only inside our export */
                if (strncmp(target, M.export, elen) || (target[elen] && target[elen] != '/')) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                    return -1;
                }
                if (strlen(M.volname) + 2 > (u32)size) { *err = ERROR_LINE_TOO_LONG; return -1; }
                strcpy(out, M.volname);
                strcat(out, ":");
                if (!unix_to_amiga(out, size, target + elen)) { *err = ERROR_LINE_TOO_LONG; return -1; }
            } else {                    /* relative to the link's drawer */
                if (k + 1 > (u32)size) { *err = ERROR_LINE_TOO_LONG; return -1; }
                memcpy(out, path, k);
                out[k] = 0;
                if (!unix_to_amiga(out, size, target)) { *err = ERROR_LINE_TOO_LONG; return -1; }
            }
            if (*p) {                   /* the rest of the path after the link */
                u32 o = strlen(out);
                if (o + strlen(p) + 2 > (u32)size) { *err = ERROR_LINE_TOO_LONG; return -1; }
                if (o && out[o - 1] != '/' && out[o - 1] != ':' && *p != '/')
                    strcat(out, "/");
                strcat(out, *p == '/' ? p + 1 : p);
            }
            return (LONG)strlen(out);
        }
        fh = next;
        if (*p == '/') p++;
    }
    *err = ERROR_OBJECT_WRONG_TYPE;     /* no soft link on that path */
    return -1;
}

/* an Amiga link target to Unix syntax: "VOL:a/b" -> "<export>/a/b",
 * "a//b" -> "a/../b", "/x" -> "../x" */
static int amiga_to_unix(const char *t, char *out, u32 max)
{
    const char *c = strchr(t, ':');
    char srv[AMIGA_NAMEMAX * 2 + 2];
    u32 n = 0;

    out[0] = 0;
    if (c) {
        if (strlen(M.export) + 2 > max) return 0;
        strcpy(out, M.export);
        n = strlen(out);
        t = c + 1;
        if (*t && n + 1 < max) out[n++] = '/';
    }
    while (*t) {
        u32 len;
        if (*t == '/') {
            if (n + 4 >= max) return 0;
            memcpy(out + n, "../", 3);
            n += 3;
            t++;
            continue;
        }
        for (len = 0; t[len] && t[len] != '/'; len++)
            ;
        {
            char part[AMIGA_NAMEMAX + 1];
            u32 k;
            if (len > AMIGA_NAMEMAX) return 0;
            memcpy(part, t, len);
            part[len] = 0;
            k = name_to_server(part, srv, sizeof(srv), M.raw);
            if (!k || n + k + 2 >= max) return 0;
            memcpy(out + n, srv, k);
            n += k;
        }
        t += len;
        if (*t == '/') {
            t++;
            if (*t && *t != '/') out[n++] = '/';
            else if (*t == '/') out[n++] = '/';
        }
    }
    if (n > 1 && out[n - 1] == '/') n--;
    out[n] = 0;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* writing                                                                */
/* ---------------------------------------------------------------------- */

/* send what the write-behind buffer holds (UNSTABLE: COMMIT comes at Close) */
static long flush_write(struct NFile *f)
{
    u32 done_ = 0;

    while (done_ < f->wlen) {
        u32 w = 0;
        u8 v[8];
        long e = CALL(B->write(&f->lk->fh, 0, f->woff + done_, f->wbuf + done_,
                                 f->wlen - done_, &w, v));
        if (e)
            return e;
        if (!w)
            return NFS3ERR_IO;
        if (!f->verf_set) {
            memcpy(f->verf, v, 8);
            f->verf_set = 1;
        } else if (memcmp(f->verf, v, 8)) {
            return NFS3ERR_IO;          /* server restarted: earlier unstable data may be gone */
        }
        done_ += w;
        f->unstable = 1;
    }
    if ((LONG)M.cfileid == f->lk->fl.fl_Key)
        M.clen = 0;                     /* the read cache may hold old bytes */
    f->wlen = 0;
    return 0;
}

static long commit_file(struct NFile *f)
{
    u8 v[8];
    long e;

    if (!f->unstable)
        return 0;
    if ((e = CALL(B->commit(&f->lk->fh, v))))
        return e;
    if (f->verf_set && memcmp(f->verf, v, 8))
        return NFS3ERR_IO;              /* the server lost data in a restart */
    f->unstable = 0;
    return 0;
}

/* flush for reads, seeks and examines on a file being written */
static LONG sync_file(struct NFile *f)
{
    long e;

    if (f->werr)
        return f->werr;
    if (f->wlen && (e = flush_write(f)))
        return f->werr = ioerr(e);
    return 0;
}

static LONG do_write(struct NFile *f, const u8 *buf, LONG len, LONG *err)
{
    LONG total = 0;
    long e;

    if (M.readonly) { *err = ERROR_DISK_WRITE_PROTECTED; return -1; }
    if (f->werr)    { *err = f->werr; return -1; }
    if (!f->wbuf && !(f->wbuf = AllocVec(M.wsize, MEMF_PUBLIC))) {
        *err = ERROR_NO_FREE_STORE;
        return -1;
    }
    if (f->wlen && f->pos != f->woff + f->wlen && (e = flush_write(f))) {
        *err = f->werr = ioerr(e);
        return -1;
    }
    if (M.depth > 1 && !f->wlen && (u32)len >= 2 * M.wsize) {
        u32 want = ((u32)len / M.wsize) * M.wsize;
        long k = 0;
        if (B->write_pipe && CALL(0L) == 0 &&
            (k = B->write_pipe(&f->lk->fh, f->pos, buf, want, f->verf, &f->verf_set)) > 0) {
            f->unstable = 1;
            if ((LONG)M.cfileid == f->lk->fl.fl_Key)
                M.clen = 0;
            M.last_act = now_secs();
        }
        if (k < 0) {
            *err = f->werr = ERROR_SEEK_ERROR;  /* server lost unstable data */
            return -1;
        }
        buf += k; len -= k; total += k; f->pos += k;
        if (f->pos > f->size)
            f->size = f->pos;
    }
    while (len > 0) {
        u32 n;

        if (!f->wlen)
            f->woff = f->pos;
        n = M.wsize - f->wlen;
        if (n > (u32)len) n = len;
        CopyMem((APTR)buf, f->wbuf + f->wlen, n);
        f->wlen += n; f->pos += n; buf += n; len -= n; total += n;
        if (f->pos > f->size)
            f->size = f->pos;
        if (f->wlen == M.wsize && (e = flush_write(f))) {
            *err = f->werr = ioerr(e);
            return total ? total : -1;
        }
    }
    return total;
}

/* split "dir/leaf" (or "vol:leaf", "/leaf") and resolve the directory */
static long split_parent(struct NLock *base, const char *name, struct Fh *dir,
                         char *dirpath, char *leaf)
{
    const char *sep = NULL, *p;
    char dirpart[256];
    struct Fattr a;
    u32 n;
    long e;

    for (p = name; *p; p++)
        if (*p == '/' || *p == ':')
            sep = p;
    n = sep ? (u32)(sep - name) + 1 : 0;
    if (n >= sizeof(dirpart) || strlen(name + n) > AMIGA_NAMEMAX)
        return NFS3ERR_NAMETOOLONG;
    memcpy(dirpart, name, n);
    dirpart[n] = 0;
    strcpy(leaf, name + n);
    if (!leaf[0])
        return -ERROR_INVALID_COMPONENT_NAME;
    if ((e = resolve(base, dirpart, dir, &a, dirpath)))
        return e;
    return a.type == NF3DIR ? 0 : NFS3ERR_NOTDIR;
}

/* split_parent() and friends: <= -100 is a negated AmigaDOS error */
static LONG err_of(long e)
{
    return e <= -100 ? -e : ioerr(e);
}

/* does dirpath + "/" + leaf fit a PATHMAX buffer? */
static int path_fits(const char *dirpath, const char *leaf)
{
    return strlen(dirpath) + 1 + strlen(leaf) + 1 <= PATHMAX;
}

static void join_path(char *out, const char *dirpath, const char *leaf)
{
    strcpy(out, dirpath);
    if (out[0]) strcat(out, "/");
    strcat(out, leaf);
}

/* ---------------------------------------------------------------------- */
/* startup                                                                */
/* ---------------------------------------------------------------------- */

static int opt(const char *s, const char *key, const char **val)
{
    u32 n = strlen(key);
    if (strncmp(s, key, n) || s[n] != '=')
        return 0;
    *val = s + n + 1;
    return 1;
}

static u32 num(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static int parse_startup(char *s)
{
    char *tok;
    u32 n;

    while (*s == '"' || *s == ' ' || *s == '\t') s++;
    for (n = strlen(s); n && (s[n - 1] == '"' || s[n - 1] == '\n' || s[n - 1] == ' '); n--)
        s[n - 1] = 0;
    tok = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = 0;
    if (!B->parse_target(tok))
        return 0;

    while (*s) {
        const char *v;
        while (*s == ' ' || *s == '\t') s++;
        tok = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
        if (!*tok)                          continue;
        if (opt(tok, "VOLUME", &v))         strncpy(M.volname, v, sizeof(M.volname) - 1);
        else if (opt(tok, "UID", &v))       M.uid = num(v);
        else if (opt(tok, "GID", &v))       M.gid = num(v);
        else if (opt(tok, "RSIZE", &v))     M.rsize = num(v);
        else if (opt(tok, "WSIZE", &v))     M.wsize = num(v);
        else if (!strcmp(tok, "READONLY"))  M.readonly = 1;
        else if (opt(tok, "TIMEOUT", &v))   M.timeout = num(v);
        else if (opt(tok, "RETRY", &v))     M.retry = num(v);
        else if (opt(tok, "IDLE", &v))      M.idle = num(v);
        else if (opt(tok, "DEPTH", &v))     M.depth = num(v);
        else if (opt(tok, "CHARSET", &v))   M.raw = !strcmp(v, "RAW");
        else if (opt(tok, "TZ", &v)) {
            long sgn = 1, h, m = 0;
            if (*v == '+') v++; else if (*v == '-') { sgn = -1; v++; }
            h = num(v);
            if ((v = strchr(v, ':'))) m = num(v + 1);
            M.tz_east = sgn * (h * 3600 + m * 60);
            M.tz_set = 1;
        }
        else B->option(tok);                /* the protocol's own; unknown ones are ignored */
    }
    return 1;
}

static void reply(struct DosPacket *dp, LONG r1, LONG r2)
{
    struct MsgPort *rp = dp->dp_Port;

    dp->dp_Res1 = r1;
    dp->dp_Res2 = r2;
    dp->dp_Port = M.port;
    PutMsg(rp, dp->dp_Link);
}

/* ---------------------------------------------------------------------- */
/* the packet loop                                                        */
/* ---------------------------------------------------------------------- */

#define LOCK(b) ((struct NLock *)BADDR(b))

void handler_main(void)
{
    struct Process *me;
    struct DosPacket *dp;
    struct Message *msg;
    char startup[300];
    int running = 1;

    SysBase = *(struct ExecBase **)4L;
    /* After ACTION_DIE the next access starts a new process on the same
     * loaded segment: nothing from the last run may survive in the globals */
    memset(&M, 0, sizeof(M));
    me = (struct Process *)FindTask(NULL);
    M.port = &me->pr_MsgPort;
    WaitPort(M.port);
    msg = GetMsg(M.port);
    dp = (struct DosPacket *)msg->mn_Node.ln_Name;

    M.uid = M.gid = 1000;
    M.timeout = 15;
    M.retry = 30;
    M.idle = 60;
    M.depth = 1;
    M.dn = (struct DeviceNode *)BADDR(dp->dp_Arg3);
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 36);
    if (!DOSBase || !UtilityBase) {
        if (DOSBase) CloseLibrary((struct Library *)DOSBase);
        reply(dp, DOSFALSE, ERROR_INVALID_RESIDENT_LIBRARY);
        return;
    }
    if (dp->dp_Arg2 && ((ULONG)dp->dp_Arg2 >> 30) == 0)
        bstr_to_c((BSTR)dp->dp_Arg2, startup, sizeof(startup));
    else
        startup[0] = 0;
    if (!M.rsize) {                     /* RSIZE= not given: fit the machine */
        ULONG free_ = AvailMem(MEMF_ANY);
        M.rsize = free_ < 1024 * 1024 ? 4096 : free_ < 4 * 1024 * 1024 ? 8192 : 32768;
    }
    if (!parse_startup(startup)) {
        reply(dp, DOSFALSE, ERROR_REQUIRED_ARG_MISSING);
        CloseLibrary(UtilityBase);
        CloseLibrary((struct Library *)DOSBase);
        return;
    }
    if (M.rsize < 4096) M.rsize = 4096;
    if (M.rsize > 65536) M.rsize = 65536;
    if (M.depth < 1) M.depth = 1;
    if (M.depth > 4) M.depth = 4;
    if (!M.wsize) M.wsize = M.rsize;
    if (M.wsize < 4096) M.wsize = 4096;
    if (M.wsize > 65536) M.wsize = 65536;
    M.cbuf = alloc(M.rsize);
    if (!M.cbuf || B->init()) {
        if (M.cbuf) FreeVec(M.cbuf);
        reply(dp, DOSFALSE, ERROR_NO_FREE_STORE);
        CloseLibrary(UtilityBase);
        CloseLibrary((struct Library *)DOSBase);
        return;
    }
    /* Packets for this device come to a port of our own, not pr_MsgPort:
     * DOS calls made in this process (bsdsocket.library starting PrismNet on
     * demand, locale.library) wait for their replies on pr_MsgPort, and a
     * client packet arriving there meanwhile is a dead-end alert 07000004. */
    if (!(M.port = CreateMsgPort())) {
        B->cleanup(); FreeVec(M.cbuf);
        M.port = &me->pr_MsgPort;
        reply(dp, DOSFALSE, ERROR_NO_FREE_STORE);
        CloseLibrary(UtilityBase);
        CloseLibrary((struct Library *)DOSBase);
        return;
    }
    if ((M.tport = CreateMsgPort()) &&
        (M.treq = (struct timerequest *)CreateIORequest(M.tport, sizeof(struct timerequest)))) {
        if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)M.treq, 0)) {
            DeleteIORequest((struct IORequest *)M.treq);
            M.treq = NULL;
        }
    }
    M.dn->dn_Task = M.port;             /* later accesses come to this process */
    reply(dp, DOSTRUE, 0);

    while (running) {
        while ((msg = GetMsg(M.port)) != NULL) {
            LONG r1 = DOSFALSE, r2 = 0;
            long e;

            dp = (struct DosPacket *)msg->mn_Node.ln_Name;
            switch (dp->dp_Type) {

            case ACTION_LOCATE_OBJECT: {
                char name[256], path[PATHMAX];
                struct Fh fh;
                struct Fattr a;
                struct NLock *l;
                bstr_to_c((BSTR)dp->dp_Arg2, name, sizeof(name));
                if ((e = resolve(LOCK(dp->dp_Arg1), name, &fh, &a, path))) { r2 = ioerr(e); break; }
                if (!(l = new_lock(&fh, a.type, a.fileid_lo, path, dp->dp_Arg3))) { r2 = ERROR_NO_FREE_STORE; break; }
                r1 = MKBADDR(l);
                break;
            }
            case ACTION_FREE_LOCK:
                free_lock(LOCK(dp->dp_Arg1));
                r1 = DOSTRUE;
                break;

            case ACTION_COPY_DIR:
            case ACTION_COPY_DIR_FH: {
                struct NLock *s = dp->dp_Type == ACTION_COPY_DIR ? LOCK(dp->dp_Arg1)
                                  : ((struct NFile *)dp->dp_Arg1)->lk, *l;
                if (!s) {               /* lock on the root */
                    if ((e = CALL(0L))) { r2 = ioerr(e); break; }
                    l = new_lock(&M.root, NF3DIR, M.rootattr.fileid_lo, "", SHARED_LOCK);
                } else {
                    l = new_lock(&s->fh, s->type, s->fl.fl_Key, s->path, SHARED_LOCK);
                }
                if (!l) { r2 = ERROR_NO_FREE_STORE; break; }
                r1 = MKBADDR(l);
                break;
            }
            case ACTION_PARENT:
            case ACTION_PARENT_FH: {
                struct NLock *s = dp->dp_Type == ACTION_PARENT ? LOCK(dp->dp_Arg1)
                                  : ((struct NFile *)dp->dp_Arg1)->lk, *l;
                char path[PATHMAX], *cut;
                struct Fh fh;
                struct Fattr a;
                int ha;
                if (!s || !s->path[0]) { r1 = 0; r2 = 0; break; }   /* root has no parent */
                strcpy(path, s->path);
                cut = strrchr(path, '/');
                if (cut) *cut = 0; else path[0] = 0;
                if (s->type == NF3DIR) {
                    if ((e = CALL(B->lookup(&s->fh, "..", &fh, &a, &ha))) ||
                        (!ha && (e = CALL(B->getattr(&fh, &a))))) { r2 = ioerr(e); break; }
                } else {
                    /* a file: LOOKUP ".." is for directories only (NOTDIR), so
                     * walk the parent's path down from the root instead */
                    char real[PATHMAX];
                    if (!path[0]) { fh = M.root; a = M.rootattr; }
                    else if ((e = resolve(NULL, path, &fh, &a, real)) < 0 || e) {
                        r2 = ioerr(e); break;
                    }
                }
                if (!(l = new_lock(&fh, NF3DIR, a.fileid_lo, path, SHARED_LOCK))) { r2 = ERROR_NO_FREE_STORE; break; }
                r1 = MKBADDR(l);
                break;
            }
            case ACTION_SAME_LOCK: {
                struct NLock *a = LOCK(dp->dp_Arg1), *b = LOCK(dp->dp_Arg2);
                r1 = (a && b && a->fh.len == b->fh.len && !memcmp(a->fh.data, b->fh.data, a->fh.len))
                     ? DOSTRUE : DOSFALSE;
                break;
            }
            case ACTION_EXAMINE_OBJECT:
            case ACTION_EXAMINE_FH: {
                struct NLock *l = dp->dp_Type == ACTION_EXAMINE_OBJECT ? LOCK(dp->dp_Arg1)
                                  : ((struct NFile *)dp->dp_Arg1)->lk;
                struct FileInfoBlock *fib = BADDR(dp->dp_Arg2);
                if (dp->dp_Type == ACTION_EXAMINE_FH && (r2 = sync_file((struct NFile *)dp->dp_Arg1)))
                    break;
                if (!l) {
                    struct NLock root;
                    if ((e = CALL(0L))) { r2 = ioerr(e); break; }
                    memset(&root, 0, sizeof(root));
                    root.fh = M.root;
                    root.path = (char *)"";
                    root.fl.fl_Key = M.rootattr.fileid_lo;
                    e = examine_lock(&root, fib);
                } else {
                    e = examine_lock(l, fib);
                }
                if (e) { r2 = ioerr(e); break; }
                r1 = DOSTRUE;
                break;
            }
            case ACTION_EXAMINE_NEXT: {
                struct NLock *l = LOCK(dp->dp_Arg1);
                if (!l) { r2 = ERROR_OBJECT_WRONG_TYPE; break; }   /* root: Lock it first */
                e = examine_next(l, BADDR(dp->dp_Arg2));
                if (e) { r2 = ioerr(e); break; }
                r1 = DOSTRUE;
                break;
            }

            case ACTION_FINDINPUT: {
                struct FileHandle *fh = BADDR(dp->dp_Arg1);
                char name[256], path[PATHMAX];
                struct Fh nfh;
                struct Fattr a;
                struct NLock *l;
                struct NFile *f;
                bstr_to_c((BSTR)dp->dp_Arg3, name, sizeof(name));
                if ((e = resolve(LOCK(dp->dp_Arg2), name, &nfh, &a, path))) { r2 = ioerr(e); break; }
                if (a.type == NF3DIR) { r2 = ERROR_OBJECT_WRONG_TYPE; break; }
                if (!(l = new_lock(&nfh, a.type, a.fileid_lo, path, SHARED_LOCK))) { r2 = ERROR_NO_FREE_STORE; break; }
                if (!(f = alloc(sizeof(*f)))) { free_lock(l); r2 = ERROR_NO_FREE_STORE; break; }
                f->lk = l;
                f->size = a.size_hi ? 0xffffffffUL : a.size_lo;
                fh->fh_Arg1 = (LONG)f;
                M.nfiles++;
                r1 = DOSTRUE;
                break;
            }
            case ACTION_FH_FROM_LOCK: {
                struct FileHandle *fh = BADDR(dp->dp_Arg1);
                struct NLock *l = LOCK(dp->dp_Arg2);
                struct NFile *f;
                struct Fattr a;
                if (!l || l->type == NF3DIR) { r2 = ERROR_OBJECT_WRONG_TYPE; break; }
                if ((e = CALL(B->getattr(&l->fh, &a)))) { r2 = ioerr(e); break; }
                if (!(f = alloc(sizeof(*f)))) { r2 = ERROR_NO_FREE_STORE; break; }
                f->lk = l;                      /* the lock now belongs to the file */
                f->size = a.size_hi ? 0xffffffffUL : a.size_lo;
                fh->fh_Arg1 = (LONG)f;
                M.nfiles++;
                r1 = DOSTRUE;
                break;
            }
            case ACTION_READ: {
                struct NFile *f = (struct NFile *)dp->dp_Arg1;
                if ((r2 = sync_file(f))) { r1 = -1; break; }
                r1 = do_read(f, (u8 *)dp->dp_Arg2, dp->dp_Arg3, &r2);
                break;
            }
            case ACTION_WRITE:
                r1 = do_write((struct NFile *)dp->dp_Arg1, (const u8 *)dp->dp_Arg2, dp->dp_Arg3, &r2);
                break;
            case ACTION_SEEK: {
                struct NFile *f = (struct NFile *)dp->dp_Arg1;
                LONG off = dp->dp_Arg2, np;
                if ((r2 = sync_file(f))) { r1 = -1; break; }
                if (dp->dp_Arg3 == OFFSET_END) {
                    struct Fattr a;
                    if ((e = CALL(B->getattr(&f->lk->fh, &a)))) { r1 = -1; r2 = ioerr(e); break; }
                    f->size = a.size_hi ? 0xffffffffUL : a.size_lo;
                    np = (LONG)f->size + off;
                } else if (dp->dp_Arg3 == OFFSET_CURRENT) {
                    np = (LONG)f->pos + off;
                } else {
                    np = off;
                }
                if (np < 0 || (u32)np > f->size) { r1 = -1; r2 = ERROR_SEEK_ERROR; break; }
                r1 = f->pos;
                f->pos = np;
                break;
            }
            case ACTION_END: {
                struct NFile *f = (struct NFile *)dp->dp_Arg1;
                r1 = DOSTRUE;
                if (f) {
                    LONG err = sync_file(f);
                    if (!err && (e = commit_file(f))) err = ioerr(e);
                    if (err) { r1 = DOSFALSE; r2 = err; }
                    if ((LONG)M.cfileid == f->lk->fl.fl_Key) M.clen = 0;
                    if (B->release) B->release(&f->lk->fh);
                    if (f->wbuf) FreeVec(f->wbuf);
                    free_lock(f->lk);
                    FreeVec(f);
                    M.nfiles--;
                }
                break;
            }

            case ACTION_FINDOUTPUT:
            case ACTION_FINDUPDATE: {
                struct FileHandle *fh = BADDR(dp->dp_Arg1);
                char name[256], dirpath[PATHMAX], leaf[AMIGA_NAMEMAX + 1], real[AMIGA_NAMEMAX + 1];
                char srv[AMIGA_NAMEMAX * 2 + 2], path[PATHMAX];
                struct Fh dir, nfh;
                struct Fattr a;
                struct Sattr sa;
                struct NLock *l;
                struct NFile *f;
                int ha = 0;
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                bstr_to_c((BSTR)dp->dp_Arg3, name, sizeof(name));
                if ((e = split_parent(LOCK(dp->dp_Arg2), name, &dir, dirpath, leaf))) { r2 = err_of(e); break; }
                if (!path_fits(dirpath, leaf)) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                memset(&sa, 0, sizeof(sa));
                e = lookup_name(&dir, leaf, &nfh, &a, &ha, real);
                if (e == 0) {                   /* it exists (perhaps in another case) */
                    if (!ha && (e = CALL(B->getattr(&nfh, &a)))) { r2 = ioerr(e); break; }
                    if (a.type == NF3DIR) { r2 = ERROR_OBJECT_WRONG_TYPE; break; }
                    if (a.type == NF3LNK) { r2 = ERROR_IS_SOFT_LINK; break; }   /* DOS follows it and retries */
                    if (dp->dp_Type == ACTION_FINDOUTPUT) {
                        sa.set_size = 1;
                        if ((e = CALL(B->setattr(&nfh, &sa)))) { r2 = ioerr(e); break; }
                        a.size_hi = a.size_lo = 0;
                    }
                } else if (e == NFS3ERR_NOENT) {
                    sa.set_mode = 1; sa.mode = 0644;
                    sa.set_size = 1;
                    if (!name_to_server(leaf, srv, sizeof(srv), M.raw)) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                    if ((e = CALL(B->create(&dir, srv, 1, &sa, &nfh, &a, &ha)))) { r2 = ioerr(e); break; }
                    if (!ha && (e = CALL(B->getattr(&nfh, &a)))) { r2 = ioerr(e); break; }
                    strcpy(real, leaf);
                } else {
                    r2 = ioerr(e);
                    break;
                }
                if (!path_fits(dirpath, real)) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                join_path(path, dirpath, real);
                if (!(l = new_lock(&nfh, NF3REG, a.fileid_lo, path, SHARED_LOCK))) { r2 = ERROR_NO_FREE_STORE; break; }
                if (!(f = alloc(sizeof(*f)))) { free_lock(l); r2 = ERROR_NO_FREE_STORE; break; }
                f->lk = l;
                f->size = a.size_hi ? 0xffffffffUL : a.size_lo;
                fh->fh_Arg1 = (LONG)f;
                M.nfiles++;
                r1 = DOSTRUE;
                break;
            }
            case ACTION_SET_FILE_SIZE: {
                struct NFile *f = (struct NFile *)dp->dp_Arg1;
                struct Sattr sa;
                LONG ns;
                r1 = -1;
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                if ((r2 = sync_file(f))) break;
                ns = dp->dp_Arg3 == OFFSET_END ? (LONG)f->size + dp->dp_Arg2
                   : dp->dp_Arg3 == OFFSET_CURRENT ? (LONG)f->pos + dp->dp_Arg2 : dp->dp_Arg2;
                if (ns < 0) { r2 = ERROR_SEEK_ERROR; break; }
                memset(&sa, 0, sizeof(sa));
                sa.set_size = 1; sa.size = ns;
                if ((e = CALL(B->setattr(&f->lk->fh, &sa)))) { r2 = ioerr(e); break; }
                if ((LONG)M.cfileid == f->lk->fl.fl_Key) M.clen = 0;
                f->size = ns;
                if (f->pos > (u32)ns) f->pos = ns;
                r1 = ns;
                r2 = 0;
                break;
            }
            case ACTION_DELETE_OBJECT: {
                char name[256], dirpath[PATHMAX], leaf[AMIGA_NAMEMAX + 1], real[AMIGA_NAMEMAX + 1];
                char srv[AMIGA_NAMEMAX * 2 + 2];
                struct Fh dir, ofh;
                struct Fattr a;
                int ha = 0;
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                bstr_to_c((BSTR)dp->dp_Arg2, name, sizeof(name));
                if ((e = split_parent(LOCK(dp->dp_Arg1), name, &dir, dirpath, leaf)) ||
                    (e = lookup_name(&dir, leaf, &ofh, &a, &ha, real)) ||
                    (!ha && (e = CALL(B->getattr(&ofh, &a))))) { r2 = err_of(e); break; }
                name_to_server(real, srv, sizeof(srv), M.raw);
                e = a.type == NF3DIR ? CALL(B->rmdir(&dir, srv)) : CALL(B->remove(&dir, srv));
                if (e) { r2 = ioerr(e); break; }
                r1 = DOSTRUE;
                break;
            }
            case ACTION_RENAME_OBJECT: {
                char fname[256], tname[256], fdpath[PATHMAX], tdpath[PATHMAX];
                char fleaf[AMIGA_NAMEMAX + 1], tleaf[AMIGA_NAMEMAX + 1];
                char freal[AMIGA_NAMEMAX + 1], treal[AMIGA_NAMEMAX + 1];
                char fsrv[AMIGA_NAMEMAX * 2 + 2], tsrv[AMIGA_NAMEMAX * 2 + 2];
                struct Fh fdir, tdir, ofh, tfh;
                struct Fattr a, ta;
                int ha = 0, tha = 0;
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                bstr_to_c((BSTR)dp->dp_Arg2, fname, sizeof(fname));
                bstr_to_c((BSTR)dp->dp_Arg4, tname, sizeof(tname));
                if ((e = split_parent(LOCK(dp->dp_Arg1), fname, &fdir, fdpath, fleaf)) ||
                    (e = lookup_name(&fdir, fleaf, &ofh, &a, &ha, freal)) ||
                    (!ha && (e = CALL(B->getattr(&ofh, &a)))) ||
                    (e = split_parent(LOCK(dp->dp_Arg3), tname, &tdir, tdpath, tleaf))) { r2 = err_of(e); break; }
                e = lookup_name(&tdir, tleaf, &tfh, &ta, &tha, treal);
                if (e == 0) {                   /* AmigaDOS never replaces: only a change of case */
                    if (!tha) CALL(B->getattr(&tfh, &ta));
                    if (ta.fileid_lo != a.fileid_lo || ta.fileid_hi != a.fileid_hi) { r2 = ERROR_OBJECT_EXISTS; break; }
                } else if (e != NFS3ERR_NOENT) {
                    r2 = ioerr(e);
                    break;
                }
                name_to_server(freal, fsrv, sizeof(fsrv), M.raw);
                if (!name_to_server(tleaf, tsrv, sizeof(tsrv), M.raw)) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                if ((e = CALL(B->rename(&fdir, fsrv, &tdir, tsrv)))) { r2 = ioerr(e); break; }
                r1 = DOSTRUE;
                break;
            }
            case ACTION_CREATE_DIR: {
                char name[256], dirpath[PATHMAX], leaf[AMIGA_NAMEMAX + 1], path[PATHMAX];
                char srv[AMIGA_NAMEMAX * 2 + 2], real[AMIGA_NAMEMAX + 1];
                struct Fh dir, nfh;
                struct Fattr a;
                struct Sattr sa;
                struct NLock *l;
                int ha = 0;
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                bstr_to_c((BSTR)dp->dp_Arg2, name, sizeof(name));
                if ((e = split_parent(LOCK(dp->dp_Arg1), name, &dir, dirpath, leaf))) { r2 = err_of(e); break; }
                if (!path_fits(dirpath, leaf)) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                if (lookup_name(&dir, leaf, &nfh, &a, &ha, real) == 0) { r2 = ERROR_OBJECT_EXISTS; break; }
                memset(&sa, 0, sizeof(sa));
                sa.set_mode = 1; sa.mode = 0755;
                if (!name_to_server(leaf, srv, sizeof(srv), M.raw)) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                if ((e = CALL(B->mkdir(&dir, srv, &sa, &nfh, &a, &ha)))) { r2 = ioerr(e); break; }
                if (!ha && (e = CALL(B->getattr(&nfh, &a)))) { r2 = ioerr(e); break; }
                join_path(path, dirpath, leaf);
                if (!(l = new_lock(&nfh, NF3DIR, a.fileid_lo, path, EXCLUSIVE_LOCK))) { r2 = ERROR_NO_FREE_STORE; break; }
                r1 = MKBADDR(l);
                break;
            }
            case ACTION_SET_PROTECT:
            case ACTION_SET_DATE: {
                char name[256], path[PATHMAX];
                struct Fh ofh;
                struct Fattr a;
                struct Sattr sa;
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                bstr_to_c((BSTR)dp->dp_Arg3, name, sizeof(name));
                if ((e = resolve(LOCK(dp->dp_Arg2), name, &ofh, &a, path))) { r2 = ioerr(e); break; }
                memset(&sa, 0, sizeof(sa));
                if (dp->dp_Type == ACTION_SET_PROTECT) {
                    LONG m = dp->dp_Arg4;
                    u32 mode = a.mode & ~0700UL;
                    if (!(m & FIBF_READ))  mode |= 0400;
                    if (!(m & FIBF_WRITE)) mode |= 0200;
                    if (a.type == NF3DIR)       /* E means nothing for drawers: keep x */
                        mode |= a.mode & 0100;
                    else if (!(m & FIBF_EXECUTE))
                        mode |= 0100;
                    if (m & 0xff00) {           /* group/other bits given: use them */
                        mode &= ~077UL;
                        if (m & FIBF_GRP_READ)    mode |= 040;
                        if (m & FIBF_GRP_WRITE)   mode |= 020;
                        if (m & FIBF_GRP_EXECUTE) mode |= 010;
                        if (m & FIBF_OTR_READ)    mode |= 04;
                        if (m & FIBF_OTR_WRITE)   mode |= 02;
                        if (m & FIBF_OTR_EXECUTE) mode |= 01;
                    }
                    sa.set_mode = 1;
                    sa.mode = mode;
                } else {
                    struct DateStamp *ds = (struct DateStamp *)dp->dp_Arg4;
                    long secs = ds->ds_Days * 86400L + ds->ds_Minute * 60L + ds->ds_Tick / 50
                              + EPOCH_DIFF - M.tz_east;
                    sa.set_mtime = 1;
                    sa.mtime = secs < 0 ? 0 : (u32)secs;
                }
                if ((e = CALL(B->setattr(&ofh, &sa)))) { r2 = ioerr(e); break; }
                r1 = DOSTRUE;
                break;
            }
            case ACTION_SET_COMMENT: {
                u8 *c = BADDR(dp->dp_Arg4);     /* NFS has no comments: only "no comment" works */
                if (c && c[0]) { r2 = ERROR_ACTION_NOT_KNOWN; break; }
                r1 = DOSTRUE;
                break;
            }

            case ACTION_DISK_INFO:
                fill_info(BADDR(dp->dp_Arg1));
                r1 = DOSTRUE;
                break;
            case ACTION_INFO:
                if ((e = CALL(0L))) { r2 = ioerr(e); break; }
                fill_info(BADDR(dp->dp_Arg2));
                r1 = DOSTRUE;
                break;
            case ACTION_CURRENT_VOLUME:
                r1 = MKBADDR(M.vol);
                break;
            case ACTION_IS_FILESYSTEM:
            case ACTION_FLUSH:
            case ACTION_CHANGE_MODE:
                r1 = DOSTRUE;
                break;

            case ACTION_EXAMINE_ALL:
                r1 = examine_all(LOCK(dp->dp_Arg1), (u8 *)dp->dp_Arg2, dp->dp_Arg3, dp->dp_Arg4,
                                 (struct ExAllControl *)dp->dp_Arg5, &r2);
                break;
            case ACTION_EXAMINE_ALL_END: {
                struct NLock *l = LOCK(dp->dp_Arg1);
                if (l && l->dir) { FreeVec(l->dir); l->dir = NULL; }
                r1 = DOSTRUE;
                break;
            }
            case ACTION_READ_LINK:
                r1 = read_link(LOCK(dp->dp_Arg1), (const char *)dp->dp_Arg2,
                               (char *)dp->dp_Arg3, dp->dp_Arg4, &r2);
                break;
            case ACTION_MAKE_LINK: {
                char name[256], dirpath[PATHMAX], leaf[AMIGA_NAMEMAX + 1], real[AMIGA_NAMEMAX + 1];
                char srv[AMIGA_NAMEMAX * 2 + 2], target[512];
                struct Fh dir, nfh;
                struct Fattr a;
                int ha = 0;
                if (dp->dp_Arg4 != LINK_SOFT) { r2 = ERROR_ACTION_NOT_KNOWN; break; }   /* no hard links */
                if (M.readonly) { r2 = ERROR_DISK_WRITE_PROTECTED; break; }
                bstr_to_c((BSTR)dp->dp_Arg2, name, sizeof(name));
                if ((e = split_parent(LOCK(dp->dp_Arg1), name, &dir, dirpath, leaf))) { r2 = err_of(e); break; }
                if (lookup_name(&dir, leaf, &nfh, &a, &ha, real) == 0) { r2 = ERROR_OBJECT_EXISTS; break; }
                if (!name_to_server(leaf, srv, sizeof(srv), M.raw) ||
                    !amiga_to_unix((const char *)dp->dp_Arg3, target, sizeof(target))) { r2 = ERROR_INVALID_COMPONENT_NAME; break; }
                if ((e = CALL(B->symlink(&dir, srv, target, &nfh, &a, &ha)))) { r2 = ioerr(e); break; }
                r1 = DOSTRUE;
                break;
            }
            case ACTION_RENAME_DISK:
                r2 = ERROR_ACTION_NOT_KNOWN;
                break;

            case ACTION_DIE:
                if (M.nlocks > 0) { r2 = ERROR_OBJECT_IN_USE; break; }
                B->drop();
                remove_volume();
                M.dn->dn_Task = NULL;
                running = 0;
                r1 = DOSTRUE;
                break;

            default:
                r2 = ERROR_ACTION_NOT_KNOWN;
                break;
            }
            reply(dp, r1, r2);
            if (!running)
                break;
        }
        if (!running)
            break;
        /* IDLE=: with no open files, drop the connection (and our hold on
         * bsdsocket.library) after a quiet spell; the next access reconnects */
        if (M.treq && M.idle && B->connected() && !M.nfiles && !M.tpending) {
            M.treq->tr_node.io_Command = TR_ADDREQUEST;
            M.treq->tr_time.tv_secs = M.idle;
            M.treq->tr_time.tv_micro = 0;
            SendIO((struct IORequest *)M.treq);
            M.tpending = 1;
        }
        Wait((1UL << M.port->mp_SigBit) | (M.tport ? 1UL << M.tport->mp_SigBit : 0));
        if (M.tpending && CheckIO((struct IORequest *)M.treq)) {
            WaitIO((struct IORequest *)M.treq);
            M.tpending = 0;
            if (B->connected() && !M.nfiles && now_secs() - M.last_act >= M.idle) {
                DBG("idle: disconnect");
                B->drop();
                net_down();
            }
        }
    }

    if (M.treq) {
        if (M.tpending) {
            AbortIO((struct IORequest *)M.treq);
            WaitIO((struct IORequest *)M.treq);
        }
        CloseDevice((struct IORequest *)M.treq);
        DeleteIORequest((struct IORequest *)M.treq);
    }
    if (M.tport) DeleteMsgPort(M.tport);
    B->drop();
    net_down();
    B->cleanup();
    FreeVec(M.cbuf);
    DeleteMsgPort(M.port);
    CloseLibrary(UtilityBase);
    CloseLibrary((struct Library *)DOSBase);
}
