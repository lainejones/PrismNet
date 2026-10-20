/*
 * PrismNet SMB client: the SMB2 backend of the shared network-filesystem
 * core (tools/netfs/core.c). Read/write (stage 2). See docs/SMBCLIENT.md.
 * GPL-3.0-only, see LICENSE.
 *
 * Startup:  "host/share[/path] [options]"; SMB options: USER=name
 * PASSWORD=secret DOMAIN=name GUEST SIGN=YES|AUTO
 * ENCRYPT=AUTO|YES|NO MAXPROTO=2.1|3.0|3.0.2|3.1.1 PORT=n (the rest are the core's, e.g.
 * READONLY).
 *
 * Handles are paths, so a lock inside a drawer that is renamed while the
 * lock is held goes stale (NFS handles survive renames; SMB ones do not).
 *
 * A handle (struct Fh) holds the object's path on the share (UTF-8, '\'
 * separated, "" = the share root); the core is built with FS_FHSIZE=260.
 * Files being read keep a server handle in a small cache, closed by
 * release(); directory listings keep theirs (and the unread rest of the
 * last QUERY_DIRECTORY reply) in a slot per listing.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "netfs.h"
#include "smb2.h"
#include "passreq.h"

#define NOPEN   4                       /* files with a server handle kept open */
#define NDIR    2                       /* directory listings in progress */

static struct Smb smb;
static u8 *tx, *rx;
static unsigned port;
static char basepath[FS_FHSIZE];

static struct Open {
    char path[FS_FHSIZE];
    struct Smb2Id id;
    u32 used;                           /* LRU stamp; 0 = free */
    int rw;                             /* opened for writing */
} opens[NOPEN];

static struct DirSlot {
    char path[FS_FHSIZE];
    struct Smb2Id id;
    u8  *buf;                           /* copy of the last reply's entries */
    u32  len, pos, seq;
    u32  last;                          /* use counter, for picking the oldest slot */
    int  open, eof;
} dirs[NDIR];
static u32 stamp, dirbufsize;

/* The last few folders listed in full, kept for a few seconds so the lookups
 * that follow a listing (Workbench fetching every icon, a program checking each
 * name) are answered without asking the server - "not there" included, since
 * the listing was complete. Several, because Workbench lists other folders
 * (the volume's root, for its Disk.info) in between. Any change made through
 * this handler drops them all. Each folder's entries sit in one buffer that
 * grows as the listing comes in: [u32 record length][Fattr][name\0], longword
 * aligned. */
#define LC_SLOTS 3
#define LC_FIRST 4096                   /* first buffer; doubles as needed */
#define LC_BIG   65536                  /* a folder bigger than this is not kept */
#define LC_SECS  10                     /* how long a listing is trusted */
static struct LSlot {
    char dir[FS_FHSIZE];
    u8  *pool;
    u32  size, used, t;
    int  state;                         /* 0 none, 1 being built, 2 complete */
} lcs[LC_SLOTS];

/* Recently looked-up paths: every open resolves its path one name at a time
 * (Amiga, then Amiga\Games, then the file), so the folders on the way would
 * otherwise cost a round trip each, for every file. Same lifetime as lcs. */
#define AC_N 16
static struct { char path[FS_FHSIZE]; struct Fattr a; u32 t, use; } ac[AC_N];
static u32 ac_clock;

static int ac_find(const char *path, struct Fattr *a)
{
    int i;
    for (i = 0; i < AC_N; i++)
        if (ac[i].use && !strcmp(ac[i].path, path)) {
            if (now_secs() - ac[i].t > LC_SECS) { ac[i].use = 0; return 0; }
            *a = ac[i].a;
            ac[i].use = ++ac_clock;
            return 1;
        }
    return 0;
}

static void ac_put(const char *path, const struct Fattr *a)
{
    int i, old = 0;
    for (i = 0; i < AC_N; i++) {
        if (ac[i].use && !strcmp(ac[i].path, path)) { old = i; break; }
        if (ac[i].use < ac[old].use) old = i;
    }
    strcpy(ac[old].path, path);
    ac[old].a = *a;
    ac[old].t = now_secs();
    ac[old].use = ++ac_clock;
}

static void lc_drop(void)
{
    int i;
    for (i = 0; i < LC_SLOTS; i++) lcs[i].state = 0;
    for (i = 0; i < AC_N; i++) ac[i].use = 0;
}

static void lc_free(void)
{
    int i;
    for (i = 0; i < LC_SLOTS; i++) {
        if (lcs[i].pool) FreeVec(lcs[i].pool);
        lcs[i].pool = NULL;
        lcs[i].size = lcs[i].used = 0;
        lcs[i].state = 0;
    }
}

static struct LSlot *lc_slot(const char *dir, int state)
{
    int i;
    for (i = 0; i < LC_SLOTS; i++)
        if (lcs[i].state == state && !strcmp(lcs[i].dir, dir)) return &lcs[i];
    return NULL;
}

/* a listing of dir starts from its first entry */
static void lc_begin(const char *dir)
{
    struct LSlot *l = lc_slot(dir, 1), *x;
    int i;
    if (!l && (x = lc_slot(dir, 2))) x->state = 0;      /* the old copy is replaced */
    if (!l) {                           /* a free slot, else the oldest */
        l = &lcs[0];
        for (i = 0; i < LC_SLOTS; i++) {
            if (!lcs[i].state) { l = &lcs[i]; break; }
            if (lcs[i].t < l->t) l = &lcs[i];
        }
    }
    strcpy(l->dir, dir);
    l->used = 0;
    l->t = now_secs();
    l->state = 1;
}

static void lc_add(const char *dir, const char *name, const struct Fattr *a)
{
    struct LSlot *l = lc_slot(dir, 1);
    u32 need = (4 + sizeof(struct Fattr) + strlen(name) + 1 + 3) & ~3UL;
    if (!l) return;
    if (l->used + need > l->size) {     /* grow, or give up on a huge folder */
        u32 ns = l->size ? l->size * 2 : LC_FIRST;
        u8 *np;
        while (ns < l->used + need) ns *= 2;
        if (ns > LC_BIG || !(np = AllocVec(ns, MEMF_PUBLIC))) { l->state = 0; return; }
        if (l->pool) { CopyMem(l->pool, np, l->used); FreeVec(l->pool); }
        l->pool = np;
        l->size = ns;
    }
    *(u32 *)(l->pool + l->used) = need;
    CopyMem((APTR)a, l->pool + l->used + 4, sizeof(struct Fattr));
    strcpy((char *)l->pool + l->used + 4 + sizeof(struct Fattr), name);
    l->used += need;
}

static void lc_done(const char *dir)    /* the listing reached its end */
{
    struct LSlot *l = lc_slot(dir, 1);
    if (l) { l->state = 2; l->t = now_secs(); }
}

static void lc_abort(const char *dir)   /* a listing that did not run from the start */
{
    struct LSlot *l = lc_slot(dir, 1);
    if (l) l->state = 0;
}

static int lc_same(const char *a, const char *b)  /* names match as the Amiga sees them */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* 1 found (*a, *real), 0 known not to exist, -1 ask the server */
static int lc_find(const char *dir, const char *name, struct Fattr *a, const char **real)
{
    struct LSlot *l = lc_slot(dir, 2);
    u32 off;
    if (!l) return -1;
    if (now_secs() - l->t > LC_SECS) { l->state = 0; return -1; }
    for (off = 0; off < l->used; off += *(u32 *)(l->pool + off)) {
        const char *nm = (const char *)l->pool + off + 4 + sizeof(struct Fattr);
        if (lc_same(nm, name)) {
            CopyMem(l->pool + off + 4, a, sizeof(struct Fattr));
            *real = nm;
            return 1;
        }
    }
    return 0;
}

static const char *P(const struct Fh *fh) { return (const char *)fh->data; }

static void set_fh(struct Fh *fh, const char *path)
{
    strncpy((char *)fh->data, path, FS_FHSIZE - 1);
    fh->data[FS_FHSIZE - 1] = 0;
    fh->len = strlen((char *)fh->data) + 1;
}

/* ---- startup ------------------------------------------------------------- */

/* "host/share[/path]" (also "//host/share"); resets this backend's state:
 * after ACTION_DIE the next access starts a new process on this segment */
static int smb_parse_target(const char *tok)
{
    const char *p = tok, *slash, *rest;
    u32 n;

    memset(&smb, 0, sizeof(smb));
    smb.sock = -1;
    memset(opens, 0, sizeof(opens));
    memset(dirs, 0, sizeof(dirs));
    tx = rx = NULL;
    port = 0;
    basepath[0] = 0;
    stamp = 0;
    while (*p == '/' || *p == '\\') p++;
    if (!(slash = strpbrk(p, "/\\")) || slash == p || !slash[1])
        return 0;
    n = slash - p;
    if (n >= sizeof(M.host)) n = sizeof(M.host) - 1;
    memcpy(M.host, p, n);
    M.host[n] = 0;
    rest = slash + 1;
    n = strcspn(rest, "/\\");
    if (n >= sizeof(M.export)) n = sizeof(M.export) - 1;
    memcpy(M.export, rest, n);
    M.export[n] = 0;
    if (rest[n]) {                      /* a folder inside the share */
        const char *q = rest + n + 1;
        char *o = basepath;
        while (*q && o < basepath + sizeof(basepath) - 1) {
            *o++ = *q == '/' ? '\\' : *q;
            q++;
        }
        *o = 0;
        if (o > basepath && o[-1] == '\\') o[-1] = 0;
    }
    strncpy(M.volname, M.export, sizeof(M.volname) - 1);
    return 1;
}

static char opt_password[64];       /* PASSWORD= from the mountlist */
static int  pw_from_store;          /* the current password came from ENVARC: */
static int  pw_pending_save;        /* typed in the window: save after it worked */
static int  remember = 1;           /* the window's checkbox */

/* Saved passwords: one file per server and user, ENVARC:PrismNet/smb/<host>.<user>
 * (and the ENV: copy). An Amiga is one person's machine: plain files. */
static void store_name(char *out, unsigned max, const char *arc)
{
    unsigned n = 0;
    const char *s;
    strncpy(out, arc, max - 1); out[max - 1] = 0;
    n = strlen(out);
    if (n < max - 2) out[n++] = '/';
    for (s = M.host; *s && n < max - 2; s++) out[n++] = *s == '/' ? '_' : *s;
    if (n < max - 2) out[n++] = '.';
    for (s = smb.user; *s && n < max - 2; s++) out[n++] = *s == '/' ? '_' : *s;
    out[n] = 0;
}

static int load_password(void)
{
    char name[200];
    BPTR f;
    LONG n = 0;

    store_name(name, sizeof(name), "ENV:PrismNet/smb");
    if (!(f = Open((CONST_STRPTR)name, MODE_OLDFILE))) {
        store_name(name, sizeof(name), "ENVARC:PrismNet/smb");
        f = Open((CONST_STRPTR)name, MODE_OLDFILE);
    }
    if (!f) {
        DBG("password: no %s (IoErr %ld)", name, (long)IoErr());
        return 0;
    }
    n = Read(f, smb.password, sizeof(smb.password) - 1);
    Close(f);
    DBG("password: read %ld bytes from %s", (long)n, name);
    if (n < 0) n = 0;
    smb.password[n] = 0;
    while (n > 0 && (smb.password[n - 1] == '\n' || smb.password[n - 1] == '\r')) smb.password[--n] = 0;
    return n > 0;
}

static void save_password(int keep)
{
    /* no trailing slash on the directory names: "dir/" is the parent to Lock() */
    static const char *const dirs2[2] = { "ENVARC:PrismNet/smb", "ENV:PrismNet/smb" };
    char name[200];
    int i;

    for (i = 0; i < 2; i++) {
        BPTR f, d;
        store_name(name, sizeof(name), dirs2[i]);
        if (!keep) { DeleteFile((CONST_STRPTR)name); continue; }
        if ((d = Lock((CONST_STRPTR)dirs2[i], SHARED_LOCK))) UnLock(d);
        else if ((d = CreateDir((CONST_STRPTR)dirs2[i]))) UnLock(d);
        if ((f = Open((CONST_STRPTR)name, MODE_NEWFILE))) {
            Write(f, smb.password, strlen(smb.password));
            Write(f, "\n", 1);
            Close(f);
        }
    }
}

static int smb_option(const char *tok)
{
    if (!strncmp(tok, "USER=", 5))   { strncpy(smb.user, tok + 5, sizeof(smb.user) - 1); return 1; }
    if (!strncmp(tok, "PASSWORD=", 9)) { strncpy(opt_password, tok + 9, sizeof(opt_password) - 1); return 1; }
    if (!strncmp(tok, "DOMAIN=", 7)) { strncpy(smb.domain, tok + 7, sizeof(smb.domain) - 1); return 1; }
    if (!strcmp(tok, "GUEST"))       { smb.want_guest = 1; return 1; }
    if (!strncmp(tok, "SIGN=", 5))   { smb.sign_required_by_us = !strcmp(tok + 5, "YES"); return 1; }
    if (!strncmp(tok, "ENCRYPT=", 8)) {
        smb.encrypt_mode = !strcmp(tok + 8, "YES") ? 1 : !strcmp(tok + 8, "NO") ? -1 : 0;
        return 1;
    }
    if (!strncmp(tok, "MAXPROTO=", 9)) {        /* 2.0.2 2.1 3.0 3.0.2 3.1.1 */
        const char *v = tok + 9;
        smb.max_dialect = !strcmp(v, "2.0.2") ? 0x202 : !strcmp(v, "2.1") ? 0x210 :
                          !strcmp(v, "3.0") ? 0x300 : !strcmp(v, "3.0.2") ? 0x302 : 0x311;
        return 1;
    }
    if (!strncmp(tok, "PORT=", 5)) {
        const char *s = tok + 5;
        port = 0;
        while (*s >= '0' && *s <= '9') port = port * 10 + (*s++ - '0');
        return 1;
    }
    return 0;
}

static int smb_init(void)
{
    /* sized to the machine: a 2 MB Amiga has ~190 KB left after Workbench
     * and PrismNet. Requests are small while read-only; replies are at most
     * one RSIZE read (or a QUERY_DIRECTORY of that size) plus headers. */
    u32 txs = M.readonly ? 2048 : M.wsize + 1024;    /* both leave room for TXH */
    u32 rxs = M.rsize + 1024;

    dirbufsize = rxs;                   /* listing buffers come on first use */
    tx = AllocVec(txs, MEMF_PUBLIC | MEMF_CLEAR);
    rx = AllocVec(rxs, MEMF_PUBLIC | MEMF_CLEAR);
    if (!tx || !rx)
        return -1;
    {
        char user[64], domain[32];
        int g = smb.want_guest, sr = smb.sign_required_by_us, em = smb.encrypt_mode;
        u32 md = smb.max_dialect;
        strcpy(user, smb.user); strcpy(domain, smb.domain);
        smb2_init(&smb, tx, txs, rx, rxs);
        strcpy(smb.user, user); strcpy(smb.domain, domain);
        smb.want_guest = g; smb.sign_required_by_us = sr;
        smb.encrypt_mode = em; smb.max_dialect = md;
    }
    smb.timeout = M.timeout;
    return 0;
}

static void forget_handles(void)
{
    int i;
    for (i = 0; i < NOPEN; i++) opens[i].used = 0;
    for (i = 0; i < NDIR; i++) { dirs[i].open = 0; dirs[i].path[0] = 0; dirs[i].len = dirs[i].pos = 0; }
    lc_drop();                          /* the share may have changed meanwhile */
}

static void smb_cleanup(void)
{
    int i;
    smb2_close(&smb);
    memset(smb.password, 0, sizeof(smb.password));
    if (tx) FreeVec(tx);
    if (rx) FreeVec(rx);
    for (i = 0; i < NDIR; i++) { if (dirs[i].buf) FreeVec(dirs[i].buf); dirs[i].buf = NULL; }
    lc_free();
    tx = rx = NULL;
}

/* ---- connection ------------------------------------------------------------ */

static long smb_connect(void)
{
    u32 ip;
    long e;

    forget_handles();                   /* server handles die with the connection */
    if (!(ip = net_resolve(M.host)))
        return NET_EHOST;
    if (smb.user[0] && !smb.want_guest && !smb.password[0]) {
        /* PASSWORD= in the mountlist, else the saved one, else the window */
        DBG("password: none in memory (user %s, option %s)", smb.user, opt_password[0] ? "set" : "unset");
        if (opt_password[0]) {
            strcpy(smb.password, opt_password);
            pw_from_store = 0;
        } else if (load_password()) {
            pw_from_store = 1;
        } else {
            char title[160];
            strcpy(title, "Password for ");
            strncat(title, smb.user, 40);
            strcat(title, " on ");
            strncat(title, M.host, 40);
            strcat(title, "/");
            strncat(title, M.export, 40);
            remember = 1;
            if (!ask_password(title, smb.password, sizeof(smb.password), &remember))
                return FS_EDENIED;      /* closed: no session */
            pw_from_store = 0;
            pw_pending_save = 1;        /* saved once it has worked */
        }
    }
    DBG("smb: connect");
    if ((e = smb2_connect(&smb, ip, port, M.host, M.export))) {
        DBG("smb: connect failed %ld status %lx", e, (unsigned long)smb.ntstatus);
        if (e == FS_EDENIED) {          /* wrong password: ask again next time */
            memset(smb.password, 0, sizeof(smb.password));
            if (pw_from_store) save_password(0);   /* the saved one is stale */
            pw_from_store = pw_pending_save = 0;
        }
        return e;
    }
    if (pw_pending_save) {
        save_password(remember);        /* it worked: keep it (or drop an old one) */
        pw_pending_save = 0;
    }
    set_fh(&M.root, basepath);
    if (!M.mounted) {                   /* credit charge 1: transfers up to 64 KB */
        u32 lim = 65536;
        if (smb.maxread && smb.maxread < lim) lim = smb.maxread;
        if (M.rsize > lim) M.rsize = lim;
        lim = 65536;
        if (smb.maxwrite && smb.maxwrite < lim) lim = smb.maxwrite;
        if (M.wsize > lim) M.wsize = lim;
    }
    return 0;
}

static void smb_drop(void)
{
    smb2_close(&smb);
    forget_handles();
}

static int smb_connected(void) { return smb.sock >= 0; }

/* ---- reading ------------------------------------------------------------------ */

static long smb_getattr(const struct Fh *fh, struct Fattr *a)
{
    long e;
    if (ac_find(P(fh), a))
        return 0;
    DBG("srv getattr %s", P(fh));
    if (!(e = smb2_getattr(&smb, P(fh), a))) {
        a->fileid_lo = smb2_fnv(P(fh));
        ac_put(P(fh), a);
    }
    return e;
}

static long smb_lookup(const struct Fh *dir, const char *name, struct Fh *out,
                       struct Fattr *a, int *ha)
{
    char path[FS_FHSIZE];
    u32 dl = strlen(P(dir));
    long e;

    if (strcmp(name, "..")) {           /* from the last listing, if it is this folder */
        const char *real;
        int k = lc_find(P(dir), name, a, &real);
        if (k == 0)
            return NFS3ERR_NOENT;
        if (k == 1 && dl + 1 + strlen(real) + 1 <= FS_FHSIZE) {
            strcpy(path, P(dir));
            if (dl) strcat(path, "\\");
            strcat(path, real);
            a->fileid_lo = smb2_fnv(path);
            ac_put(path, a);            /* the Examine that usually follows */
            set_fh(out, path);
            *ha = 1;
            return 0;
        }
    }
    if (!strcmp(name, "..")) {          /* the parent is just a shorter path */
        char *cut;
        strcpy(path, P(dir));
        cut = strrchr(path, '\\');
        if (cut) *cut = 0; else path[0] = 0;
    } else {
        if (dl + 1 + strlen(name) + 1 > FS_FHSIZE)
            return NFS3ERR_NAMETOOLONG;
        strcpy(path, P(dir));
        if (dl) strcat(path, "\\");
        strcat(path, name);
    }
    if (!ac_find(path, a)) {
        DBG("srv lookup %s", path);
        if ((e = smb2_getattr(&smb, path, a)))
            return e;
        a->fileid_lo = smb2_fnv(path);
        ac_put(path, a);
    }
    set_fh(out, path);
    *ha = 1;
    return 0;
}

/* a cached server handle for a file, opened for writing too if rw */
static struct Open *open_for(const struct Fh *fh, int rw, long *err)
{
    struct Open *o, *lru = &opens[0];
    int i;

    *err = 0;
    for (i = 0; i < NOPEN; i++) {
        o = &opens[i];
        if (o->used && !strcmp(o->path, P(fh))) {
            if (rw && !o->rw) {         /* read-only handle: reopen for writing */
                smb2_closefile(&smb, &o->id);
                o->used = 0;
                lru = o;
                break;
            }
            o->used = ++stamp;
            return o;
        }
        if (!o->used || o->used < lru->used) lru = o;
    }
    if (lru->used)                      /* make room */
        smb2_closefile(&smb, &lru->id);
    lru->used = 0;
    DBG("srv open %s", P(fh));
    *err = rw ? smb2_openw(&smb, P(fh), SMB2_OPEN, &lru->id, NULL)
              : smb2_open(&smb, P(fh), 0, &lru->id, NULL);
    if (*err)
        return NULL;
    strcpy(lru->path, P(fh));
    lru->rw = rw;
    lru->used = ++stamp;
    return lru;
}

/* close the handles we keep on path and anything inside it: the server
 * refuses deletes and renames of open objects */
static void close_path(const char *path)
{
    u32 n = strlen(path);
    int i;

    for (i = 0; i < NOPEN; i++)
        if (opens[i].used && !strncmp(opens[i].path, path, n) &&
            (!opens[i].path[n] || opens[i].path[n] == '\\')) {
            if (smb.sock >= 0) smb2_closefile(&smb, &opens[i].id);
            opens[i].used = 0;
        }
    for (i = 0; i < NDIR; i++)
        if (dirs[i].open && !strncmp(dirs[i].path, path, n) &&
            (!dirs[i].path[n] || dirs[i].path[n] == '\\')) {
            if (smb.sock >= 0) smb2_closefile(&smb, &dirs[i].id);
            dirs[i].open = 0;
            dirs[i].path[0] = 0;
        }
}

static int join(char *out, const struct Fh *dir, const char *name)
{
    u32 dl = strlen(P(dir));
    if (dl + 1 + strlen(name) + 1 > FS_FHSIZE)
        return 0;
    strcpy(out, P(dir));
    if (dl) strcat(out, "\\");
    strcat(out, name);
    return 1;
}

static long smb_read(const struct Fh *fh, u32 hi, u32 lo, u32 count,
                     const u8 **data, u32 *got, int *eof)
{
    long e;
    struct Open *o;
    int i;

    /* from the start, with no handle open on it: open, read and close in one
     * round trip; a longer file opens a handle for the rest as before */
    if (!hi && !lo) {
        for (i = 0; i < NOPEN && !(opens[i].used && !strcmp(opens[i].path, P(fh))); i++)
            ;
        if (i == NOPEN)
        {
            DBG("srv readsmall %s", P(fh));
            return smb2_readsmall(&smb, P(fh), count, data, got, eof);
        }
    }
    if (!(o = open_for(fh, 0, &e))) { DBG("smb: open failed %ld status %lx", e, (unsigned long)smb.ntstatus); return e; }
    e = smb2_read(&smb, &o->id, hi, lo, count, data, got, eof);
    if (e) DBG("smb: read at %lu failed %ld status %lx", lo, e, (unsigned long)smb.ntstatus);
    return e;
}

static void smb_release(const struct Fh *fh)
{
    int i;
    for (i = 0; i < NOPEN; i++)
        if (opens[i].used && !strcmp(opens[i].path, P(fh))) {
            if (smb.sock >= 0) smb2_closefile(&smb, &opens[i].id);
            opens[i].used = 0;
        }
}

/* cookie[0] = slot + 1, cookie[1] = entries handed out; (0,0) = start */
static long smb_readdir(const struct Fh *dir, u32 cookie[2], u8 verf[8], u32 maxcount,
                        nfs3_entry_fn fn, void *ctx, int *eof)
{
    struct DirSlot *d = NULL;
    static u32 dir_clock;
    long e;
    int i, restart = 0;
    u32 skip = 0;

    (void)verf; (void)maxcount;
    *eof = 0;
    DBG("readdir %s cookie %lu/%lu slots [%ld %s %lu] [%ld %s %lu]", P(dir), (unsigned long)cookie[0],
        (unsigned long)cookie[1], (long)dirs[0].open, dirs[0].path, (unsigned long)dirs[0].seq,
        (long)dirs[1].open, dirs[1].path, (unsigned long)dirs[1].seq);
    if (cookie[0] >= 1 && cookie[0] <= NDIR && dirs[cookie[0] - 1].open &&
        !strcmp(dirs[cookie[0] - 1].path, P(dir)) && dirs[cookie[0] - 1].seq == cookie[1])
        d = &dirs[cookie[0] - 1];
    if (!d) {                           /* a new listing: take a free or the oldest slot */
        restart = 1;
        /* a continuation whose slot went to another directory (a tree walk
         * keeps more than NDIR listings open): reopen and skip the entries
         * already handed out, rather than start again from the first */
        if (cookie[0] || cookie[1]) {
            skip = cookie[1];
            DBG("readdir %s: slot gone, reopening and skipping %lu", P(dir), (unsigned long)skip);
        }
        d = &dirs[0];
        for (i = 0; i < NDIR; i++)
            if (!dirs[i].open) { d = &dirs[i]; break; }
            else if (dirs[i].last < d->last) d = &dirs[i];
        if (d->open)
            smb2_closefile(&smb, &d->id);
        d->open = 0;
        if (!d->buf && !(d->buf = AllocVec(dirbufsize, MEMF_PUBLIC)))
            return NFS3ERR_IO;
        if ((e = smb2_open(&smb, P(dir), 1, &d->id, NULL)))
            return e;
        strcpy(d->path, P(dir));
        d->open = 1;
        d->len = d->pos = d->seq = 0;
        d->eof = 0;
        if (!skip) lc_begin(d->path); else lc_abort(d->path);
    }
    d->last = ++dir_clock;
    for (;;) {
        char name[FS_FHSIZE * 3];
        struct Fattr a;
        u32 at = d->pos;

        if (d->pos >= d->len) {
            const u8 *buf;
            u32 len, bs = M.rsize < 16384 ? M.rsize : 16384;
            int end;
            if (d->eof || (e = smb2_querydir(&smb, &d->id, restart, bs, &buf, &len, &end))) {
                if (!d->eof) return e;
                end = 1;
            }
            restart = 0;
            if (end) {
                lc_done(d->path);
                smb2_closefile(&smb, &d->id);
                d->open = 0;
                d->path[0] = 0;
                *eof = 1;
                return 0;
            }
            CopyMem((APTR)buf, d->buf, len);
            d->len = len;
            d->pos = at = 0;
        }
        if (!smb2_dir_next(d->buf, d->len, &d->pos, name, sizeof(name), &a)) {
            d->pos = d->len;
            continue;
        }
        {
            char full[FS_FHSIZE * 2];
            strcpy(full, d->path);
            if (full[0]) strcat(full, "\\");
            strncat(full, name, FS_FHSIZE - 1);
            a.fileid_lo = smb2_fnv(full);
        }
        if (skip) {                     /* already delivered before the slot was lost */
            skip--;
            d->seq++;
            continue;
        }
        if (fn(ctx, name, strlen(name), &a, NULL, (u32)(d - dirs) + 1, d->seq + 1)) {
            d->pos = at;                /* not taken: hand it out again next time */
            cookie[0] = (u32)(d - dirs) + 1;
            cookie[1] = d->seq;
            return 0;
        }
        lc_add(d->path, name, &a);
        d->seq++;
        cookie[0] = (u32)(d - dirs) + 1;
        cookie[1] = d->seq;
    }
}

static long smb_fsstat(const struct Fh *root, struct Fsstat *s)
{
    (void)root;
    DBG("srv fsstat");
    return smb2_fsstat(&smb, s);
}

/* ---- writing (stage 2) --------------------------------------------------------- */

static long smb_setattr(const struct Fh *fh, const struct Sattr *sa)
{
    lc_drop();
    return smb2_setattr(&smb, P(fh), sa->set_size, sa->size, sa->set_mtime, sa->mtime,
                        sa->set_mode, sa->mode);
}

/* SMB2 writes are not "unstable": the verifier never changes */
static long smb_write(const struct Fh *fh, u32 hi, u32 lo, const u8 *data, u32 count,
                      u32 *written, u8 verf[8])
{
    long e;
    struct Open *o = open_for(fh, 1, &e);

    lc_drop();                          /* the folder changes */
    memset(verf, 0, 8);
    if (!o) return e;
    return smb2_write(&smb, &o->id, hi, lo, data, count, written);
}

static long smb_commit(const struct Fh *fh, u8 verf[8])
{
    int i;

    memset(verf, 0, 8);
    for (i = 0; i < NOPEN; i++)
        if (opens[i].used && opens[i].rw && !strcmp(opens[i].path, P(fh)))
            return smb2_flush(&smb, &opens[i].id);
    return 0;
}

static long smb_create(const struct Fh *dir, const char *name, int guarded,
                       const struct Sattr *sa, struct Fh *out, struct Fattr *a, int *ha)
{
    char path[FS_FHSIZE];
    struct Smb2Id id;
    long e;

    lc_drop();                          /* the folder changes */
    (void)sa;
    if (!join(path, dir, name))
        return NFS3ERR_NAMETOOLONG;
    close_path(path);
    if ((e = smb2_openw(&smb, path, guarded ? SMB2_CREATE : SMB2_OVERWRITE_IF, &id, a)))
        return e;
    smb2_closefile(&smb, &id);          /* the first write opens it again (cached) */
    a->fileid_lo = smb2_fnv(path);
    set_fh(out, path);
    *ha = 1;
    return 0;
}

static long smb_mkdir(const struct Fh *dir, const char *name, const struct Sattr *sa,
                      struct Fh *out, struct Fattr *a, int *ha)
{
    char path[FS_FHSIZE];
    long e;

    lc_drop();                          /* the folder changes */
    (void)sa;
    if (!join(path, dir, name))
        return NFS3ERR_NAMETOOLONG;
    if ((e = smb2_mkdir(&smb, path, a)))
        return e;
    a->fileid_lo = smb2_fnv(path);
    set_fh(out, path);
    *ha = 1;
    return 0;
}

static long del(const struct Fh *dir, const char *name, int isdir)
{
    char path[FS_FHSIZE];

    lc_drop();                          /* the folder changes */
    if (!join(path, dir, name))
        return NFS3ERR_NAMETOOLONG;
    close_path(path);
    return smb2_delete(&smb, path, isdir);
}

static long smb_remove(const struct Fh *dir, const char *name) { return del(dir, name, 0); }
static long smb_rmdir(const struct Fh *dir, const char *name)  { return del(dir, name, 1); }

static long smb_rename(const struct Fh *fdir, const char *fname,
                       const struct Fh *tdir, const char *tname)
{
    char from[FS_FHSIZE], to[FS_FHSIZE];

    lc_drop();                          /* the folder changes */
    if (!join(from, fdir, fname) || !join(to, tdir, tname))
        return NFS3ERR_NAMETOOLONG;
    close_path(from);
    close_path(to);
    return smb2_rename(&smb, from, to);
}

static long no_readlink(const struct Fh *f, char *t, u32 m) { (void)f; (void)t; (void)m; return NFS3ERR_NOTSUPP; }
static long no_symlink(const struct Fh *d, const char *n, const char *t,
                       struct Fh *o, struct Fattr *a, int *h)
{ (void)d; (void)n; (void)t; (void)o; (void)a; (void)h; return NFS3ERR_NOTSUPP; }

const struct Backend Backend_ops = {
    "SMB",
    smb_parse_target, smb_option, smb_init, smb_cleanup,
    smb_connect, smb_drop, smb_connected,
    smb_getattr, smb_lookup, smb_read, smb_readdir, smb_fsstat, smb_setattr,
    smb_write, smb_commit, smb_create, smb_mkdir, smb_remove, smb_rmdir, smb_rename,
    no_readlink, no_symlink,
    NULL, NULL,
    smb_release,
};
