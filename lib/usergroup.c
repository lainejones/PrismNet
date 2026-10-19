/*
 * PrismNet usergroup.library - AmiTCP-compatible user/group API.
 * GPL-3.0-only, see LICENSE.
 *
 * The Amiga is single-user, so by default there is exactly one user: the
 * name comes from ENV:USER (or ENV:LOGNAME, else "amiga"), uid and gid 0,
 * home SYS:. Optional databases override that:
 *
 *   ENV:PrismNet/passwd   name:passwd:uid:gid:gecos:dir:shell
 *   ENV:PrismNet/group    name:passwd:gid:member,member,...
 *
 * (AmiTCP's '|' separator is accepted too.) Every OpenLibrary() gets its own
 * base, so returned structures, errno and ids are per opener.
 * Built -nostartfiles like bsdsocket.library; see lib/support.S.
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define REG(r) __asm(#r)

/* Initialised so they are defined here, not COMMON (else the linker pulls
 * libnix's auto-open stubs). */
struct ExecBase   *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;

#define PN_DOS_MIN 37

#define UG_NGROUPS     32
#define UG_MAXLOGNAME  32
#define UG_MAXMEMBERS  16

/*
 * struct passwd: AmiTCP's 7-field layout, exactly what Roadshow's
 * usergroup.library 4.30 returns, so AmiTCP/Roadshow programs read every
 * field right. Programs built against today's NDK <pwd.h> expect the BSD
 * 10-field layout (pw_change, pw_class inserted before pw_gecos): for them
 * home and shell are repeated where that layout looks (+28, +32) and
 * pw_expire (+36) is 0. They get name, password, uid, gid, home and shell
 * right; Roadshow hands them NULL there. Group structs are the same in both.
 */
struct ug_passwd {
    char *pw_name;              /* +0  */
    char *pw_passwd;            /* +4  */
    LONG  pw_uid;               /* +8  */
    LONG  pw_gid;               /* +12 */
    char *pw_gecos;             /* +16  (BSD: pw_change) */
    char *pw_dir;               /* +20  (BSD: pw_class) */
    char *pw_shell;             /* +24  (BSD: pw_gecos) */
    char *bsd_dir;              /* +28  BSD pw_dir */
    char *bsd_shell;            /* +32  BSD pw_shell */
    LONG  bsd_expire;           /* +36  BSD pw_expire */
};

/* the BSD view of the same entry (see struct ug_passwd) */
static void pw_bsd(struct ug_passwd *pw)
{
    pw->bsd_dir    = pw->pw_dir;
    pw->bsd_shell  = pw->pw_shell;
    pw->bsd_expire = 0;
}

struct ug_group {
    char  *gr_name;
    char  *gr_passwd;
    LONG   gr_gid;
    char **gr_mem;
};

struct ug_credentials {
    LONG  cr_ruid;
    LONG  cr_rgid;
    UWORD cr_umask;             /* UWORD as in <libraries/usergroup.h>: ULONG shifted every later field */
    LONG  cr_euid;
    WORD  cr_ngroups;
    LONG  cr_groups[UG_NGROUPS];
    LONG  cr_session;
    char  cr_login[UG_MAXLOGNAME];
};

struct UgBase {
    struct Library   lib;
    struct UgBase   *master;
    BPTR             seglist;
    APTR             errnoptr;
    LONG             errnosize;
    LONG             err;
    LONG             ruid, euid, rgid, egid;
    LONG             ngroups;
    LONG             groups[UG_NGROUPS];
    ULONG            umask;
    char             login[UG_MAXLOGNAME];
    LONG             pwent, grent;          /* enumeration positions */
    struct ug_passwd pw;
    char             pwbuf[512];
    struct ug_group  gr;
    char             grbuf[512];
    char            *grmem[UG_MAXMEMBERS + 1];
    char             passbuf[128];
    char             cryptbuf[16];
    struct ug_credentials cred;
};

#define UGT_ERRNOBPTR 0x80000001UL
#define UGT_ERRNOWPTR 0x80000002UL
#define UGT_ERRNOLPTR 0x80000004UL
#define UGT_OWNER     0x80000011UL
#define UGT_INTRMASK  0x80000010UL

#define E_PERM   1
#define E_NOENT  2
#define E_INVAL  22

static void set_errno(struct UgBase *b, LONG e)
{
    b->err = e;
    if (b->errnoptr) {
        switch (b->errnosize) {
        case 1: *(UBYTE *)b->errnoptr = (UBYTE)e; break;
        case 2: *(UWORD *)b->errnoptr = (UWORD)e; break;
        default: *(LONG *)b->errnoptr = e; break;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* tiny helpers (no libnix startup)                                         */
/* ---------------------------------------------------------------------- */

static LONG atol_(const char *s)
{
    LONG v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void copyn(char *dst, const char *src, LONG n)
{
    if (n <= 0) return;
    while (--n > 0 && *src) *dst++ = *src++;
    *dst = 0;
}

/* dos.library needs a Process; a Task calling us gets the built-in defaults. */
static BOOL is_process(void)
{
    return FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS;
}

/* Default user name: ENV:USER, ENV:LOGNAME, else "amiga". */
static void default_name(char *buf, LONG size)
{
    if (!is_process()) {
        copyn(buf, "amiga", size);
        return;
    }
    if (GetVar((CONST_STRPTR)"USER", (STRPTR)buf, size, GVF_GLOBAL_ONLY) > 0 && buf[0])
        return;
    if (GetVar((CONST_STRPTR)"LOGNAME", (STRPTR)buf, size, GVF_GLOBAL_ONLY) > 0 && buf[0])
        return;
    copyn(buf, "amiga", size);
}

/* Read a whole (small) file; caller FreeVec()s. NULL if missing. */
static char *load_file(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    char *buf;
    LONG n;

    if (!fh)
        return NULL;
    buf = AllocVec(16384, MEMF_ANY);
    if (buf) {
        n = Read(fh, buf, 16383);
        if (n < 0) n = 0;
        buf[n] = 0;
    }
    Close(fh);
    return buf;
}

/*
 * Walk the lines of a database file. For each non-comment line the fields
 * (separated by ':' or '|') are split into `f`. The callback returns TRUE to
 * stop. Returns TRUE if stopped (a match), FALSE at end of file.
 */
typedef BOOL (*line_fn)(char **f, int nf, void *ctx);

static BOOL walk_db(const char *path, char *scratch, LONG scratchsize, line_fn fn, void *ctx)
{
    char *data = load_file(path), *p, *f[8];
    BOOL hit = FALSE;

    if (!data)
        return FALSE;
    for (p = data; *p && !hit; ) {
        char *eol = p, sep = 0, *q;
        LONG len;
        int nf = 0;

        while (*eol && *eol != '\n') eol++;
        len = eol - p;
        if (len > 0 && p[0] != '#' && len < scratchsize) {
            memcpy(scratch, p, len);
            scratch[len] = 0;
            if (len && scratch[len - 1] == '\r') scratch[len - 1] = 0;
            for (q = scratch; *q; q++)
                if (*q == ':' || *q == '|') { sep = *q; break; }
            if (sep) {
                f[nf++] = scratch;
                for (q = scratch; *q && nf < 8; q++)
                    if (*q == sep) { *q = 0; f[nf++] = q + 1; }
                hit = fn(f, nf, ctx);
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    FreeVec(data);
    return hit;
}

/* ---------------------------------------------------------------------- */
/* passwd                                                                   */
/* ---------------------------------------------------------------------- */

#define PASSWD_FILE "ENV:PrismNet/passwd"
#define GROUP_FILE  "ENV:PrismNet/group"

struct pwq { struct UgBase *b; const char *name; LONG uid; LONG index; LONG n; };

static void fill_pw(struct UgBase *b, char **f, int nf)
{
    b->pw.pw_name   = f[0];
    b->pw.pw_passwd = nf > 1 ? f[1] : (char *)"";
    b->pw.pw_uid    = nf > 2 ? atol_(f[2]) : 0;
    b->pw.pw_gid    = nf > 3 ? atol_(f[3]) : 0;
    b->pw.pw_gecos  = nf > 4 ? f[4] : (char *)"";
    b->pw.pw_dir    = nf > 5 ? f[5] : (char *)"SYS:";
    b->pw.pw_shell  = nf > 6 ? f[6] : (char *)"noshell";
    pw_bsd(&b->pw);
}

static BOOL pw_match(char **f, int nf, void *ctx)
{
    struct pwq *q = (struct pwq *)ctx;
    BOOL hit;

    if (q->name)
        hit = !strcmp(f[0], q->name);
    else if (q->index >= 0)
        hit = q->n++ == q->index;
    else
        hit = nf > 2 && atol_(f[2]) == q->uid;
    if (hit)
        fill_pw(q->b, f, nf);
    return hit;
}

static void default_pw(struct UgBase *b)
{
    char *s = b->pwbuf;
    default_name(s, 64);
    b->pw.pw_name   = s;
    b->pw.pw_passwd = (char *)"";
    b->pw.pw_uid    = 0;
    b->pw.pw_gid    = 0;
    {
        /* real name from PrismNet Prefs (ENV:PrismNet/realname) */
        char *g = s + 64;
        LONG n = is_process() ?
                 GetVar((CONST_STRPTR)"PrismNet/realname", (STRPTR)g, 128, GVF_GLOBAL_ONLY) : -1;
        b->pw.pw_gecos = (n > 0 && g[0]) ? g : (char *)"Amiga user";
        if (n > 0) { char *e = g; while (*e && *e != '\n' && *e != '\r') e++; *e = 0; }
    }
    b->pw.pw_dir    = (char *)"SYS:";
    b->pw.pw_shell  = (char *)"noshell";
    pw_bsd(&b->pw);
}

/* by name (name != NULL), by uid, or the index'th entry (index >= 0) */
static struct ug_passwd *pw_find(struct UgBase *b, const char *name, LONG uid, LONG index)
{
    struct pwq q;
    BPTR lock = is_process() ? Lock((CONST_STRPTR)PASSWD_FILE, ACCESS_READ) : 0;

    if (lock) {
        UnLock(lock);
        q.b = b; q.name = name; q.uid = uid; q.index = index; q.n = 0;
        return walk_db(PASSWD_FILE, b->pwbuf, sizeof(b->pwbuf), pw_match, &q) ? &b->pw : NULL;
    }
    /* No database: the single built-in user. */
    default_pw(b);
    if (index > 0)
        return NULL;
    if (index == 0)
        return &b->pw;
    if (name ? !strcmp(name, b->pw.pw_name) : uid == b->pw.pw_uid)
        return &b->pw;
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* group                                                                    */
/* ---------------------------------------------------------------------- */

struct grq { struct UgBase *b; const char *name; LONG gid; LONG index; LONG n; const char *member; };

static void fill_gr(struct UgBase *b, char **f, int nf)
{
    char *m;
    int n = 0;

    b->gr.gr_name   = f[0];
    b->gr.gr_passwd = nf > 1 ? f[1] : (char *)"";
    b->gr.gr_gid    = nf > 2 ? atol_(f[2]) : 0;
    m = nf > 3 ? f[3] : NULL;
    while (m && *m && n < UG_MAXMEMBERS) {
        b->grmem[n++] = m;
        while (*m && *m != ',') m++;
        if (*m) *m++ = 0;
    }
    b->grmem[n] = NULL;
    b->gr.gr_mem = b->grmem;
}

static BOOL gr_match(char **f, int nf, void *ctx)
{
    struct grq *q = (struct grq *)ctx;
    BOOL hit;

    if (q->name)
        hit = !strcmp(f[0], q->name);
    else if (q->index >= 0)
        hit = q->n++ == q->index;
    else
        hit = nf > 2 && atol_(f[2]) == q->gid;
    if (hit)
        fill_gr(q->b, f, nf);
    return hit;
}

static void default_gr(struct UgBase *b)
{
    char *user = b->grbuf + 64;
    copyn(b->grbuf, "wheel", 64);
    default_name(user, 64);
    b->gr.gr_name   = b->grbuf;
    b->gr.gr_passwd = (char *)"";
    b->gr.gr_gid    = 0;
    b->grmem[0]     = user;
    b->grmem[1]     = NULL;
    b->gr.gr_mem    = b->grmem;
}

static struct ug_group *gr_find(struct UgBase *b, const char *name, LONG gid, LONG index)
{
    struct grq q;
    BPTR lock = is_process() ? Lock((CONST_STRPTR)GROUP_FILE, ACCESS_READ) : 0;

    if (lock) {
        UnLock(lock);
        q.b = b; q.name = name; q.gid = gid; q.index = index; q.n = 0; q.member = NULL;
        return walk_db(GROUP_FILE, b->grbuf, sizeof(b->grbuf), gr_match, &q) ? &b->gr : NULL;
    }
    default_gr(b);
    if (index > 0)
        return NULL;
    if (index == 0)
        return &b->gr;
    if (name ? !strcmp(name, b->gr.gr_name) : gid == b->gr.gr_gid)
        return &b->gr;
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* library open / close / expunge                                           */
/* ---------------------------------------------------------------------- */

static void free_base(struct UgBase *b)
{
    FreeMem((UBYTE *)b - b->lib.lib_NegSize, b->lib.lib_NegSize + b->lib.lib_PosSize);
}

static BPTR ug_expunge(struct UgBase *lib REG(a6))
{
    BPTR seg;

    if (lib->master)
        return 0;
    if (lib->lib.lib_OpenCnt) {
        lib->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seg = lib->seglist;
    Remove(&lib->lib.lib_Node);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    free_base(lib);
    return seg;
}

static struct UgBase *ug_open(struct UgBase *lib REG(a6))
{
    struct UgBase *master = lib->master ? lib->master : lib, *nb;
    ULONG neg = master->lib.lib_NegSize, size = neg + master->lib.lib_PosSize;
    struct ug_passwd *pw;
    UBYTE *mem;

    master->lib.lib_OpenCnt++;          /* held across the file reads below */
    master->lib.lib_Flags &= ~LIBF_DELEXP;
    mem = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!mem) {
        master->lib.lib_OpenCnt--;
        return NULL;
    }
    CopyMem((UBYTE *)master - neg, mem, neg + sizeof(struct Library));
    CacheClearU();
    nb = (struct UgBase *)(mem + neg);
    nb->lib.lib_OpenCnt = 1;
    nb->master = master;
    nb->umask  = 022;

    /* Our identity: the default user's entry. */
    {
        char name[64];
        default_name(name, sizeof(name));
        pw = pw_find(nb, name, 0, -1);
        if (!pw)
            pw = pw_find(nb, NULL, 0, 0);       /* first entry of the database */
    }
    if (pw) {
        nb->ruid = nb->euid = pw->pw_uid;
        nb->rgid = nb->egid = pw->pw_gid;
        copyn(nb->login, pw->pw_name, sizeof(nb->login));
    } else {
        default_name(nb->login, sizeof(nb->login));
    }
    nb->ngroups   = 1;
    nb->groups[0] = nb->rgid;
    return nb;
}

static BPTR ug_close(struct UgBase *b REG(a6))
{
    struct UgBase *master = b->master;

    if (!master)
        return 0;
    free_base(b);
    if (--master->lib.lib_OpenCnt == 0 && (master->lib.lib_Flags & LIBF_DELEXP))
        return ug_expunge(master);
    return 0;
}

static ULONG ug_null(void) { return 0; }

/* ---------------------------------------------------------------------- */
/* API                                                                      */
/* ---------------------------------------------------------------------- */

static LONG ug_setupcontexttaglist(STRPTR name REG(a0), struct TagItem *tags REG(a1),
                                   struct UgBase *b REG(a6))
{
    (void)name;
    while (tags) {
        switch (tags->ti_Tag) {
        case TAG_DONE:   return 0;
        case TAG_IGNORE: break;
        case TAG_MORE:   tags = (struct TagItem *)tags->ti_Data; continue;
        case TAG_SKIP:   tags += tags->ti_Data; break;
        case UGT_ERRNOBPTR: b->errnoptr = (APTR)tags->ti_Data; b->errnosize = 1; break;
        case UGT_ERRNOWPTR: b->errnoptr = (APTR)tags->ti_Data; b->errnosize = 2; break;
        case UGT_ERRNOLPTR: b->errnoptr = (APTR)tags->ti_Data; b->errnosize = 4; break;
        case UGT_OWNER:
        case UGT_INTRMASK:  break;      /* accepted */
        default:            break;
        }
        tags++;
    }
    return 0;
}

static LONG ug_geterr(struct UgBase *b REG(a6)) { return b->err; }

static const char *const ug_errs[] = {
    "No error", "Operation not permitted", "No such user or group", "No such process",
    "Interrupted system call", "Input/output error",
};

static STRPTR ug_strerror(LONG code REG(d1), struct UgBase *b REG(a6))
{
    (void)b;
    if (code == E_INVAL) return (STRPTR)"Invalid argument";
    if (code >= 0 && code < (LONG)(sizeof(ug_errs) / sizeof(ug_errs[0])))
        return (STRPTR)ug_errs[code];
    return (STRPTR)"Unknown error";
}

static LONG ug_getuid(struct UgBase *b REG(a6))  { return b->ruid; }
static LONG ug_geteuid(struct UgBase *b REG(a6)) { return b->euid; }
static LONG ug_getgid(struct UgBase *b REG(a6))  { return b->rgid; }
static LONG ug_getegid(struct UgBase *b REG(a6)) { return b->egid; }

/* Single-user system: any id change is allowed. */
static LONG ug_setreuid(LONG r REG(d0), LONG e REG(d1), struct UgBase *b REG(a6))
{
    if (r != -1) b->ruid = r;
    if (e != -1) b->euid = e;
    return 0;
}

static LONG ug_setuid(LONG u REG(d0), struct UgBase *b REG(a6))
{
    b->ruid = b->euid = u;
    return 0;
}

static LONG ug_setregid(LONG r REG(d0), LONG e REG(d1), struct UgBase *b REG(a6))
{
    if (r != -1) b->rgid = r;
    if (e != -1) b->egid = e;
    return 0;
}

static LONG ug_setgid(LONG g REG(d0), struct UgBase *b REG(a6))
{
    b->rgid = b->egid = g;
    return 0;
}

static LONG ug_getgroups(LONG size REG(d0), LONG *set REG(a1), struct UgBase *b REG(a6))
{
    LONG i;
    if (size == 0)
        return b->ngroups;
    if (size < b->ngroups || !set) {
        set_errno(b, E_INVAL);
        return -1;
    }
    for (i = 0; i < b->ngroups; i++)
        set[i] = b->groups[i];
    return b->ngroups;
}

static LONG ug_setgroups(LONG n REG(d0), const LONG *set REG(a1), struct UgBase *b REG(a6))
{
    LONG i;
    if (n < 0 || n > UG_NGROUPS || (n && !set)) {
        set_errno(b, E_INVAL);
        return -1;
    }
    for (i = 0; i < n; i++)
        b->groups[i] = set[i];
    b->ngroups = n;
    return 0;
}

static LONG ug_initgroups(STRPTR name REG(a1), LONG basegid REG(d0), struct UgBase *b REG(a6))
{
    LONG i, n = 0;
    struct ug_group *g;

    b->groups[n++] = basegid;
    for (i = 0; n < UG_NGROUPS && (g = gr_find(b, NULL, 0, i)) != NULL; i++) {
        char **m;
        for (m = g->gr_mem; *m; m++)
            if (name && !strcmp(*m, (char *)name) && g->gr_gid != basegid) {
                b->groups[n++] = g->gr_gid;
                break;
            }
    }
    b->ngroups = n;
    return 0;
}

static APTR ug_getpwnam(STRPTR name REG(a1), struct UgBase *b REG(a6))
{
    struct ug_passwd *pw = name ? pw_find(b, (const char *)name, 0, -1) : NULL;
    if (!pw) set_errno(b, E_NOENT);
    return pw;
}

static APTR ug_getpwuid(LONG uid REG(d0), struct UgBase *b REG(a6))
{
    struct ug_passwd *pw = pw_find(b, NULL, uid, -1);
    if (!pw) set_errno(b, E_NOENT);
    return pw;
}

static void ug_setpwent(struct UgBase *b REG(a6)) { b->pwent = 0; }
static void ug_endpwent(struct UgBase *b REG(a6)) { b->pwent = 0; }

static APTR ug_getpwent(struct UgBase *b REG(a6))
{
    struct ug_passwd *pw = pw_find(b, NULL, 0, b->pwent);
    if (pw) b->pwent++;
    return pw;
}

static APTR ug_getgrnam(STRPTR name REG(a1), struct UgBase *b REG(a6))
{
    struct ug_group *g = name ? gr_find(b, (const char *)name, 0, -1) : NULL;
    if (!g) set_errno(b, E_NOENT);
    return g;
}

static APTR ug_getgrgid(LONG gid REG(d0), struct UgBase *b REG(a6))
{
    struct ug_group *g = gr_find(b, NULL, gid, -1);
    if (!g) set_errno(b, E_NOENT);
    return g;
}

static void ug_setgrent(struct UgBase *b REG(a6)) { b->grent = 0; }
static void ug_endgrent(struct UgBase *b REG(a6)) { b->grent = 0; }

static APTR ug_getgrent(struct UgBase *b REG(a6))
{
    struct ug_group *g = gr_find(b, NULL, 0, b->grent);
    if (g) b->grent++;
    return g;
}

/*
 * crypt(): PrismNet's own scheme, NOT Unix DES crypt. Output looks like a
 * classic 13-character hash (2 salt characters + 11 from a 64-bit FNV-1a
 * of salt+key), so passwd files written with PrismNet tools verify here.
 */
static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static STRPTR ug_crypt(STRPTR key REG(a0), STRPTR salt REG(a1), struct UgBase *b REG(a6))
{
    unsigned long long h = 0xcbf29ce484222325ULL;      /* FNV-1a 64 */
    const UBYTE *p;
    char s0 = (salt && salt[0]) ? salt[0] : '.', s1 = (salt && salt[0] && salt[1]) ? salt[1] : '.';
    int i, round;

    for (round = 0; round < 64; round++) {             /* a little stretching */
        h ^= (UBYTE)s0; h *= 0x100000001b3ULL;
        h ^= (UBYTE)s1; h *= 0x100000001b3ULL;
        for (p = key ? (const UBYTE *)key : (const UBYTE *)""; *p; p++) {
            h ^= *p;
            h *= 0x100000001b3ULL;
        }
    }
    b->cryptbuf[0] = s0;
    b->cryptbuf[1] = s1;
    for (i = 0; i < 11; i++) {
        b->cryptbuf[2 + i] = b64[(ULONG)h & 63];
        h >>= 6;
    }
    b->cryptbuf[13] = 0;
    return (STRPTR)b->cryptbuf;
}
static STRPTR ug_getsalt(APTR pw REG(a0), STRPTR buf REG(a1), LONG size REG(d0), struct UgBase *b REG(a6))
{
    struct ug_passwd *p = (struct ug_passwd *)pw;
    (void)b;
    if (!buf || size < 3)
        return NULL;
    if (p && p->pw_passwd && p->pw_passwd[0] && p->pw_passwd[1]) {
        buf[0] = p->pw_passwd[0];
        buf[1] = p->pw_passwd[1];
    } else {
        struct DateStamp ds;
        DateStamp(&ds);
        buf[0] = b64[ds.ds_Tick & 63];
        buf[1] = b64[(ds.ds_Minute ^ (ds.ds_Tick >> 6)) & 63];
    }
    buf[2] = 0;
    return buf;
}

static STRPTR ug_getpass(STRPTR prompt REG(a1), struct UgBase *b REG(a6))
{
    BPTR in = Input(), out = Output();
    LONG n = 0;
    char c;

    if (out && prompt)
        Write(out, prompt, strlen((char *)prompt));
    if (!in)
        return NULL;
    SetMode(in, 1);                     /* raw: no echo */
    while (Read(in, &c, 1) == 1 && c != '\r' && c != '\n' && c != 3) {
        if ((c == 8 || c == 127) && n > 0)
            n--;
        else if (c >= 32 && n < (LONG)sizeof(b->passbuf) - 1)
            b->passbuf[n++] = c;
    }
    SetMode(in, 0);
    if (out)
        Write(out, "\n", 1);
    b->passbuf[n] = 0;
    return (STRPTR)b->passbuf;
}

static ULONG ug_umask(ULONG mask REG(d0), struct UgBase *b REG(a6))
{
    ULONG old = b->umask;
    b->umask = mask & 0777;
    return old;
}

static ULONG ug_getumask(struct UgBase *b REG(a6)) { return b->umask; }

static LONG ug_setsid(struct UgBase *b REG(a6))  { (void)b; return (LONG)FindTask(NULL); }
static LONG ug_getpgrp(struct UgBase *b REG(a6)) { (void)b; return (LONG)FindTask(NULL); }

static STRPTR ug_getlogin(struct UgBase *b REG(a6)) { return (STRPTR)b->login; }

static LONG ug_setlogin(STRPTR name REG(a1), struct UgBase *b REG(a6))
{
    if (!name) { set_errno(b, E_INVAL); return -1; }
    copyn(b->login, (const char *)name, sizeof(b->login));
    return 0;
}

static void ug_setutent(struct UgBase *b REG(a6)) { (void)b; }
static void ug_endutent(struct UgBase *b REG(a6)) { (void)b; }
static APTR ug_getutent(struct UgBase *b REG(a6)) { (void)b; return NULL; }
static APTR ug_getlastlog(LONG uid REG(d0), struct UgBase *b REG(a6)) { (void)uid; (void)b; return NULL; }

static LONG ug_setlastlog(LONG uid REG(d0), STRPTR name REG(a0), STRPTR host REG(a1), struct UgBase *b REG(a6))
{
    (void)uid; (void)name; (void)host; (void)b;
    return 0;                           /* no lastlog database; accepted */
}

static APTR ug_getcredentials(APTR task REG(a0), struct UgBase *b REG(a6))
{
    LONG i;
    (void)task;
    b->cred.cr_ruid    = b->ruid;
    b->cred.cr_rgid    = b->rgid;
    b->cred.cr_umask   = (UWORD)b->umask;
    b->cred.cr_euid    = b->euid;
    b->cred.cr_ngroups = (WORD)b->ngroups;
    for (i = 0; i < b->ngroups; i++)
        b->cred.cr_groups[i] = b->groups[i];
    b->cred.cr_session = (LONG)FindTask(NULL);
    copyn(b->cred.cr_login, b->login, sizeof(b->cred.cr_login));
    return &b->cred;
}

/* ---------------------------------------------------------------------- */

#define F(x) ((APTR)(x))
static const APTR functable[] = {
    F(ug_open), F(ug_close), F(ug_expunge), F(ug_null),
    F(ug_setupcontexttaglist), F(ug_geterr), F(ug_strerror),                    /* -30..-42 */
    F(ug_getuid), F(ug_geteuid), F(ug_setreuid), F(ug_setuid),                  /* -48..-66 */
    F(ug_getgid), F(ug_getegid), F(ug_setregid), F(ug_setgid),                  /* -72..-90 */
    F(ug_getgroups), F(ug_setgroups), F(ug_initgroups),                         /* -96..-108 */
    F(ug_getpwnam), F(ug_getpwuid), F(ug_setpwent), F(ug_getpwent), F(ug_endpwent), /* -114..-138 */
    F(ug_getgrnam), F(ug_getgrgid), F(ug_setgrent), F(ug_getgrent), F(ug_endgrent), /* -144..-168 */
    F(ug_crypt), F(ug_getsalt), F(ug_getpass), F(ug_umask), F(ug_getumask),     /* -174..-198 */
    F(ug_setsid), F(ug_getpgrp), F(ug_getlogin), F(ug_setlogin),                /* -204..-222 */
    F(ug_setutent), F(ug_getutent), F(ug_endutent),                             /* -228..-240 */
    F(ug_getlastlog), F(ug_setlastlog), F(ug_getcredentials),                   /* -246..-258 */
    (APTR)-1
};

extern const char ug_libname[], ug_idstring[];

struct Library *ug_lib_init(BPTR seglist REG(d0), struct ExecBase *sysbase REG(a6))
{
    struct UgBase *lib;

    SysBase = sysbase;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", PN_DOS_MIN);
    if (!DOSBase)
        return NULL;
    lib = (struct UgBase *)MakeLibrary((APTR)functable, NULL, NULL, sizeof(struct UgBase), seglist);
    if (!lib) {
        CloseLibrary((struct Library *)DOSBase);
        return NULL;
    }
    lib->lib.lib_Node.ln_Type = NT_LIBRARY;
    lib->lib.lib_Node.ln_Name = (char *)ug_libname;
    lib->lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    lib->lib.lib_Version      = 4;
    lib->lib.lib_Revision     = 3;
    lib->lib.lib_IdString     = (APTR)ug_idstring;
    lib->seglist              = seglist;
    AddLibrary(&lib->lib);
    return &lib->lib;
}
