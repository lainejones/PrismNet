/*
 * PrismNet Prefs - edits ENV:PrismNet / ENVARC:PrismNet (GadTools, OS 3.1+).
 * GPL-3.0-only, see LICENSE.
 *
 *  +- PrismNet Prefs ------------------------------------------------+
 *  | Interfaces          Name    [eth0      ]   Mode    <DHCP  >       |
 *  | | eth0          |   Device  [cnet.device       ] [...]  Unit [0]  |
 *  | |               |   Address [          ]   Netmask [          ]   |
 *  | [Add]  [Remove]     Gateway [          ]   MTU     [    ]         |
 *  |-------------------------------------------------------------------|
 *  | Host name [amiga      ]  Domain [          ]  Gateway [          ]|
 *  | DNS servers [          ] [          ] [          ]   [Routes...]  |
 *  | User name [      ]  Real name [            ]        [Accounts...] |
 *  | [x] Start automatically when needed  [ ] Start at boot  [Hosts...]|
 *  |-------------------------------------------------------------------|
 *  | [ Save ]               [ Use ]                       [ Cancel ]   |
 *  +-------------------------------------------------------------------+
 *
 * Routes... edits the ROUTE lines in routes; Accounts... edits passwd and
 * group (usergroup.library's databases) in small list windows.
 *
 * Use writes ENV:PrismNet, Save writes ENV: and ENVARC:. If the stack is
 * running it offers to restart it. Project/Import reads a Roadshow setup
 * (DEVS:NetInterfaces, DEVS:Internet/name_resolution, routes, hosts).
 *
 * Without a window (for scripts and tests):
 *   PrismNetPrefs [FROM=dir] [IMPORT=devsdir] TO=dir
 * loads FROM (default ENV:PrismNet, else ENVARC:PrismNet), optionally merges a
 * Roadshow setup found under IMPORT (e.g. DEVS:), and writes the result to TO.
 *   PrismNetPrefs BOOT=YES|NO
 * adds/removes the PrismNet block in S:User-Startup (used by the uninstaller).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <intuition/sghooks.h>
#include <utility/hooks.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/text.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/layers.h>
#include <graphics/regions.h>

#include "prismnet/pnmsg.h"
#include "tabs.h"

static const char verstag[] __attribute__((used)) = "$VER: PrismNetPrefs 0.6 (18.10.2026) PrismNet";

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;
struct Library       *AslBase;
struct Library       *LayersBase;

#define MAXIF       4
#define ENV_DIR     "ENV:PrismNet"
#define ARC_DIR     "ENVARC:PrismNet"
#define USTARTUP    "S:User-Startup"
#define BOOT_BEGIN  ";BEGIN PrismNet"
#define BOOT_END    ";END PrismNet"

/* ---------------------------------------------------------------------- */
/* the settings model (all strings, validated on save)                      */
/* ---------------------------------------------------------------------- */

struct PrefIf {
    char name[16], device[80], unit[8], addr[16], mask[16], gw[16], mtu[8];
    char baud[8];                       /* serial SLIP speed ("" = default) */
    BOOL dhcp, rtscts;
};

#define MAXROUTES 16
#define MAXUSERS  32
#define MAXGROUPS 32

struct PrefRoute { char dest[20], gw[16]; };            /* dest: a.b.c.d[/bits] */
struct PrefUser  { char name[32], passwd[48], uid[8], gid[8], gecos[64], dir[64], shell[32]; };
struct PrefGroup { char name[32], passwd[48], gid[8], members[128]; };

/* a network share: one DOSDriver file (tools/prefsshares_io.c) */
#define MAXSHARES 8
struct PrefShare {
    char name[16];                      /* device name = file name, e.g. NAS */
    int  smb;                           /* 0 NFS, 1 SMB */
    int  handler;                       /* index into sh_handlers[] */
    char host[64], share[64], volume[32], user[32], uid[8], gid[8];
    char extra[128];                    /* other Startup options, kept as they were */
    char stack[8], pri[4];
    BOOL atboot;                        /* DEVS:DOSDrivers, else SYS:Storage/DOSDrivers */
    BOOL readonly, indevs;
};

struct Prefs {
    struct PrefIf ifs[MAXIF];
    int    nifs;
    struct PrefRoute routes[MAXROUTES];
    int    nroutes;
    struct PrefUser  users[MAXUSERS];                   /* ENV:PrismNet/passwd */
    int    nusers;
    struct PrefGroup groups[MAXGROUPS];                 /* ENV:PrismNet/group */
    int    ngroups;
    char   hostname[64], domain[64], defgw[16], dns[3][16], command[80];
    char   user[32], realname[64], workgroup[16];   /* user / workgroup area */
    BOOL   autostart, atboot;
    char   memory[8];                   /* startup MEMORY=<KB> ("" = default) */
    char   import_hosts[120];       /* hosts file to copy in on save ("" = none) */
    struct PrefShare shares[MAXSHARES];
    int    nshares;
    char   sh_loaded[MAXSHARES][16];    /* files found at load: gone ones are deleted on save */
    BOOL   sh_loaded_devs[MAXSHARES];
    int    nloaded;
    char   inetd[96];                   /* startup INETD= ("" = the default, C:inetd) */
    /* the Services tab: the ftp line of ENV:PrismNet/services */
    BOOL   have_services, ftp_on, ftp_anon, ftp_ro;
    char   ftp_port[8], ftp_prog[96], ftp_anondir[128], ftp_pasv[16], ftp_extra[96];
    BOOL   tel_on;                      /* the telnet line */
    char   tel_port[8], tel_prog[96], tel_extra[96];
    char   svc_other[1536];             /* every other line of the file, kept as it was */
};

static struct Prefs P;

/* ---------------------------------------------------------------------- */
/* small text helpers                                                       */
/* ---------------------------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* a file a settings folder may hold that is not a setting: an icon, or a
 * backup someone kept next to the real file (scsidayna.orig, eth0.bak, ...) */
static int skip_name(const char *fn)
{
    static const char *const ext[] = { ".info", ".bak", ".new", ".old", ".orig", ".prev", ".backup", NULL };
    size_t l = strlen(fn), k;
    int i;
    if (fn[0] == '.' || l == 0 || fn[l - 1] == '~')
        return 1;
    for (i = 0; ext[i]; i++) {
        k = strlen(ext[i]);
        if (l > k && ieq(fn + l - k, ext[i]))
            return 1;
    }
    return 0;
}

static void copy(char *dst, const char *src, int size)
{
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = 0;
}

/* The whole file (S:User-Startup can be far bigger than 16 KB, and it is
 * written back: truncating it here would lose the rest). */
static char *load_file(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    struct FileInfoBlock *fib;
    LONG size = -1, n;
    char *buf;

    if (!fh) return NULL;
    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (ExamineFH(fh, fib)) size = fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    if (size < 0 || !(buf = AllocVec(size + 1, MEMF_ANY))) { Close(fh); return NULL; }
    n = Read(fh, buf, size);
    Close(fh);
    if (n != size) { FreeVec(buf); return NULL; }   /* short read: never write it back */
    buf[n] = 0;
    return buf;
}

/* Write path.new, then replace path with it: a full disk leaves the old
 * file intact instead of an empty one the core would refuse at boot. */
static char save_err[120];          /* what the last failed save_file() was doing */

static BOOL save_file(const char *path, const char *text)
{
    char tmp[200];
    BPTR fh;
    LONG len = strlen(text);
    BOOL ok;

    if (strlen(path) + 5 > sizeof(tmp)) { sprintf(save_err, "path too long"); return FALSE; }
    strcpy(tmp, path);
    strcat(tmp, ".new");
    if (!(fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE))) {
        sprintf(save_err, "cannot create %.80s\n(error %ld)", tmp, (long)IoErr());
        return FALSE;
    }
    ok = Write(fh, (APTR)text, len) == len;
    if (!Close(fh)) ok = FALSE;
    if (!ok) { sprintf(save_err, "cannot write %.80s\n(error %ld)", tmp, (long)IoErr()); DeleteFile((CONST_STRPTR)tmp); return FALSE; }
    DeleteFile((CONST_STRPTR)path);
    if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
        /* OS 3.2's ENV: is a lazy link to ENVARC:: a deleted name comes
         * straight back, so the rename finds it in the way (203). The
         * .new copy is complete by now, so write the file in place. */
        LONG err = IoErr();
        if (err == ERROR_OBJECT_EXISTS && (fh = Open((CONST_STRPTR)path, MODE_NEWFILE)) != 0) {
            ok = Write(fh, (APTR)text, len) == len;
            if (!Close(fh)) ok = FALSE;
            DeleteFile((CONST_STRPTR)tmp);
            if (ok) return TRUE;
            err = IoErr();
        }
        sprintf(save_err, "cannot rename %.80s\n(error %ld)", tmp, (long)err);
        DeleteFile((CONST_STRPTR)tmp);
        return FALSE;
    }
    return TRUE;
}

/* Remove a settings file. In ENV: an empty file is written instead: on
 * OS 3.2 ENV: is a lazy link to ENVARC:, and a deleted name comes straight
 * back from there on the next access. The core treats empty as "none". */
static void drop_file(const char *dir, const char *path)
{
    if (!strcmp(dir, ENV_DIR)) save_file(path, "");
    else DeleteFile((CONST_STRPTR)path);
}

static BOOL copy_file(const char *from, const char *to)
{
    char *t = load_file(from);
    BOOL ok;
    if (!t) return FALSE;
    ok = save_file(to, t);
    FreeVec(t);
    return ok;
}

static BOOL exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

static void make_dir(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!l) l = CreateDir((CONST_STRPTR)path);
    if (l) UnLock(l);
}

/* First line of a small text file (without the newline); "" if none. */
static void load_line(const char *path, char *out, int size)
{
    char *t = load_file(path), *e;
    out[0] = 0;
    if (!t) return;
    for (e = t; *e && *e != '\n' && *e != '\r'; e++) ;
    *e = 0;
    copy(out, t, size);
    FreeVec(t);
}

/* next token: whitespace separated, '#'/';' comments, "quotes" */
static BOOL token(char **pp, char *out, int size)
{
    char *p = *pp;
    int n = 0;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    if (!*p) { *pp = p; return FALSE; }
    while (*p && !isspace((unsigned char)*p)) {
        if (*p == '"') { p++; while (*p && *p != '"') { if (n < size - 1) out[n++] = *p; p++; } if (*p) p++; }
        else { if (n < size - 1) out[n++] = *p; p++; }
    }
    out[n] = 0;
    *pp = p;
    return TRUE;
}

static BOOL valid_ip(const char *s)
{
    int parts = 0;
    while (parts < 4) {
        int v = 0, d = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++d > 3) return FALSE; }
        if (!d || v > 255) return FALSE;
        parts++;
        if (parts < 4) { if (*s++ != '.') return FALSE; }
    }
    return *s == 0;
}

/* ---------------------------------------------------------------------- */
/* load / save ENV:PrismNet style directories                               */
/* ---------------------------------------------------------------------- */

static void defaults(struct Prefs *p)
{
    memset(p, 0, sizeof(*p));
    strcpy(p->hostname, "amiga");
    p->autostart = TRUE;
}

static void parse_iface(struct PrefIf *pi, const char *name, char *text)
{
    char tok[128], *p = text;
    memset(pi, 0, sizeof(*pi));
    copy(pi->name, name, sizeof(pi->name));
    strcpy(pi->unit, "0");
    pi->dhcp = TRUE;
    while (token(&p, tok, sizeof(tok))) {
        char *v = strchr(tok, '=');
        if (!v) continue;
        *v++ = 0;
        if (ieq(tok, "DEVICE"))       copy(pi->device, v, sizeof(pi->device));
        else if (ieq(tok, "UNIT"))    copy(pi->unit, v, sizeof(pi->unit));
        else if (ieq(tok, "MTU"))     copy(pi->mtu, v, sizeof(pi->mtu));
        else if (ieq(tok, "CONFIG"))  pi->dhcp = !ieq(v, "STATIC");
        else if (ieq(tok, "ADDRESS")) { if (ieq(v, "DHCP")) pi->dhcp = TRUE; else { copy(pi->addr, v, sizeof(pi->addr)); pi->dhcp = FALSE; } }
        else if (ieq(tok, "NETMASK")) copy(pi->mask, v, sizeof(pi->mask));
        else if (ieq(tok, "GATEWAY")) copy(pi->gw, v, sizeof(pi->gw));
        else if (ieq(tok, "BAUD"))    copy(pi->baud, v, sizeof(pi->baud));
        else if (ieq(tok, "RTSCTS"))  pi->rtscts = ieq(v, "YES") || ieq(v, "ON") || ieq(v, "1");
    }
}

/* Add the interface file names in dir/interfaces to names[] (no duplicates). */
static int scan_names(const char *dir, char names[][16], int n, int max)
{
    char path[160];
    BPTR lock;
    struct FileInfoBlock *fib;
    int i;

    sprintf(path, "%s/interfaces", dir);
    if (!(lock = Lock((CONST_STRPTR)path, ACCESS_READ)))
        return n;
    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (Examine(lock, fib))
            while (ExNext(lock, fib) && n < max) {
                const char *fn = fib->fib_FileName;
                size_t l = strlen(fn);
                if (fib->fib_DirEntryType > 0 || l >= 16 || skip_name(fn))
                    continue;
                for (i = 0; i < n && !ieq(names[i], fn); i++) ;
                if (i == n)
                    strcpy(names[n++], fn);
            }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return n;
}

/*
 * OS 3.2 links RAM:ENV to ENVARC: and copies a file in only when it is opened
 * by name: right after a boot, listing ENV:PrismNet/interfaces shows nothing.
 * So for ENV: the names also come from ENVARC:, and each file is opened via
 * ENV: (which pulls it in), falling back to ENVARC:.
 */
static void load_ifaces(struct Prefs *p, const char *dir)
{
    char path[160], names[MAXIF * 2][16];
    int n = 0, i, j;
    BOOL env = ieq(dir, ENV_DIR);

    n = scan_names(dir, names, n, MAXIF * 2);
    if (env)
        n = scan_names(ARC_DIR, names, n, MAXIF * 2);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) < 0) { char t[16]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t); }
    for (i = 0; i < n && p->nifs < MAXIF; i++) {
        char *t;
        sprintf(path, "%s/interfaces/%s", dir, names[i]);
        if (!(t = load_file(path)) && env) {
            sprintf(path, ARC_DIR "/interfaces/%s", names[i]);
            t = load_file(path);
        }
        if (t) {
            char *c = t;
            while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
            if (*c)                     /* empty = removed (see drop_file) */
                parse_iface(&p->ifs[p->nifs++], names[i], t);
            FreeVec(t);
        }
    }
}

/* passwd / group lines: fields split at ':' (or AmiTCP's '|', whichever
 * comes first, like usergroup.library). Returns the field count. */
static int split_db(char *line, char **f, int maxf)
{
    char sep = 0, *q;
    int n = 1;

    for (q = line; *q; q++)
        if (*q == ':' || *q == '|') { sep = *q; break; }
    f[0] = line;
    if (sep)
        for (q = line; *q && n < maxf; q++)
            if (*q == sep) { *q = 0; f[n++] = q + 1; }
    return n;
}

/* Calls fn for each non-comment line of dir/name, split into fields. */
static void walk_db(const char *dir, const char *name, struct Prefs *p,
                    void (*fn)(struct Prefs *, char **, int))
{
    char path[160], *t, *line, *e, *f[8];

    sprintf(path, "%s/%s", dir, name);
    if (!(t = load_file(path))) return;
    for (line = t; *line; ) {
        char *next;
        for (e = line; *e && *e != '\n'; e++) ;
        next = *e ? e + 1 : e;          /* before the line end is cut off */
        *e = 0;
        if (e > line && e[-1] == '\r') e[-1] = 0;
        if (*line && *line != '#')
            fn(p, f, split_db(line, f, 8));
        line = next;
    }
    FreeVec(t);
}

static void add_user(struct Prefs *p, char **f, int n)
{
    struct PrefUser *u;
    if (p->nusers >= MAXUSERS) return;
    u = &p->users[p->nusers++];
    memset(u, 0, sizeof(*u));
    copy(u->name, f[0], sizeof(u->name));
    if (n > 1) copy(u->passwd, f[1], sizeof(u->passwd));
    copy(u->uid, n > 2 ? f[2] : "0", sizeof(u->uid));
    copy(u->gid, n > 3 ? f[3] : "0", sizeof(u->gid));
    if (n > 4) copy(u->gecos, f[4], sizeof(u->gecos));
    copy(u->dir, n > 5 ? f[5] : "SYS:", sizeof(u->dir));
    copy(u->shell, n > 6 ? f[6] : "noshell", sizeof(u->shell));
}

static void add_group(struct Prefs *p, char **f, int n)
{
    struct PrefGroup *g;
    if (p->ngroups >= MAXGROUPS) return;
    g = &p->groups[p->ngroups++];
    memset(g, 0, sizeof(*g));
    copy(g->name, f[0], sizeof(g->name));
    if (n > 1) copy(g->passwd, f[1], sizeof(g->passwd));
    copy(g->gid, n > 2 ? f[2] : "0", sizeof(g->gid));
    if (n > 3) copy(g->members, f[3], sizeof(g->members));
}

static void load_users(struct Prefs *p, const char *dir)  { walk_db(dir, "passwd", p, add_user); }
static void load_groups(struct Prefs *p, const char *dir) { walk_db(dir, "group", p, add_group); }

static BOOL boot_entry_present(void)
{
    char *t = load_file(USTARTUP);
    BOOL found = t && strstr(t, BOOT_BEGIN);
    if (t) FreeVec(t);
    return found;
}

static BOOL all_digits(const char *s);
#include "prefsshares_io.c"
#include "prefsservices_io.c"

static void load_prefs(struct Prefs *p, const char *dir)
{
    char path[160], tok[128], val[128], *t, *q;
    int nns = 0;

    defaults(p);
    load_ifaces(p, dir);
    load_shares(p);
    load_services(p, dir);
    sprintf(path, "%s/routes", dir);
    if ((t = load_file(path)) != NULL) {
        for (q = t; token(&q, tok, sizeof(tok)); ) {
            if (ieq(tok, "DEFAULT") && token(&q, val, sizeof(val))) {
                char *v = strchr(val, '=');
                copy(p->defgw, v ? v + 1 : val, sizeof(p->defgw));
            } else if (ieq(tok, "ROUTE") && token(&q, val, sizeof(val)) && p->nroutes < MAXROUTES) {
                char gw[64], *v;
                if (!token(&q, gw, sizeof(gw))) break;
                v = strchr(gw, '=');
                copy(p->routes[p->nroutes].dest, val, sizeof(p->routes[0].dest));
                copy(p->routes[p->nroutes].gw, v ? v + 1 : gw, sizeof(p->routes[0].gw));
                p->nroutes++;
            }
        }
        FreeVec(t);
    }
    load_users(p, dir);
    load_groups(p, dir);
    sprintf(path, "%s/dns", dir);
    if ((t = load_file(path)) != NULL) {
        for (q = t; token(&q, tok, sizeof(tok)); ) {
            if (ieq(tok, "NAMESERVER") && token(&q, val, sizeof(val))) { if (nns < 3) copy(p->dns[nns++], val, 16); }
            else if ((ieq(tok, "DOMAIN") || ieq(tok, "SEARCH")) && token(&q, val, sizeof(val))) copy(p->domain, val, sizeof(p->domain));
        }
        FreeVec(t);
    }
    sprintf(path, "%s/hostname", dir);
    if ((t = load_file(path)) != NULL) {
        q = t;
        if (token(&q, tok, sizeof(tok))) copy(p->hostname, tok, sizeof(p->hostname));
        FreeVec(t);
    }
    sprintf(path, "%s/startup", dir);
    if ((t = load_file(path)) != NULL) {
        for (q = t; token(&q, tok, sizeof(tok)); ) {
            char *v = strchr(tok, '=');
            if (!v) continue;
            *v++ = 0;
            if (ieq(tok, "AUTOSTART")) p->autostart = !(ieq(v, "NO") || ieq(v, "OFF") || ieq(v, "0"));
            else if (ieq(tok, "COMMAND")) copy(p->command, v, sizeof(p->command));
            else if (ieq(tok, "MEMORY"))  copy(p->memory, v, sizeof(p->memory));
            else if (ieq(tok, "INETD"))   copy(p->inetd, v, sizeof(p->inetd));
        }
        FreeVec(t);
    }
    p->atboot = boot_entry_present();

    /* user area: USER is a global variable (also read by usergroup.library) */
    if (GetVar((CONST_STRPTR)"USER", (STRPTR)p->user, sizeof(p->user), GVF_GLOBAL_ONLY) <= 0)
        p->user[0] = 0;
    sprintf(path, "%s/realname", dir);
    load_line(path, p->realname, sizeof(p->realname));
    sprintf(path, "%s/workgroup", dir);
    load_line(path, p->workgroup, sizeof(p->workgroup));
}

/* passwd and group in AmiTCP's '|' format (Amiga paths contain ':'),
 * which usergroup.library reads. An empty list
 * removes the file, and usergroup.library falls back to its built-in user. */
static BOOL save_db(const struct Prefs *p, const char *dir)
{
    char path[160], *text;
    int i;
    BOOL ok = TRUE;

    if (!(text = AllocVec(MAXUSERS * 300 + 64, MEMF_ANY | MEMF_CLEAR))) return FALSE;
    sprintf(path, "%s/passwd", dir);
    if (p->nusers) {
        strcpy(text, "# PrismNet users: name|password|uid|gid|real name|home|shell\n");
        for (i = 0; i < p->nusers; i++) {
            const struct PrefUser *u = &p->users[i];
            sprintf(text + strlen(text), "%s|%s|%s|%s|%s|%s|%s\n", u->name, u->passwd, u->uid, u->gid,
                    u->gecos, u->dir[0] ? u->dir : "SYS:", u->shell[0] ? u->shell : "noshell");
        }
        ok = save_file(path, text);
    } else if (exists(path)) {
        drop_file(dir, path);
    }
    sprintf(path, "%s/group", dir);
    if (ok && p->ngroups) {
        strcpy(text, "# PrismNet groups: name|password|gid|member,member\n");
        for (i = 0; i < p->ngroups; i++) {
            const struct PrefGroup *g = &p->groups[i];
            sprintf(text + strlen(text), "%s|%s|%s|%s\n", g->name, g->passwd, g->gid, g->members);
        }
        ok = save_file(path, text);
    } else if (ok && exists(path)) {
        drop_file(dir, path);
    }
    FreeVec(text);
    return ok;
}

/* Remove every file in dir/interfaces, then write the current ones. */
static BOOL save_prefs(const struct Prefs *p, const char *dir)
{
    char path[160], text[1024];
    BPTR lock;
    int i;

    make_dir(dir);
    sprintf(path, "%s/interfaces", dir);
    make_dir(path);
    if ((lock = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        char del[MAXIF * 4][32];
        int nd = 0;
        if (fib) {
            if (Examine(lock, fib))
                while (ExNext(lock, fib) && nd < MAXIF * 4)
                    if (fib->fib_DirEntryType < 0 && strlen(fib->fib_FileName) < 32)
                        strcpy(del[nd++], fib->fib_FileName);
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(lock);
        for (i = 0; i < nd; i++) {
            sprintf(path, "%s/interfaces/%s", dir, del[i]);
            drop_file(dir, path);
        }
    }
    for (i = 0; i < p->nifs; i++) {
        const struct PrefIf *pi = &p->ifs[i];
        sprintf(text, strchr(pi->device, ' ') ? "# PrismNet interface %s\nDEVICE=\"%s\" UNIT=%s\n"
                                              : "# PrismNet interface %s\nDEVICE=%s UNIT=%s\n",
                pi->name, pi->device, pi->unit[0] ? pi->unit : "0");
        if (pi->dhcp)
            strcat(text, "CONFIG=DHCP\n");
        else {
            sprintf(text + strlen(text), "CONFIG=STATIC ADDRESS=%s NETMASK=%s", pi->addr,
                    pi->mask[0] ? pi->mask : "255.255.255.0");
            if (pi->gw[0]) sprintf(text + strlen(text), " GATEWAY=%s", pi->gw);
            strcat(text, "\n");
        }
        if (pi->mtu[0]) sprintf(text + strlen(text), "MTU=%s\n", pi->mtu);
        if (pi->baud[0]) sprintf(text + strlen(text), "BAUD=%s\n", pi->baud);
        if (pi->rtscts) strcat(text, "RTSCTS=YES\n");
        sprintf(path, "%s/interfaces/%s", dir, pi->name);
        if (!save_file(path, text)) return FALSE;
    }

    sprintf(path, "%s/routes", dir);
    text[0] = 0;
    if (p->defgw[0]) sprintf(text, "DEFAULT %s\n", p->defgw);
    for (i = 0; i < p->nroutes; i++)
        sprintf(text + strlen(text), "ROUTE %s %s\n", p->routes[i].dest, p->routes[i].gw);
    if (text[0]) { if (!save_file(path, text)) return FALSE; }
    else drop_file(dir, path);

    if (!save_db(p, dir))
        return FALSE;

    text[0] = 0;
    for (i = 0; i < 3; i++)
        if (p->dns[i][0]) sprintf(text + strlen(text), "NAMESERVER %s\n", p->dns[i]);
    if (p->domain[0]) sprintf(text + strlen(text), "DOMAIN %s\n", p->domain);
    sprintf(path, "%s/dns", dir);
    if (text[0]) { if (!save_file(path, text)) return FALSE; }
    else drop_file(dir, path);

    sprintf(path, "%s/hostname", dir);
    sprintf(text, "%s\n", p->hostname[0] ? p->hostname : "amiga");
    if (!save_file(path, text)) return FALSE;

    sprintf(text, "AUTOSTART=%s\n", p->autostart ? "YES" : "NO");
    if (p->command[0]) sprintf(text + strlen(text), "COMMAND=%s\n", p->command);
    if (p->memory[0]) sprintf(text + strlen(text), "MEMORY=%s\n", p->memory);
    if (p->inetd[0]) sprintf(text + strlen(text), "INETD=%s\n", p->inetd);
    sprintf(path, "%s/startup", dir);
    if (!save_file(path, text)) return FALSE;
    if (!save_services(p, dir)) return FALSE;

    sprintf(path, "%s/realname", dir);
    if (p->realname[0]) { sprintf(text, "%s\n", p->realname); if (!save_file(path, text)) return FALSE; }
    else drop_file(dir, path);
    sprintf(path, "%s/workgroup", dir);
    if (p->workgroup[0]) { sprintf(text, "%s\n", p->workgroup); if (!save_file(path, text)) return FALSE; }
    else drop_file(dir, path);

    if (p->import_hosts[0]) {
        sprintf(path, "%s/hosts", dir);
        copy_file(p->import_hosts, path);
    }
    return TRUE;
}

/* Add/remove the PrismNet block in S:User-Startup. */
static BOOL set_boot_entry(BOOL on)
{
    char *t = load_file(USTARTUP), *b, *e, *out;
    BOOL ok;

    if (!t) { t = AllocVec(4, MEMF_CLEAR); if (!t) return FALSE; }
    out = AllocVec(strlen(t) + 200, MEMF_CLEAR);
    if (!out) { FreeVec(t); return FALSE; }
    b = strstr(t, BOOT_BEGIN);
    if (b) {
        e = strstr(b, BOOT_END);
        e = e ? e + strlen(BOOT_END) : b + strlen(b);
        if (*e == '\n') e++;
        memcpy(out, t, b - t);
        strcat(out, e);
    } else {
        strcpy(out, t);
    }
    if (on) {
        if (out[0] && out[strlen(out) - 1] != '\n') strcat(out, "\n");
        strcat(out, BOOT_BEGIN "\nC:PrismNet START >NIL:\n" BOOT_END "\n");
    }
    ok = (strcmp(out, t) == 0) || save_file(USTARTUP, out);
    FreeVec(out);
    FreeVec(t);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Roadshow import                                                          */
/* ---------------------------------------------------------------------- */

/* Roadshow interface files are "key=value" per line. */
/* One Roadshow setting (key in lower or upper case). */
static void rs_setting(struct PrefIf *pi, const char *k, const char *v)
{
    if (ieq(k, "device")) {
        const char *fp = (const char *)FilePart((STRPTR)v);
        /* DEVS:Networks/x.device -> x.device (PrismNet looks there) */
        copy(pi->device, (strncmp(v, "DEVS:Networks/", 14) == 0 || strncmp(v, "devs:networks/", 14) == 0) ? fp : v,
             sizeof(pi->device));
    }
    else if (ieq(k, "unit"))    copy(pi->unit, v, sizeof(pi->unit));
    else if (ieq(k, "address")) {
        if (ieq(v, "dhcp")) pi->dhcp = TRUE;
        else { copy(pi->addr, v, sizeof(pi->addr)); pi->dhcp = FALSE; }
    }
    else if (ieq(k, "netmask")) copy(pi->mask, v, sizeof(pi->mask));
    else if (ieq(k, "mtu"))     copy(pi->mtu, v, sizeof(pi->mtu));
    else if (ieq(k, "configure"))   /* dhcp, fastdhcp, slowauto, fastauto: all automatic */
        pi->dhcp = !ieq(v, "static") && !ieq(v, "none");
}

/*
 * Roadshow interface files are ReadArgs-like: key=value pairs, several to a
 * line or one per line, "key = value" with spaces, values in quotes.
 */
static void import_roadshow_iface(struct PrefIf *pi, const char *name, char *text)
{
    char tok[128], key[64], *p = text, *v;
    BOOL want_value = FALSE;

    memset(pi, 0, sizeof(*pi));
    copy(pi->name, name, sizeof(pi->name));
    strcpy(pi->unit, "0");
    pi->dhcp = TRUE;
    key[0] = 0;
    while (token(&p, tok, sizeof(tok))) {
        if (want_value) {                       /* "key =" seen, this is the value */
            if (!strcmp(tok, "=")) continue;
            rs_setting(pi, key, tok);
            want_value = FALSE;
        } else if (!strcmp(tok, "=")) {         /* "key = value" */
            want_value = key[0] != 0;
        } else if ((v = strchr(tok, '=')) != NULL) {
            *v++ = 0;
            copy(key, tok, sizeof(key));
            if (*v) { rs_setting(pi, key, v); key[0] = 0; }
            else want_value = TRUE;             /* "key= value" */
        } else {
            copy(key, tok, sizeof(key));        /* maybe "key = value" follows */
        }
    }
}

/* Merge a Roadshow setup under `devs` (normally "DEVS:"). Returns a summary. */
static int import_roadshow(struct Prefs *p, const char *devs, char *summary)
{
    char path[160], tok[128], val[128], *t, *q;
    BPTR lock;
    int found = 0, nifs = 0, nns = 0;
    struct FileInfoBlock *fib;

    sprintf(path, "%sNetInterfaces", devs);
    if ((lock = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
        if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
            if (Examine(lock, fib)) {
                p->nifs = 0;
                while (ExNext(lock, fib) && p->nifs < MAXIF) {
                    const char *fn = fib->fib_FileName;
                    size_t l = strlen(fn);
                    char fpath[200];
                    if (fib->fib_DirEntryType > 0 || l >= 16 || skip_name(fn))
                        continue;
                    sprintf(fpath, "%s/%s", path, fn);
                    if ((t = load_file(fpath)) != NULL) {
                        import_roadshow_iface(&p->ifs[p->nifs], fn, t);
                        if (p->ifs[p->nifs].device[0]) { p->nifs++; nifs++; }
                        FreeVec(t);
                    }
                }
            }
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(lock);
        found++;
    }
    sprintf(path, "%sInternet/name_resolution", devs);
    if ((t = load_file(path)) != NULL) {
        for (q = t; token(&q, tok, sizeof(tok)); ) {
            if (ieq(tok, "nameserver") && token(&q, val, sizeof(val))) { if (nns < 3) copy(p->dns[nns++], val, 16); }
            else if ((ieq(tok, "domain") || ieq(tok, "search")) && token(&q, val, sizeof(val))) copy(p->domain, val, sizeof(p->domain));
        }
        FreeVec(t);
        found++;
    }
    if (nns) { int i; for (i = nns; i < 3; i++) p->dns[i][0] = 0; }
    sprintf(path, "%sInternet/routes", devs);
    if ((t = load_file(path)) != NULL) {
        for (q = t; token(&q, tok, sizeof(tok)); )
            if (ieq(tok, "default") && token(&q, val, sizeof(val)))
                copy(p->defgw, val, sizeof(p->defgw));
        FreeVec(t);
        found++;
    }
    sprintf(path, "%sInternet/hosts", devs);
    if (exists(path)) { copy(p->import_hosts, path, sizeof(p->import_hosts)); found++; }

    sprintf(summary, "Imported %d interface%s, %d nameserver%s%s%s%s.", nifs, nifs == 1 ? "" : "s",
            nns, nns == 1 ? "" : "s", p->defgw[0] ? ", default route" : "",
            p->domain[0] ? ", domain" : "", p->import_hosts[0] ? ", hosts file" : "");
    return found;
}

/* ---------------------------------------------------------------------- */
/* validation                                                               */
/* ---------------------------------------------------------------------- */

static BOOL all_digits(const char *s)
{
    if (!*s) return FALSE;
    for (; *s; s++) if (*s < '0' || *s > '9') return FALSE;
    return TRUE;
}

/* a.b.c.d or a.b.c.d/bits */
static BOOL valid_dest(const char *s)
{
    char ip[20];
    const char *slash = strchr(s, '/');
    int n = slash ? slash - s : (int)strlen(s);
    if (n <= 0 || n >= (int)sizeof(ip)) return FALSE;
    memcpy(ip, s, n); ip[n] = 0;
    if (!valid_ip(ip)) return FALSE;
    return !slash || (all_digits(slash + 1) && atol(slash + 1) <= 32);
}

static BOOL db_safe(const char *s)        /* no field separator or line break (':' is fine) */
{
    return !strpbrk(s, "|\n\r");
}

static const char *check_route(const struct PrefRoute *r)
{
    static char msg[100];
    if (!valid_dest(r->dest)) { sprintf(msg, "\"%s\" is not a valid destination.\nUse a.b.c.d or a.b.c.d/bits.", r->dest); return msg; }
    if (!valid_ip(r->gw))     { sprintf(msg, "\"%s\" is not a valid gateway address.", r->gw); return msg; }
    return NULL;
}

static const char *check_user(const struct PrefUser *u)
{
    static char msg[100];
    if (!u->name[0] || strpbrk(u->name, " :|/")) return "A user name must be set and contain\nno spaces, ':', '|' or '/'.";
    if (!all_digits(u->uid) || !all_digits(u->gid)) { sprintf(msg, "%s: UID and GID must be numbers.", u->name); return msg; }
    if (!db_safe(u->gecos) || !db_safe(u->dir) || !db_safe(u->shell)) { sprintf(msg, "%s: no '|' in the fields.", u->name); return msg; }
    return NULL;
}

static const char *check_group(const struct PrefGroup *g)
{
    static char msg[100];
    if (!g->name[0] || strpbrk(g->name, " :|/,")) return "A group name must be set and contain\nno spaces, ':', '|', '/' or ','.";
    if (!all_digits(g->gid)) { sprintf(msg, "%s: the GID must be a number.", g->name); return msg; }
    if (!db_safe(g->members) || strchr(g->members, ' ')) { sprintf(msg, "%s: members are names separated\nby ',' (no spaces).", g->name); return msg; }
    return NULL;
}

static const char *validate(const struct Prefs *p, int *bad_if)
{
    static char msg[120];
    int i, j;

    *bad_if = -1;
    for (i = 0; i < p->nifs; i++) {
        const struct PrefIf *pi = &p->ifs[i];
        *bad_if = i;
        if (!pi->name[0] || strpbrk(pi->name, ":/ "))  { sprintf(msg, "Interface %d: the name must be set\nand contain no ':', '/' or spaces.", i + 1); return msg; }
        for (j = 0; j < i; j++)
            if (ieq(p->ifs[j].name, pi->name))           { sprintf(msg, "Two interfaces are called \"%s\".", pi->name); return msg; }
        if (!pi->device[0])                              { sprintf(msg, "%s: no device driver set.", pi->name); return msg; }
        if (pi->unit[0] && !all_digits(pi->unit))        { sprintf(msg, "%s: the unit must be a number.", pi->name); return msg; }
        if (pi->mtu[0] && (!all_digits(pi->mtu) || atol(pi->mtu) < 576 || atol(pi->mtu) > 16384))
                                                         { sprintf(msg, "%s: the MTU must be a number\nfrom 576 to 16384 (or empty).", pi->name); return msg; }
        if (pi->baud[0] && (!all_digits(pi->baud) || atol(pi->baud) < 300))
                                                         { sprintf(msg, "%s: the serial speed must be a number\n(e.g. 19200), or empty.", pi->name); return msg; }
        if (!pi->dhcp) {
            if (!valid_ip(pi->addr))                     { sprintf(msg, "%s: \"%s\" is not a valid address.", pi->name, pi->addr); return msg; }
            if (pi->mask[0] && !valid_ip(pi->mask))      { sprintf(msg, "%s: \"%s\" is not a valid netmask.", pi->name, pi->mask); return msg; }
            if (pi->gw[0] && !valid_ip(pi->gw))          { sprintf(msg, "%s: \"%s\" is not a valid gateway.", pi->name, pi->gw); return msg; }
        }
    }
    *bad_if = -1;
    if (p->defgw[0] && !valid_ip(p->defgw))              { sprintf(msg, "\"%s\" is not a valid default gateway.", p->defgw); return msg; }
    for (i = 0; i < 3; i++)
        if (p->dns[i][0] && !valid_ip(p->dns[i]))        { sprintf(msg, "\"%s\" is not a valid DNS server address.", p->dns[i]); return msg; }
    if (!p->hostname[0] || strpbrk(p->hostname, " :/"))   return "The computer name must be set and contain\nno spaces, ':' or '/'.";
    if (p->memory[0] && (!all_digits(p->memory) || atol(p->memory) < 16))
                                                         return "The memory budget must be a number\nof KB, at least 16 (or empty).";
    if (strpbrk(p->user, " :/|"))                        return "The user name must not contain\nspaces, ':', '/' or '|'.";
    if (strpbrk(p->realname, ":|"))                      return "The real name must not contain ':' or '|'.";
    if (strpbrk(p->workgroup, " :/\\*?\"<>|"))             return "The workgroup must not contain spaces\nor any of  : / \\ * ? \" < > |";
    for (i = 0; i < p->nroutes; i++) { const char *e = check_route(&p->routes[i]); if (e) return e; }
    for (i = 0; i < p->nusers; i++)  { const char *e = check_user(&p->users[i]);   if (e) return e; }
    for (i = 0; i < p->ngroups; i++) { const char *e = check_group(&p->groups[i]); if (e) return e; }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* GUI                                                                      */
/* ---------------------------------------------------------------------- */

enum { G_IFLIST = 1, G_ADD, G_REMOVE, G_NAME, G_MODE, G_DEVICE, G_PICK, G_UNIT,
       G_ADDR, G_MASK, G_GW, G_MTU, G_HOST, G_DOMAIN, G_DEFGW, G_DNS1, G_DNS2, G_DNS3,
       G_USER, G_REALNAME, G_WORKGROUP, G_ROUTES, G_ACCOUNTS,
       G_AUTO, G_BOOT, G_HOSTS, G_BAUD, G_RTSCTS, G_MEMORY,
       G_SHLIST, G_SHADD, G_SHREMOVE, G_SHNAME, G_SHTYPE, G_SHHOST, G_SHBROWSE, G_SHSHARE,
       G_SHVOL, G_SHUSER, G_SHUID, G_SHGID, G_SHBOOT, G_SHRO, G_SHMOUNT, G_SHUNMOUNT,
       G_FTPON, G_FTPPORT, G_FTPRO, G_FTPANON, G_FTPDIR, G_FTPPICK, G_FTPPASV, G_FTPINFO, G_FTPNOTE,
       G_TELON, G_TELPORT,
       G_SAVE, G_USE, G_CANCEL, G_SCROLL, G_COUNT };

/* the tabs: each page holds only its own gadgets */
enum { PG_IF, PG_NET, PG_USER, PG_START, PG_SHARE, PG_SERVICE, PG_COUNT };
static const char *page_labels[PG_COUNT] = { "Interfaces", "Network", "Users", "Startup", "Shares", "Services" };
#define G_TAB0 200                      /* tab gadget IDs: G_TAB0 + page */

#define M_IMPORT 1
#define M_LASTSAVED 2
#define M_DEFAULTS 3
#define M_QUIT 4

struct Gui {
    struct Tabs    tabs;
    int            page;
    struct Screen *scr;
    APTR           vi;
    struct Window *win;
    struct Menu   *menu;
    struct Gadget *glist, *g[G_COUNT];
    struct List    iflist;
    struct Node    ifnodes[MAXIF];
    int            sel;                 /* selected interface, -1 = none */
    struct List    shlist;              /* the Shares page */
    struct Node    shnodes[MAXSHARES];
    char           shlabel[MAXSHARES][24];
    int            shsel;
    struct { WORD x, y, w, h; const char *title; } frame[4];   /* section boxes */
    int            nframes;
    /* A screen too small for the whole window (NTSC 640x200, big fonts):
     * the sections scroll under a scroller, Save/Use/Cancel stay put. The
     * scroller and buttons live in their own gadget list (bglist), so a
     * scroll rebuilds only the content gadgets. */
    struct Gadget *bglist;
    BOOL           scrolling;
    WORD           scroll;              /* pixels scrolled */
    WORD           view_top, view_bot;  /* visible content band */
    WORD           unit, total_units, view_units;
};

#define SCROLL_W 18

static const char *mode_labels[] = { "DHCP", "Static", NULL };

static const char *gstr(struct Gui *gui, int id)
{
    struct Gadget *g = gui->g[id];
    return g ? (const char *)((struct StringInfo *)g->SpecialInfo)->Buffer : "";
}

/* Copy a string gadget into the model, unless it is scrolled out of sight
 * (not created): then the model keeps its value. */
static void gcopy(struct Gui *gui, int id, char *dst, int size)
{
    if (gui->g[id])
        copy(dst, gstr(gui, id), size);
}

static void sets(struct Gui *gui, int id, const char *s)
{
    if (gui->g[id]) GT_SetGadgetAttrs(gui->g[id], gui->win, NULL, GTST_String, (ULONG)s, TAG_END);
}

static void dis(struct Gui *gui, int id, BOOL off)
{
    if (gui->g[id]) GT_SetGadgetAttrs(gui->g[id], gui->win, NULL, GA_Disabled, off, TAG_END);
}

static void build_iflist(struct Gui *gui)
{
    int i;
    if (gui->g[G_IFLIST])
        GT_SetGadgetAttrs(gui->g[G_IFLIST], gui->win, NULL, GTLV_Labels, ~0UL, TAG_END);
    gui->iflist.lh_Head = (struct Node *)&gui->iflist.lh_Tail;
    gui->iflist.lh_Tail = NULL;
    gui->iflist.lh_TailPred = (struct Node *)&gui->iflist.lh_Head;
    for (i = 0; i < P.nifs; i++) {
        memset(&gui->ifnodes[i], 0, sizeof(struct Node));
        gui->ifnodes[i].ln_Name = P.ifs[i].name;
        AddTail(&gui->iflist, &gui->ifnodes[i]);
    }
    if (gui->g[G_IFLIST])
        GT_SetGadgetAttrs(gui->g[G_IFLIST], gui->win, NULL, GTLV_Labels, (ULONG)&gui->iflist,
                          GTLV_Selected, gui->sel >= 0 ? (ULONG)gui->sel : ~0UL, TAG_END);
}

/* gadgets -> model for the selected interface */
static void store_if(struct Gui *gui)
{
    struct PrefIf *pi;
    if (gui->sel < 0 || gui->sel >= P.nifs) return;
    pi = &P.ifs[gui->sel];
    gcopy(gui, G_NAME, pi->name, sizeof(pi->name));
    gcopy(gui, G_DEVICE, pi->device, sizeof(pi->device));
    gcopy(gui, G_UNIT, pi->unit, sizeof(pi->unit));
    gcopy(gui, G_ADDR, pi->addr, sizeof(pi->addr));
    gcopy(gui, G_MASK, pi->mask, sizeof(pi->mask));
    gcopy(gui, G_GW, pi->gw, sizeof(pi->gw));
    gcopy(gui, G_MTU, pi->mtu, sizeof(pi->mtu));
    gcopy(gui, G_BAUD, pi->baud, sizeof(pi->baud));
}

/* model -> gadgets for the selected interface */
static void show_if(struct Gui *gui)
{
    static const int fields[] = { G_NAME, G_MODE, G_DEVICE, G_PICK, G_UNIT, G_MTU };
    struct PrefIf *pi = (gui->sel >= 0 && gui->sel < P.nifs) ? &P.ifs[gui->sel] : NULL;
    unsigned i;

    sets(gui, G_NAME, pi ? pi->name : "");
    sets(gui, G_DEVICE, pi ? pi->device : "");
    sets(gui, G_UNIT, pi ? pi->unit : "");
    sets(gui, G_ADDR, pi ? pi->addr : "");
    sets(gui, G_MASK, pi ? pi->mask : "");
    sets(gui, G_GW, pi ? pi->gw : "");
    sets(gui, G_MTU, pi ? pi->mtu : "");
    sets(gui, G_BAUD, pi ? pi->baud : "");
    if (gui->g[G_RTSCTS])
        GT_SetGadgetAttrs(gui->g[G_RTSCTS], gui->win, NULL, GTCB_Checked, pi && pi->rtscts, TAG_END);
    dis(gui, G_BAUD, pi == NULL);
    dis(gui, G_RTSCTS, pi == NULL);
    if (gui->g[G_MODE])
        GT_SetGadgetAttrs(gui->g[G_MODE], gui->win, NULL, GTCY_Active, pi && !pi->dhcp ? 1 : 0, TAG_END);
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        dis(gui, fields[i], pi == NULL);
    dis(gui, G_ADDR, !pi || pi->dhcp);
    dis(gui, G_MASK, !pi || pi->dhcp);
    dis(gui, G_GW,   !pi || pi->dhcp);
    dis(gui, G_REMOVE, pi == NULL);
    dis(gui, G_ADD, P.nifs >= MAXIF);
}

/* globals -> model */
static void store_globals(struct Gui *gui)
{
    gcopy(gui, G_HOST, P.hostname, sizeof(P.hostname));
    gcopy(gui, G_DOMAIN, P.domain, sizeof(P.domain));
    gcopy(gui, G_DEFGW, P.defgw, sizeof(P.defgw));
    gcopy(gui, G_DNS1, P.dns[0], 16);
    gcopy(gui, G_DNS2, P.dns[1], 16);
    gcopy(gui, G_DNS3, P.dns[2], 16);
    gcopy(gui, G_USER, P.user, sizeof(P.user));
    gcopy(gui, G_REALNAME, P.realname, sizeof(P.realname));
    gcopy(gui, G_WORKGROUP, P.workgroup, sizeof(P.workgroup));
    gcopy(gui, G_MEMORY, P.memory, sizeof(P.memory));
}

static void build_shlist(struct Gui *gui);
static void show_services(struct Gui *gui);
static void store_services(struct Gui *gui);
static void show_share(struct Gui *gui);
static void store_share(struct Gui *gui);

static void show_all(struct Gui *gui)
{
    if (gui->sel >= P.nifs) gui->sel = P.nifs - 1;
    if (gui->sel < 0 && P.nifs) gui->sel = 0;
    build_iflist(gui);
    show_if(gui);
    if (gui->shsel >= P.nshares) gui->shsel = P.nshares - 1;
    if (gui->shsel < 0 && P.nshares) gui->shsel = 0;
    build_shlist(gui);
    show_share(gui);
    show_services(gui);
    sets(gui, G_HOST, P.hostname);
    sets(gui, G_DOMAIN, P.domain);
    sets(gui, G_DEFGW, P.defgw);
    sets(gui, G_DNS1, P.dns[0]);
    sets(gui, G_DNS2, P.dns[1]);
    sets(gui, G_DNS3, P.dns[2]);
    sets(gui, G_USER, P.user);
    sets(gui, G_REALNAME, P.realname);
    sets(gui, G_WORKGROUP, P.workgroup);
    sets(gui, G_MEMORY, P.memory);
    if (gui->g[G_AUTO]) GT_SetGadgetAttrs(gui->g[G_AUTO], gui->win, NULL, GTCB_Checked, P.autostart, TAG_END);
    if (gui->g[G_BOOT]) GT_SetGadgetAttrs(gui->g[G_BOOT], gui->win, NULL, GTCB_Checked, P.atboot, TAG_END);
}

static LONG ask(struct Gui *gui, const char *title, const char *text, const char *buttons)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)title, (STRPTR)text, (STRPTR)buttons };
    return EasyRequestArgs(gui ? gui->win : NULL, &es, NULL, NULL);
}

#include "prefsshares_gui.c"
#include "prefsservices_gui.c"

static WORD tlen(struct Gui *gui, const char *s)
{
    return (WORD)TextLength(&gui->scr->RastPort, (STRPTR)s, strlen(s));
}

/* Layout metrics shared by make_gadgets() and the window height. */
#define GH(fh)    ((fh) + 4)                 /* gadget height */
#define ROW(fh)   ((fh) + 6)                 /* row pitch */
#define TITLE(fh) ((fh) + 3)                 /* section title band */
#define FRAME_H(fh, rows) (TITLE(fh) + (rows) * ROW(fh) + 3)

#define TABS_H(fh) ((fh) + 8)          /* the tab row (tabs.c) */

static WORD layout_height(WORD fh)
{
    /* tabs + the tallest page (Interfaces: 5 rows) + the button row */
    return 3 + TABS_H(fh) + 4 + (fh) / 2 + FRAME_H(fh, 5) + 6 + GH(fh) + 5;
}

static void add_frame(struct Gui *gui, WORD x, WORD y, WORD w, WORD h, const char *title)
{
    gui->frame[gui->nframes].x = x;
    gui->frame[gui->nframes].y = y;
    gui->frame[gui->nframes].w = w;
    gui->frame[gui->nframes].h = h;
    gui->frame[gui->nframes].title = title;
    gui->nframes++;
}

/* Recessed bevel boxes with a title sitting on the top edge. */
static void draw_frames(struct Gui *gui)
{
    struct RastPort *rp = gui->win->RPort;
    struct DrawInfo *dri = GetScreenDrawInfo(gui->scr);
    struct Region *clip = NULL, *old = NULL;
    int i;

    if (gui->scrolling && LayersBase && (clip = NewRegion()) != NULL) {
        struct Rectangle r;             /* layer (window) coordinates */
        r.MinX = gui->win->BorderLeft;
        r.MaxX = gui->win->Width - gui->win->BorderRight - SCROLL_W - 8;
        r.MinY = gui->view_top;
        r.MaxY = gui->view_bot - 1;
        OrRectRegion(clip, &r);
        old = InstallClipRegion(gui->win->WLayer, clip);
    }

    for (i = 0; i < gui->nframes; i++) {
        WORD x = gui->frame[i].x, y = gui->frame[i].y, tw;
        const char *t = gui->frame[i].title;

        DrawBevelBox(rp, x, y, gui->frame[i].w, gui->frame[i].h,
                     GT_VisualInfo, (ULONG)gui->vi, GTBB_Recessed, TRUE, TAG_END);
        tw = TextLength(rp, (STRPTR)t, strlen(t));
        if (dri) SetAPen(rp, dri->dri_Pens[BACKGROUNDPEN]);
        RectFill(rp, x + 8, y - rp->TxBaseline + 1, x + 8 + tw + 8, y + rp->TxHeight - rp->TxBaseline);
        if (dri) SetAPen(rp, dri->dri_Pens[HIGHLIGHTTEXTPEN]);
        SetDrMd(rp, JAM1);
        Move(rp, x + 12, y + 1);
        Text(rp, (STRPTR)t, strlen(t));
    }
    if (dri) FreeScreenDrawInfo(gui->scr, dri);
    if (clip) {
        InstallClipRegion(gui->win->WLayer, old);
        DisposeRegion(clip);
    }
}

static BOOL make_gadgets(struct Gui *gui);

/* The window scrolled: rebuild the content gadgets at the new offset. */
static void relayout(struct Gui *gui)
{
    struct Window *w = gui->win;
    store_if(gui);
    store_globals(gui);
    store_share(gui);
    store_services(gui);
    if (gui->g[G_IFLIST])
        GT_SetGadgetAttrs(gui->g[G_IFLIST], w, NULL, GTLV_Labels, ~0UL, TAG_END);
    RemoveGList(w, gui->glist, -1);
    FreeGadgets(gui->glist);
    gui->glist = NULL;
    SetAPen(w->RPort, 0);
    EraseRect(w->RPort, w->BorderLeft, gui->view_top,
              w->Width - w->BorderRight - 1 - (gui->scrolling ? SCROLL_W + 8 : 0), gui->view_bot - 1);
    if (!make_gadgets(gui)) return;
    AddGList(w, gui->glist, ~0, -1, NULL);
    RefreshGList(gui->glist, w, NULL, -1);
    GT_RefreshWindow(w, NULL);
    draw_frames(gui);
    show_all(gui);
}

static BOOL make_gadgets(struct Gui *gui)
{
    struct NewGadget ng;
    struct Gadget *g;
    struct Window *w = gui->win;
    WORD fh = gui->scr->Font->ta_YSize, gh = GH(fh), row = ROW(fh);
    WORD fx0 = w->BorderLeft + 4;
    WORD fx1 = w->Width - w->BorderRight - 5 - (gui->scrolling ? SCROLL_W + 4 : 0);   /* frames */
    WORD x0 = fx0 + 8, x1 = fx1 - 8;                                        /* contents */
    WORD y = gui->view_top + 3 + fh / 2 - gui->scroll, fy, lw = 104, lab, rlab, rx, colw, half, x, xr, hw, bw;

    memset(gui->g, 0, sizeof(gui->g));
    gui->nframes = 0;
    g = CreateContext(&gui->glist);
    if (!g) return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = gui->vi;
    ng.ng_TextAttr = gui->scr->Font;

    /* gadgets only where they are wholly visible (scrolled windows) */
#define MK(kind, id, l, t, wd, ht, txt, flags, ...) \
    if (!gui->scrolling || ((t) >= gui->view_top && (t) + (ht) <= gui->view_bot)) { \
        ng.ng_LeftEdge = (l); ng.ng_TopEdge = (t); ng.ng_Width = (wd); ng.ng_Height = (ht); \
        ng.ng_GadgetText = (UBYTE *)(txt); ng.ng_Flags = (flags); ng.ng_GadgetID = (id); \
        g = gui->g[id] = CreateGadget(kind, g, &ng, __VA_ARGS__, TAG_END); \
    } else gui->g[id] = NULL

    switch (gui->page) {
    case PG_IF:     /* ---- Network interfaces --------------------------------- */
        fy = y;
        add_frame(gui, fx0, fy, fx1 - fx0, FRAME_H(fh, 5), "Network interfaces");
        y = fy + TITLE(fh);
        MK(LISTVIEW_KIND, G_IFLIST, x0, y, lw, 4 * row + gh - row + 2, NULL, 0,
           GTLV_Labels, (ULONG)&gui->iflist, GTLV_ShowSelected, 0UL);
        MK(BUTTON_KIND, G_ADD, x0, y + 4 * row, (lw - 4) / 2, gh, "Add", PLACETEXT_IN, GT_Underscore, 0);
        MK(BUTTON_KIND, G_REMOVE, x0 + (lw + 4) / 2, y + 4 * row, (lw - 4) / 2, gh, "Remove", PLACETEXT_IN, GT_Underscore, 0);
        lab  = tlen(gui, "Address") + 12;
        rx   = x0 + lw + 12 + lab;
        colw = (x1 - rx - lab - 12) / 2;
        half = rx + colw + 12 + lab;
        MK(STRING_KIND, G_NAME, rx, y, colw, gh, "Name", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(CYCLE_KIND, G_MODE, half, y, x1 - half, gh, "Mode", PLACETEXT_LEFT, GTCY_Labels, (ULONG)mode_labels);
        MK(STRING_KIND, G_DEVICE, rx, y + row, x1 - rx - 112, gh, "Device", PLACETEXT_LEFT, GTST_MaxChars, 79);
        MK(BUTTON_KIND, G_PICK, x1 - 108, y + row, 28, gh, "...", PLACETEXT_IN, GT_Underscore, 0);
        MK(STRING_KIND, G_UNIT, x1 - 36, y + row, 36, gh, "Unit", PLACETEXT_LEFT, GTST_MaxChars, 4);
        MK(STRING_KIND, G_ADDR, rx, y + 2 * row, colw, gh, "Address", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(STRING_KIND, G_MASK, half, y + 2 * row, x1 - half, gh, "Netmask", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(STRING_KIND, G_GW, rx, y + 3 * row, colw, gh, "Gateway", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(STRING_KIND, G_MTU, half, y + 3 * row, 56, gh, "MTU", PLACETEXT_LEFT, GTST_MaxChars, 5);
        /* serial SLIP (DEVICE=serial.device): speed and handshake */
        MK(STRING_KIND, G_BAUD, rx, y + 4 * row, 72, gh, "Speed", PLACETEXT_LEFT, GTST_MaxChars, 7);
        MK(CHECKBOX_KIND, G_RTSCTS, half, y + 4 * row + (gh - fh - 3) / 2, 26, fh + 3, "RTS/CTS",
           PLACETEXT_LEFT, GTCB_Scaled, TRUE);
        break;

    case PG_NET:    /* ---- Network settings ----------------------------------- */
        fy = y;
        add_frame(gui, fx0, fy, fx1 - fx0, FRAME_H(fh, 4), "Network settings");
        y = fy + TITLE(fh);
        lab  = tlen(gui, "Computer name") + 12;      /* left label column */
        rlab = tlen(gui, "Default gateway") + 12;    /* right label column */
        x    = x0 + lab;
        hw   = (x1 - x - rlab - 12) / 2;             /* width of each half */
        xr   = x + hw + 12 + rlab;
        MK(STRING_KIND, G_HOST, x, y, hw, gh, "Computer name", PLACETEXT_LEFT, GTST_MaxChars, 63);
        MK(STRING_KIND, G_WORKGROUP, xr, y, x1 - xr, gh, "Workgroup", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(STRING_KIND, G_DOMAIN, x, y + row, hw, gh, "Domain", PLACETEXT_LEFT, GTST_MaxChars, 63);
        MK(STRING_KIND, G_DEFGW, xr, y + row, x1 - xr, gh, "Default gateway", PLACETEXT_LEFT, GTST_MaxChars, 15);
        colw = (x1 - x - 2 * 8) / 3;
        MK(STRING_KIND, G_DNS1, x, y + 2 * row, colw, gh, "DNS servers", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(STRING_KIND, G_DNS2, x + colw + 8, y + 2 * row, colw, gh, NULL, 0, GTST_MaxChars, 15);
        MK(STRING_KIND, G_DNS3, x + 2 * (colw + 8), y + 2 * row, colw, gh, NULL, 0, GTST_MaxChars, 15);
        bw = tlen(gui, "Edit hosts...") + 16;
        MK(BUTTON_KIND, G_ROUTES, x, y + 3 * row, bw, gh, "Routes...", PLACETEXT_IN, GT_Underscore, 0);
        MK(BUTTON_KIND, G_HOSTS, x + bw + 8, y + 3 * row, bw, gh, "Edit hosts...", PLACETEXT_IN, GT_Underscore, 0);
        break;

    case PG_USER:   /* ---- User ----------------------------------------------- */
        fy = y;
        add_frame(gui, fx0, fy, fx1 - fx0, FRAME_H(fh, 2), "User");
        y = fy + TITLE(fh);
        lab = tlen(gui, "User name") + 12;
        x   = x0 + lab;
        {
            WORD rl = tlen(gui, "Real name") + 12, uw = (x1 - x - rl - 12) / 3, rnx = x + uw + 12 + rl;
            MK(STRING_KIND, G_USER, x, y, uw, gh, "User name", PLACETEXT_LEFT, GTST_MaxChars, 31);
            MK(STRING_KIND, G_REALNAME, rnx, y, x1 - rnx, gh, "Real name", PLACETEXT_LEFT, GTST_MaxChars, 63);
        }
        bw = tlen(gui, "Accounts...") + 16;
        MK(BUTTON_KIND, G_ACCOUNTS, x, y + row, bw, gh, "Accounts...", PLACETEXT_IN, GT_Underscore, 0);
        break;

    case PG_START:  /* ---- Startup -------------------------------------------- */
        fy = y;
        add_frame(gui, fx0, fy, fx1 - fx0, FRAME_H(fh, 2), "Startup");
        y = fy + TITLE(fh);
        MK(CHECKBOX_KIND, G_AUTO, x0, y + (gh - fh - 3) / 2, 26, fh + 3, "Start when needed", PLACETEXT_RIGHT,
           GTCB_Checked, P.autostart, GTCB_Scaled, TRUE);
        MK(CHECKBOX_KIND, G_BOOT, x0 + 26 + tlen(gui, "Start when needed") + 24, y + (gh - fh - 3) / 2, 26, fh + 3,
           "Start at boot", PLACETEXT_RIGHT, GTCB_Checked, P.atboot, GTCB_Scaled, TRUE);
        lab = tlen(gui, "Memory budget (KB)") + 12;
        MK(STRING_KIND, G_MEMORY, x0 + lab, y + row, 64, gh, "Memory budget (KB)", PLACETEXT_LEFT,
           GTST_MaxChars, 6);
        break;

    case PG_SHARE:  /* ---- Network shares ------------------------------------- */
        fy = y;
        add_frame(gui, fx0, fy, fx1 - fx0, FRAME_H(fh, 5), "Network shares (NFS, SMB)");
        y = fy + TITLE(fh);
        MK(LISTVIEW_KIND, G_SHLIST, x0, y, lw, 4 * row + gh - row + 2, NULL, 0,
           GTLV_Labels, (ULONG)&gui->shlist, GTLV_ShowSelected, 0UL);
        MK(BUTTON_KIND, G_SHADD, x0, y + 4 * row, (lw - 4) / 2, gh, "Add", PLACETEXT_IN, GT_Underscore, 0);
        MK(BUTTON_KIND, G_SHREMOVE, x0 + (lw + 4) / 2, y + 4 * row, (lw - 4) / 2, gh, "Remove", PLACETEXT_IN, GT_Underscore, 0);
        lab  = tlen(gui, "Volume") + 12;
        rx   = x0 + lw + 12 + lab;
        colw = (x1 - rx - lab - 12) / 2;
        half = rx + colw + 12 + lab;
        bw   = tlen(gui, "Browse...") + 16;
        MK(STRING_KIND, G_SHNAME, rx, y, colw, gh, "Device", PLACETEXT_LEFT, GTST_MaxChars, 15);
        MK(CYCLE_KIND, G_SHTYPE, half, y, x1 - half, gh, "Type", PLACETEXT_LEFT, GTCY_Labels, (ULONG)share_type_labels);
        MK(STRING_KIND, G_SHHOST, rx, y + row, x1 - rx - bw - 8, gh, "Server", PLACETEXT_LEFT, GTST_MaxChars, 63);
        MK(BUTTON_KIND, G_SHBROWSE, x1 - bw, y + row, bw, gh, "Browse...", PLACETEXT_IN, GT_Underscore, 0);
        MK(STRING_KIND, G_SHSHARE, rx, y + 2 * row, colw, gh, "Share", PLACETEXT_LEFT, GTST_MaxChars, 63);
        MK(STRING_KIND, G_SHVOL, half, y + 2 * row, x1 - half, gh, "Volume", PLACETEXT_LEFT, GTST_MaxChars, 31);
        MK(STRING_KIND, G_SHUSER, rx, y + 3 * row, colw, gh, "User", PLACETEXT_LEFT, GTST_MaxChars, 31);
        MK(STRING_KIND, G_SHUID, half, y + 3 * row, 48, gh, "UID", PLACETEXT_LEFT, GTST_MaxChars, 7);
        MK(STRING_KIND, G_SHGID, half + 48 + tlen(gui, "GID") + 12, y + 3 * row, 48, gh, "GID", PLACETEXT_LEFT, GTST_MaxChars, 7);
        MK(CHECKBOX_KIND, G_SHBOOT, rx - lab, y + 4 * row + (gh - fh - 3) / 2, 26, fh + 3, "Mount at boot",
           PLACETEXT_RIGHT, GTCB_Scaled, TRUE);
        MK(CHECKBOX_KIND, G_SHRO, rx - lab + 26 + tlen(gui, "Mount at boot") + 20, y + 4 * row + (gh - fh - 3) / 2,
           26, fh + 3, "Read-only", PLACETEXT_RIGHT, GTCB_Scaled, TRUE);
        bw = tlen(gui, "Dismount") + 16;
        MK(BUTTON_KIND, G_SHUNMOUNT, x1 - bw, y + 4 * row, bw, gh, "Dismount", PLACETEXT_IN, GT_Underscore, 0);
        MK(BUTTON_KIND, G_SHMOUNT, x1 - bw - 8 - (tlen(gui, "Mount now") + 16), y + 4 * row,
           tlen(gui, "Mount now") + 16, gh, "Mount now", PLACETEXT_IN, GT_Underscore, 0);
        break;

    case PG_SERVICE:  /* ---- Servers this Amiga runs -------------------------- */
        fy = y;
        add_frame(gui, fx0, fy, fx1 - fx0, FRAME_H(fh, 5), "Servers");
        y = fy + TITLE(fh);
        {
            WORD cy = (gh - fh - 3) / 2, px, rx2, ax;
            MK(CHECKBOX_KIND, G_FTPON, x0, y + cy, 26, fh + 3, "Run the FTP server", PLACETEXT_RIGHT,
               GTCB_Checked, P.ftp_on, GTCB_Scaled, TRUE);
            px = x0 + 26 + tlen(gui, "Run the FTP server") + 24 + tlen(gui, "Port") + 8;
            MK(STRING_KIND, G_FTPPORT, px, y, 56, gh, "Port", PLACETEXT_LEFT, GTST_MaxChars, 5);
            rx2 = px + 56 + 24;
            MK(CHECKBOX_KIND, G_FTPRO, rx2, y + cy, 26, fh + 3, "Read-only for everyone", PLACETEXT_RIGHT,
               GTCB_Checked, P.ftp_ro, GTCB_Scaled, TRUE);
            ax = x0 + 26 + tlen(gui, "Anonymous, read-only, in") + 16;
            bw = 28;
            MK(CHECKBOX_KIND, G_FTPANON, x0, y + row + cy, 26, fh + 3, "Anonymous, read-only, in", PLACETEXT_RIGHT,
               GTCB_Checked, P.ftp_anon, GTCB_Scaled, TRUE);
            MK(STRING_KIND, G_FTPDIR, ax, y + row, x1 - ax - bw - 6, gh, NULL, 0, GTST_MaxChars, 127);
            MK(BUTTON_KIND, G_FTPPICK, x1 - bw, y + row, bw, gh, "...", PLACETEXT_IN, GT_Underscore, 0);
            lab = tlen(gui, "Passive ports") + 12;
            MK(STRING_KIND, G_FTPPASV, x0 + lab, y + 2 * row, 120, gh, "Passive ports", PLACETEXT_LEFT,
               GTST_MaxChars, 15);
            MK(TEXT_KIND, G_FTPINFO, x0 + lab + 128, y + 2 * row, x1 - x0 - lab - 128, gh, NULL, 0,
               GTTX_Text, (ULONG)"e.g. 50000-50019; empty = any", GTTX_CopyText, TRUE);
            MK(CHECKBOX_KIND, G_TELON, x0, y + 3 * row + cy, 26, fh + 3, "Run the telnet server", PLACETEXT_RIGHT,
               GTCB_Checked, P.tel_on, GTCB_Scaled, TRUE);
            px = x0 + 26 + tlen(gui, "Run the telnet server") + 24 + tlen(gui, "Port") + 8;
            MK(STRING_KIND, G_TELPORT, px, y + 3 * row, 56, gh, "Port", PLACETEXT_LEFT, GTST_MaxChars, 5);
            MK(TEXT_KIND, G_FTPNOTE, x0, y + 4 * row, x1 - x0, gh, NULL, 0,
               GTTX_Text, (ULONG)"Logins: the users in Users > Accounts. Telnet gives them a Shell.",
               GTTX_CopyText, TRUE);
        }
        break;
    }

#undef MK
    (void)rlab;
    return g != NULL;
}

/* Save / Use / Cancel and (when scrolling) the scroller: never rebuilt. */
static BOOL make_base(struct Gui *gui)
{
    struct NewGadget ng;
    struct Gadget *g;
    struct Window *w = gui->win;
    WORD fh = gui->scr->Font->ta_YSize, gh = GH(fh);
    WORD fx0 = w->BorderLeft + 4, fx1 = w->Width - w->BorderRight - 5;
    WORD yb = w->Height - w->BorderBottom - gh - 4, bw = (fx1 - fx0) / 4;

    if (!(g = CreateContext(&gui->bglist))) return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = gui->vi;
    ng.ng_TextAttr = gui->scr->Font;
#define MB(kind, id, l, t, wd, ht, txt, flags, ...) \
    ng.ng_LeftEdge = (l); ng.ng_TopEdge = (t); ng.ng_Width = (wd); ng.ng_Height = (ht); \
    ng.ng_GadgetText = (UBYTE *)(txt); ng.ng_Flags = (flags); ng.ng_GadgetID = (id); \
    g = gui->g[id] = CreateGadget(kind, g, &ng, __VA_ARGS__, TAG_END)
    MB(BUTTON_KIND, G_SAVE, fx0, yb, bw, gh, "_Save", PLACETEXT_IN, GT_Underscore, '_');
    MB(BUTTON_KIND, G_USE, fx0 + (fx1 - fx0 - bw) / 2, yb, bw, gh, "_Use", PLACETEXT_IN, GT_Underscore, '_');
    MB(BUTTON_KIND, G_CANCEL, fx1 - bw, yb, bw, gh, "_Cancel", PLACETEXT_IN, GT_Underscore, '_');
    if (gui->scrolling) {
        MB(SCROLLER_KIND, G_SCROLL, fx1 - SCROLL_W, gui->view_top + 2, SCROLL_W, gui->view_bot - gui->view_top - 2,
           NULL, 0, GTSC_Top, 0, GTSC_Total, gui->total_units, GTSC_Visible, gui->view_units,
           GTSC_Arrows, fh + 3, PGA_Freedom, LORIENT_VERT, GA_RelVerify, TRUE, GA_Immediate, TRUE);
    } else {
        gui->g[G_SCROLL] = NULL;
    }
#undef MB
    return g != NULL;
}

static struct NewMenu newmenu[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Import Roadshow settings...", (STRPTR)"I", 0, 0, (APTR)M_IMPORT },
    { NM_ITEM,  NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)M_QUIT },
    { NM_TITLE, (STRPTR)"Edit", NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Reset to defaults", (STRPTR)"D", 0, 0, (APTR)M_DEFAULTS },
    { NM_ITEM,  (STRPTR)"Last saved", (STRPTR)"L", 0, 0, (APTR)M_LASTSAVED },
    { NM_END,   NULL, NULL, 0, 0, NULL }
};

static void pick_device(struct Gui *gui)
{
    struct FileRequester *fr;
    if (!AslBase) { DisplayBeep(gui->scr); return; }
    fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (ULONG)"Choose a SANA-II network driver",
                             ASLFR_InitialDrawer, (ULONG)"DEVS:Networks", ASLFR_InitialPattern, (ULONG)"#?.device",
                             ASLFR_DoPatterns, TRUE, ASLFR_Window, (ULONG)gui->win, TAG_END);
    if (!fr) return;
    if (AslRequestTags(fr, TAG_END) && fr->fr_File[0]) {
        char path[200];
        if (ieq((const char *)fr->fr_Drawer, "DEVS:Networks") || ieq((const char *)fr->fr_Drawer, "DEVS:Networks/"))
            copy(path, (const char *)fr->fr_File, sizeof(path));
        else {
            copy(path, (const char *)fr->fr_Drawer, sizeof(path));
            AddPart((STRPTR)path, fr->fr_File, sizeof(path));
        }
        sets(gui, G_DEVICE, path);
        store_if(gui);
    }
    FreeAslRequest(fr);
}

static void edit_hosts(struct Gui *gui)
{
    char editor[100], cmd[200];
    BPTR in, out;

    make_dir(ENV_DIR);
    if (!exists(ENV_DIR "/hosts"))
        save_file(ENV_DIR "/hosts", "# PrismNet hosts: address name [aliases...]\n# 192.168.1.10  nas  nas.home.lan\n");
    if (GetVar((CONST_STRPTR)"EDITOR", (STRPTR)editor, sizeof(editor), 0) <= 0)
        strcpy(editor, "C:Ed");
    sprintf(cmd, "%s " ENV_DIR "/hosts", editor);
    in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        ask(gui, "PrismNet Prefs", "Could not start the editor.\nSet ENV:EDITOR or install C:Ed.", "OK");
    }
}

/* ---------------------------------------------------------------------- */
/* list editor window (routes, users, groups)                               */
/* ---------------------------------------------------------------------- */

struct TCol { const char *label; int off, size, maxch; };
struct Table {
    const char *title;
    const struct TCol *cols;
    int ncols, rowsize, maxrows;
    void (*label)(const void *row, char *out);           /* list line, max 63 chars */
    const char *(*check)(const void *row);
    void (*newrow)(void *row, const void *rows, int n);  /* defaults for Add */
    int passoff;                    /* >= 0: the row's password hash, set with Password... */
};

#define T_LIST 1
#define T_ADD 2
#define T_REMOVE 3
#define T_OK 4
#define T_CANCEL 5
#define T_PASS 6
#define T_FIELD 7       /* T_FIELD + column */
#define T_MAXCOL 6

/*
 * Account passwords. The hash is usergroup.library's crypt() (lib/usergroup.c:
 * 2 salt characters + 11 of a stretched 64-bit FNV-1a of salt + key); it must
 * stay the same as there, or the servers won't accept the password.
 */
static const char pw_b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void pw_hash(const char *key, char s0, char s1, char *out)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    const UBYTE *p;
    int i, round;
    for (round = 0; round < 64; round++) {
        h ^= (UBYTE)s0; h *= 0x100000001b3ULL;
        h ^= (UBYTE)s1; h *= 0x100000001b3ULL;
        for (p = (const UBYTE *)key; *p; p++) { h ^= *p; h *= 0x100000001b3ULL; }
    }
    out[0] = s0;
    out[1] = s1;
    for (i = 0; i < 11; i++) { out[2 + i] = pw_b64[(ULONG)h & 63]; h >>= 6; }
    out[13] = 0;
}

/* typing in a password field shows '*': the edit hook keeps the real
 * characters aside (append and backspace only, as in tools/smb/passreq.c) */
struct Secret { char s[64]; int n; };

static ULONG mask_hook(struct Hook *h __asm("a0"), struct SGWork *sgw __asm("a2"), ULONG *msg __asm("a1"))
{
    struct Secret *x = h->h_Data;
    if (*msg != SGH_KEY)
        return 0;
    switch (sgw->EditOp) {
    case EO_INSERTCHAR:
    case EO_REPLACECHAR:
        if (sgw->BufferPos == sgw->NumChars && x->n < (int)sizeof(x->s) - 1) {
            x->s[x->n++] = sgw->WorkBuffer[sgw->BufferPos - 1];
            sgw->WorkBuffer[sgw->BufferPos - 1] = '*';
        } else {
            sgw->Actions &= ~SGA_USE;
            sgw->Actions |= SGA_BEEP;
        }
        break;
    case EO_DELBACKWARD:
        if (x->n > 0 && sgw->BufferPos == sgw->NumChars) x->n--;
        else sgw->Actions &= ~SGA_USE;
        break;
    case EO_CLEAR:
    case EO_RESET:
        x->n = 0;
        break;
    case EO_ENTER:
    case EO_NOOP:
        break;
    default:                            /* cursor moves, delete-forward: no */
        sgw->Actions &= ~SGA_USE;
        break;
    }
    return ~0UL;
}

/*
 * The new password for `user`, typed twice: hash (14 bytes) gets its crypt()
 * hash, or "" for no password. FALSE: cancelled, nothing changes.
 */
static BOOL ask_new_password(struct Gui *parent, struct Window *over, const char *user, char *hash)
{
    struct Screen *scr = parent->scr;
    struct Window *w = NULL;
    struct Gadget *glist = NULL, *g, *gad[5];
    struct NewGadget ng;
    struct Hook hook[2];
    struct Secret sec[2];
    char title[64];
    WORD fh = scr->Font->ta_YSize, gh = GH(fh), row = ROW(fh), lab, ww, wh, x0, y0;
    BOOL done = FALSE, ok = FALSE;
    int k;

    memset(sec, 0, sizeof(sec));
    memset(hook, 0, sizeof(hook));
    for (k = 0; k < 2; k++) {
        *(APTR *)&hook[k].h_Entry = (APTR)mask_hook;    /* field types vary between NDKs */
        hook[k].h_Data = &sec[k];
    }
    lab = tlen(parent, "New password") + 12;
    ww = tlen(parent, "Both empty: no password, so no FTP or telnet login.") + 40;
    if (ww < 360) ww = 360;
    if (ww > scr->Width - 16) ww = scr->Width - 16;
    wh = scr->WBorTop + fh + 1 + 6 + 3 * row + 8 + gh + 6 + scr->WBorBottom;
    snprintf(title, sizeof(title), "Password for %s", user);
    w = OpenWindowTags(NULL, WA_Title, (ULONG)title, WA_PubScreen, (ULONG)scr,
        WA_Left, over->LeftEdge + 30, WA_Top, over->TopEdge + 30, WA_Width, ww, WA_Height, wh,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP | TEXTIDCMP,
        TAG_END);
    if (!w) return FALSE;
    if (!(g = CreateContext(&glist))) goto out;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = parent->vi;
    ng.ng_TextAttr = scr->Font;
    x0 = w->BorderLeft + 8;
    y0 = w->BorderTop + 6;
#define PK(kind, id, l, tp, wd, txt, flags, ...) \
    ng.ng_LeftEdge = (l); ng.ng_TopEdge = (tp); ng.ng_Width = (wd); ng.ng_Height = gh; \
    ng.ng_GadgetText = (UBYTE *)(txt); ng.ng_Flags = (flags); ng.ng_GadgetID = (id); \
    g = gad[id] = CreateGadget(kind, g, &ng, __VA_ARGS__, TAG_END)
    PK(STRING_KIND, 0, x0 + lab, y0, w->Width - w->BorderRight - 8 - x0 - lab, "New password", PLACETEXT_LEFT,
       GTST_MaxChars, 63, GTST_EditHook, (ULONG)&hook[0]);
    PK(STRING_KIND, 1, x0 + lab, y0 + row, w->Width - w->BorderRight - 8 - x0 - lab, "Again", PLACETEXT_LEFT,
       GTST_MaxChars, 63, GTST_EditHook, (ULONG)&hook[1]);
    PK(TEXT_KIND, 2, x0, y0 + 2 * row, w->Width - w->BorderRight - 8 - x0, NULL, 0,
       GTTX_Text, (ULONG)"Both empty: no password, so no FTP or telnet login.");
    PK(BUTTON_KIND, 3, x0, w->Height - w->BorderBottom - gh - 4, 100, "_OK", PLACETEXT_IN, GT_Underscore, '_');
    PK(BUTTON_KIND, 4, w->Width - w->BorderRight - 108, w->Height - w->BorderBottom - gh - 4, 100, "_Cancel",
       PLACETEXT_IN, GT_Underscore, '_');
#undef PK
    if (!g) goto out;
    AddGList(w, glist, ~0, -1, NULL);
    RefreshGList(glist, w, NULL, -1);
    GT_RefreshWindow(w, NULL);
    ActivateGadget(gad[0], w, NULL);
    while (!done) {
        struct IntuiMessage *im;
        WaitPort(w->UserPort);
        while (!done && (im = GT_GetIMsg(w->UserPort)) != NULL) {
            ULONG cls = im->Class;
            struct Gadget *gd = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); continue; }
            if (cls == IDCMP_CLOSEWINDOW) { done = TRUE; continue; }
            if (cls != IDCMP_GADGETUP) continue;
            if (gd->GadgetID == 0 && code != 9) { ActivateGadget(gad[1], w, NULL); continue; }
            if (gd->GadgetID == 4) { done = TRUE; continue; }
            if (gd->GadgetID == 3 || gd->GadgetID == 1) {
                struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)title, NULL, (STRPTR)"OK" };
                sec[0].s[sec[0].n] = sec[1].s[sec[1].n] = 0;
                if (strcmp(sec[0].s, sec[1].s)) {
                    es.es_TextFormat = (STRPTR)"The two passwords are not the same.\nType them again.";
                    EasyRequestArgs(w, &es, NULL, NULL);
                    for (k = 0; k < 2; k++) {
                        sec[k].n = 0;
                        GT_SetGadgetAttrs(gad[k], w, NULL, GTST_String, (ULONG)"", TAG_END);
                    }
                    ActivateGadget(gad[0], w, NULL);
                    continue;
                }
                if (!sec[0].n)
                    hash[0] = 0;
                else {
                    struct DateStamp ds;
                    ULONG r;
                    DateStamp(&ds);
                    r = (ULONG)ds.ds_Tick * 2654435761UL ^ (ULONG)ds.ds_Minute ^ (ULONG)FindTask(NULL)
                        ^ ((ULONG)VBeamPos() << 12);
                    pw_hash(sec[0].s, pw_b64[r & 63], pw_b64[(r >> 6) & 63], hash);
                }
                ok = done = TRUE;
            }
        }
    }
out:
    memset(sec, 0, sizeof(sec));
    if (w) CloseWindow(w);
    if (glist) FreeGadgets(glist);
    return ok;
}

static void route_label(const void *r, char *o)
{
    const struct PrefRoute *x = r;
    sprintf(o, "%s > %s", x->dest, x->gw);
}
static void user_label(const void *r, char *o)
{
    const struct PrefUser *x = r;
    sprintf(o, "%-10.10s %4s %.20s", x->name, x->uid, x->passwd[0] ? x->gecos : "(no password)");
}
static void group_label(const void *r, char *o)
{
    const struct PrefGroup *x = r;
    sprintf(o, "%-10.10s %4s %.30s", x->name, x->gid, x->members);
}
static const char *route_check(const void *r) { return check_route(r); }
static const char *user_check(const void *r)  { return check_user(r); }
static const char *group_check(const void *r) { return check_group(r); }

static int next_id(const void *rows, int n, int rowsize, int off, int first)
{
    int id = first, i;
    for (;;) {
        for (i = 0; i < n && atol((const char *)rows + i * rowsize + off) != id; i++) ;
        if (i == n) return id;
        id++;
    }
}
static void route_new(void *r, const void *rows, int n)
{
    struct PrefRoute *x = r;
    (void)rows; (void)n;
    strcpy(x->dest, "192.168.2.0/24");
    strcpy(x->gw, "192.168.1.254");
}
static void user_new(void *r, const void *rows, int n)
{
    struct PrefUser *x = r;
    if (n == 0) {                       /* the first account: this Amiga's user */
        if (GetVar((CONST_STRPTR)"USER", (STRPTR)x->name, sizeof(x->name), GVF_GLOBAL_ONLY) <= 0 || !x->name[0])
            strcpy(x->name, "amiga");
        strcpy(x->uid, "0");
        strcpy(x->gid, "0");
        copy(x->gecos, P.realname, sizeof(x->gecos));
    } else {
        strcpy(x->name, "user");
        sprintf(x->uid, "%d", next_id(rows, n, sizeof(struct PrefUser), offsetof(struct PrefUser, uid), 1000));
        strcpy(x->gid, "100");
    }
    strcpy(x->dir, "SYS:");
    strcpy(x->shell, "noshell");
}
static void group_new(void *r, const void *rows, int n)
{
    struct PrefGroup *x = r;
    if (n == 0) { strcpy(x->name, "wheel"); strcpy(x->gid, "0"); }
    else {
        strcpy(x->name, "users");
        sprintf(x->gid, "%d", next_id(rows, n, sizeof(struct PrefGroup), offsetof(struct PrefGroup, gid), 100));
    }
}

static const struct TCol route_cols[] = {
    { "Destination", offsetof(struct PrefRoute, dest), 20, 19 },
    { "Gateway",     offsetof(struct PrefRoute, gw),   16, 15 },
};
static const struct TCol user_cols[] = {
    { "Name",      offsetof(struct PrefUser, name),  32, 31 },
    { "UID",       offsetof(struct PrefUser, uid),   8,  6 },
    { "GID",       offsetof(struct PrefUser, gid),   8,  6 },
    { "Real name", offsetof(struct PrefUser, gecos), 64, 63 },
    { "Home",      offsetof(struct PrefUser, dir),   64, 63 },
    { "Shell",     offsetof(struct PrefUser, shell), 32, 31 },
};
static const struct TCol group_cols[] = {
    { "Name",    offsetof(struct PrefGroup, name),    32,  31 },
    { "GID",     offsetof(struct PrefGroup, gid),     8,   6 },
    { "Members", offsetof(struct PrefGroup, members), 128, 127 },
};
static const struct Table route_table = { "Static routes", route_cols, 2, sizeof(struct PrefRoute), MAXROUTES,
                                          route_label, route_check, route_new, -1 };
static const struct Table user_table  = { "Users", user_cols, 6, sizeof(struct PrefUser), MAXUSERS,
                                          user_label, user_check, user_new, offsetof(struct PrefUser, passwd) };
static const struct Table group_table = { "Groups", group_cols, 3, sizeof(struct PrefGroup), MAXGROUPS,
                                          group_label, group_check, group_new, -1 };

/*
 * Modal editor for one list. Works on a copy; OK (after every row passes
 * check) copies it back into rows and nrows. Returns TRUE if it did.
 */
static BOOL edit_table(struct Gui *parent, const struct Table *t, void *rows, int *nrows)
{
    struct Screen *scr = parent->scr;
    struct Window *w = NULL;
    struct Gadget *glist = NULL, *g, *gad[T_FIELD + T_MAXCOL];
    struct NewGadget ng;
    struct List list;
    struct Node *nodes = NULL;
    char (*labels)[64] = NULL;
    UBYTE *work = NULL;
    WORD fh = scr->Font->ta_YSize, gh = GH(fh), row = ROW(fh), lw, lab = 0, fx, y0, ww, wh, i;
    int n = *nrows, sel = n ? 0 : -1, k;
    BOOL done = FALSE, ok = FALSE;

    memset(gad, 0, sizeof(gad));
    work   = AllocVec(t->maxrows * t->rowsize, MEMF_CLEAR);
    nodes  = AllocVec(t->maxrows * sizeof(struct Node), MEMF_CLEAR);
    labels = AllocVec(t->maxrows * 64, MEMF_CLEAR);
    if (!work || !nodes || !labels) goto out;
    memcpy(work, rows, n * t->rowsize);

    for (k = 0; k < t->ncols; k++)
        if (tlen(parent, t->cols[k].label) > lab) lab = tlen(parent, t->cols[k].label);
    lab += 12;
    ww = 540; if (ww > scr->Width - 16) ww = scr->Width - 16;
    lw = ww * 9 / 20;
    {
        int frows = t->ncols < 4 ? 4 : t->ncols;
        if (t->passoff >= 0) frows++;               /* the Password... button */
        wh = scr->WBorTop + fh + 1 + 6 + frows * row + 8 + gh + 6 + scr->WBorBottom;
    }
    w = OpenWindowTags(NULL, WA_Title, (ULONG)t->title, WA_PubScreen, (ULONG)scr,
        WA_Left, parent->win->LeftEdge + 20, WA_Top, parent->win->TopEdge + 30,
        WA_Width, ww, WA_Height, wh,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | BUTTONIDCMP | STRINGIDCMP | LISTVIEWIDCMP,
        TAG_END);
    if (!w) goto out;

    list.lh_Head = (struct Node *)&list.lh_Tail; list.lh_Tail = NULL; list.lh_TailPred = (struct Node *)&list.lh_Head;
    if (!(g = CreateContext(&glist))) goto out;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = parent->vi;
    ng.ng_TextAttr = scr->Font;
    y0 = w->BorderTop + 6;
    fx = w->BorderLeft + 8 + lw + 12 + lab;
#define TK(kind, id, l, tp, wd, ht, txt, flags, ...) \
    ng.ng_LeftEdge = (l); ng.ng_TopEdge = (tp); ng.ng_Width = (wd); ng.ng_Height = (ht); \
    ng.ng_GadgetText = (UBYTE *)(txt); ng.ng_Flags = (flags); ng.ng_GadgetID = (id); \
    g = gad[id] = CreateGadget(kind, g, &ng, __VA_ARGS__, TAG_END)
    {
        int frows = t->ncols < 4 ? 4 : t->ncols;
        if (t->passoff >= 0) frows++;
        TK(LISTVIEW_KIND, T_LIST, w->BorderLeft + 8, y0, lw, (frows - 1) * row + 2, NULL, 0,
           GTLV_Labels, (ULONG)&list, GTLV_ShowSelected, 0UL);
        TK(BUTTON_KIND, T_ADD, w->BorderLeft + 8, y0 + (frows - 1) * row + 4, (lw - 4) / 2, gh, "Add", PLACETEXT_IN, GT_Underscore, 0);
        TK(BUTTON_KIND, T_REMOVE, w->BorderLeft + 8 + (lw + 4) / 2, y0 + (frows - 1) * row + 4, (lw - 4) / 2, gh,
           "Remove", PLACETEXT_IN, GT_Underscore, 0);
        for (k = 0; k < t->ncols; k++) {
            TK(STRING_KIND, T_FIELD + k, fx, y0 + k * row, w->Width - w->BorderRight - 8 - fx, gh,
               t->cols[k].label, PLACETEXT_LEFT, GTST_MaxChars, t->cols[k].maxch);
        }
        if (t->passoff >= 0)
            TK(BUTTON_KIND, T_PASS, fx, y0 + t->ncols * row, w->Width - w->BorderRight - 8 - fx, gh,
               "_Password...", PLACETEXT_IN, GT_Underscore, '_');
        i = w->Height - w->BorderBottom - gh - 4;
        TK(BUTTON_KIND, T_OK, w->BorderLeft + 8, i, 100, gh, "_OK", PLACETEXT_IN, GT_Underscore, '_');
        TK(BUTTON_KIND, T_CANCEL, w->Width - w->BorderRight - 108, i, 100, gh, "_Cancel", PLACETEXT_IN, GT_Underscore, '_');
    }
#undef TK
    if (!g) goto out;
    AddGList(w, glist, ~0, -1, NULL);
    RefreshGList(glist, w, NULL, -1);
    GT_RefreshWindow(w, NULL);

#define ROWP(i) (work + (i) * t->rowsize)
#define FIELD(i, c) ((char *)ROWP(i) + t->cols[c].off)
    for (;;) {
        /* (re)build the list labels and show the selected row */
        GT_SetGadgetAttrs(gad[T_LIST], w, NULL, GTLV_Labels, ~0UL, TAG_END);
        list.lh_Head = (struct Node *)&list.lh_Tail; list.lh_TailPred = (struct Node *)&list.lh_Head;
        for (k = 0; k < n; k++) {
            t->label(ROWP(k), labels[k]);
            nodes[k].ln_Name = labels[k];
            AddTail(&list, &nodes[k]);
        }
        GT_SetGadgetAttrs(gad[T_LIST], w, NULL, GTLV_Labels, (ULONG)&list,
                          GTLV_Selected, sel >= 0 ? (ULONG)sel : ~0UL, GTLV_MakeVisible, sel >= 0 ? (ULONG)sel : 0UL, TAG_END);
        for (k = 0; k < t->ncols; k++)
            GT_SetGadgetAttrs(gad[T_FIELD + k], w, NULL, GTST_String, (ULONG)(sel >= 0 ? FIELD(sel, k) : ""),
                              GA_Disabled, sel < 0, TAG_END);
        GT_SetGadgetAttrs(gad[T_REMOVE], w, NULL, GA_Disabled, sel < 0, TAG_END);
        if (gad[T_PASS]) GT_SetGadgetAttrs(gad[T_PASS], w, NULL, GA_Disabled, sel < 0, TAG_END);
        GT_SetGadgetAttrs(gad[T_ADD], w, NULL, GA_Disabled, n >= t->maxrows, TAG_END);

        /* wait for something that changes the list */
        for (;;) {
            struct IntuiMessage *im;
            BOOL changed = FALSE;
            WaitPort(w->UserPort);
            while ((im = GT_GetIMsg(w->UserPort)) != NULL) {
                ULONG cls = im->Class;
                UWORD code = im->Code;
                struct Gadget *gd = (struct Gadget *)im->IAddress;
                GT_ReplyIMsg(im);
                if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); continue; }
                if (cls == IDCMP_CLOSEWINDOW || (cls == IDCMP_VANILLAKEY && (code == 27 || (code | 0x20) == 'c'))) {
                    done = TRUE; changed = TRUE; continue;
                }
                if (cls == IDCMP_VANILLAKEY && (code | 0x20) == 'o') code = T_OK, cls = IDCMP_GADGETUP, gd = gad[T_OK];
                if (cls == IDCMP_VANILLAKEY && (code | 0x20) == 'p' && gad[T_PASS] && sel >= 0)
                    code = T_PASS, cls = IDCMP_GADGETUP, gd = gad[T_PASS];
                if (cls != IDCMP_GADGETUP) continue;
                /* every string field writes straight into the selected row */
                if (sel >= 0)
                    for (k = 0; k < t->ncols; k++)
                        copy(FIELD(sel, k), (const char *)((struct StringInfo *)gad[T_FIELD + k]->SpecialInfo)->Buffer,
                             t->cols[k].size);
                switch (gd->GadgetID) {
                case T_LIST:
                    sel = code; changed = TRUE; break;
                case T_ADD:
                    if (n < t->maxrows) {
                        memset(ROWP(n), 0, t->rowsize);
                        t->newrow(ROWP(n), work, n);
                        sel = n++;
                        changed = TRUE;
                    }
                    break;
                case T_REMOVE:
                    if (sel >= 0) {
                        memmove(ROWP(sel), ROWP(sel + 1), (n - sel - 1) * t->rowsize);
                        n--;
                        if (sel >= n) sel = n - 1;
                        changed = TRUE;
                    }
                    break;
                case T_OK: {
                    const char *err = NULL;
                    for (k = 0; k < n && !err; k++)
                        if ((err = t->check(ROWP(k))) != NULL) sel = k;
                    if (err) {
                        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)t->title, (STRPTR)err, (STRPTR)"OK" };
                        EasyRequestArgs(w, &es, NULL, NULL);
                    } else {
                        memcpy(rows, work, n * t->rowsize);
                        *nrows = n;
                        ok = TRUE;
                        done = TRUE;
                    }
                    changed = TRUE;
                    break;
                }
                case T_CANCEL:
                    done = TRUE; changed = TRUE; break;
                case T_PASS: {
                    char h[16];
                    if (sel >= 0 && ask_new_password(parent, w, FIELD(sel, 0), h))
                        strcpy((char *)ROWP(sel) + t->passoff, h);
                    changed = TRUE;
                    break;
                }
                default:            /* a field: refresh its list line */
                    changed = TRUE; break;
                }
            }
            if (changed) break;
        }
        if (done) break;
    }
#undef FIELD
#undef ROWP

out:
    if (w) CloseWindow(w);
    if (glist) FreeGadgets(glist);
    FreeVec(labels);
    FreeVec(nodes);
    FreeVec(work);
    return ok;
}

static BOOL core_running(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((CONST_STRPTR)PN_PORTNAME);
    Permit();
    return p != NULL;
}

static void restart_core(void)
{
    struct MsgPort *p;
    BPTR in, out;
    int i;

    Forbid();
    if ((p = FindPort((CONST_STRPTR)PN_PORTNAME)) != NULL)
        Signal(p->mp_SigTask, SIGBREAKF_CTRL_C);
    Permit();
    for (i = 0; i < 100 && core_running(); i++)
        Delay(5);
    in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((CONST_STRPTR)"C:PrismNet START", SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
    }
}

/* Save (arc=TRUE) or Use. Returns TRUE when the window should close. */
static BOOL commit(struct Gui *gui, BOOL arc)
{
    const char *err;
    int bad;

    store_if(gui);
    store_globals(gui);
    store_share(gui);
    store_services(gui);
    if ((err = validate(&P, &bad)) != NULL) {
        if (bad >= 0) { gui->sel = bad; show_all(gui); }
        ask(gui, "PrismNet Prefs", err, "OK");
        return FALSE;
    }
    if ((err = check_services(&P)) != NULL) {
        gui->page = gui->tabs.active = PG_SERVICE;
        relayout(gui);
        tabs_draw(&gui->tabs, gui->win);
        ask(gui, "PrismNet Prefs", err, "OK");
        return FALSE;
    }
    if ((err = check_shares(&P, &bad)) != NULL) {
        if (bad >= 0) { gui->shsel = bad; show_all(gui); }
        ask(gui, "PrismNet Prefs", err, "OK");
        return FALSE;
    }
    if (!save_prefs(&P, ENV_DIR) || (arc && !save_prefs(&P, ARC_DIR))) {
        char m[200];
        sprintf(m, "Could not write the settings:\n%s", save_err);
        ask(gui, "PrismNet Prefs", m, "OK");
        return FALSE;
    }
    /* the shares are DOSDriver files (no ENV: copy): written by Use and Save alike */
    if (!save_shares(&P)) {
        ask(gui, "PrismNet Prefs", "Could not write a share's mountlist file\nin DEVS:DOSDrivers or SYS:Storage/DOSDrivers.", "OK");
        return FALSE;
    }
    if (P.user[0])
        SetVar((CONST_STRPTR)"USER", (STRPTR)P.user, -1, GVF_GLOBAL_ONLY | (arc ? GVF_SAVE_VAR : 0));
    else
        DeleteVar((CONST_STRPTR)"USER", GVF_GLOBAL_ONLY | (arc ? GVF_SAVE_VAR : 0));
    if (arc) {
        if (exists(ENV_DIR "/hosts") && !P.import_hosts[0])
            copy_file(ENV_DIR "/hosts", ARC_DIR "/hosts");
        if (P.atboot != boot_entry_present() && !set_boot_entry(P.atboot))
            ask(gui, "PrismNet Prefs", "Could not update S:User-Startup.", "OK");
    }
    if (core_running() &&
        ask(gui, "PrismNet Prefs", "PrismNet is running.\nRestart it now to use the new settings?",
            "Restart|Later") == 1)
        restart_core();
    return TRUE;
}

static int run_gui(void)
{
    struct Gui gui;
    BOOL done = FALSE;
    WORD fh, w, h;

    memset(&gui, 0, sizeof(gui));
    gui.sel = P.nifs ? 0 : -1;
    gui.shsel = P.nshares ? 0 : -1;
    gui.shlist.lh_Head = (struct Node *)&gui.shlist.lh_Tail;
    gui.shlist.lh_TailPred = (struct Node *)&gui.shlist.lh_Head;
    gui.iflist.lh_Head = (struct Node *)&gui.iflist.lh_Tail;
    gui.iflist.lh_TailPred = (struct Node *)&gui.iflist.lh_Head;

    if (!(gui.scr = LockPubScreen(NULL)) || !(gui.vi = GetVisualInfo(gui.scr, TAG_END)))
        goto out;
    fh = gui.scr->Font->ta_YSize;
    w = 600; if (w > gui.scr->Width) w = gui.scr->Width;
    h = gui.scr->WBorTop + fh + 1 + layout_height(fh) + gui.scr->WBorBottom;
    {
        WORD maxh = gui.scr->Height - gui.scr->BarHeight - 2;
        char v[12];
        /* PrismNet/prefsmaxheight: pretend the screen is that small (testing) */
        if (GetVar((CONST_STRPTR)"PrismNet/prefsmaxheight", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0 &&
            atol(v) >= 100 && atol(v) < maxh)
            maxh = (WORD)atol(v);
        if (h > maxh) { h = maxh; gui.scrolling = TRUE; }
        /* PrismNet/prefspage: open on that tab, 0-4 (testing) */
        if (GetVar((CONST_STRPTR)"PrismNet/prefspage", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0 &&
            atol(v) >= 0 && atol(v) < PG_COUNT)
            gui.page = gui.tabs.active = (int)atol(v);
    }
    gui.win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"PrismNet Prefs", WA_ScreenTitle, (ULONG)"PrismNet Prefs - network settings",
        WA_Left, (gui.scr->Width - w) / 2, WA_Top, gui.scr->BarHeight + 2, WA_InnerWidth, w - 16,
        WA_Height, h,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MENUPICK | IDCMP_VANILLAKEY |
                  BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP | CHECKBOXIDCMP | LISTVIEWIDCMP | SCROLLERIDCMP,
        WA_PubScreen, (ULONG)gui.scr, TAG_END);
    if (!gui.win)
        goto out;
    /* the tab row under the title bar; the pages below it */
    SetFont(gui.win->RPort, gui.scr->RastPort.Font);
    tabs_init(&gui.tabs, gui.win->RPort, page_labels, PG_COUNT, G_TAB0, gui.win->BorderLeft + 4,
              gui.win->BorderTop + 3, gui.win->Width - gui.win->BorderLeft - gui.win->BorderRight - 8);
    tabs_add(&gui.tabs, gui.win);
    gui.view_top = gui.tabs.y + gui.tabs.h + 4;
    gui.view_bot = gui.win->Height - gui.win->BorderBottom - GH(fh) - 8;
    if (gui.scrolling) {
        WORD content = layout_height(fh) - (6 + GH(fh) + 5) - TABS_H(fh) - 3;
        gui.view_bot = gui.win->Height - gui.win->BorderBottom - GH(fh) - 8;
        gui.unit = ROW(fh) / 2;
        gui.total_units = (content + gui.unit - 1) / gui.unit;
        gui.view_units = (gui.view_bot - gui.view_top) / gui.unit;
        if (gui.view_units >= gui.total_units) gui.scrolling = FALSE;
    }
    if (!make_base(&gui) || !make_gadgets(&gui))
        goto out;
    AddGList(gui.win, gui.bglist, ~0, -1, NULL);
    AddGList(gui.win, gui.glist, ~0, -1, NULL);
    RefreshGList(gui.bglist, gui.win, NULL, -1);
    RefreshGList(gui.glist, gui.win, NULL, -1);
    GT_RefreshWindow(gui.win, NULL);
    tabs_draw(&gui.tabs, gui.win);
    draw_frames(&gui);
    show_all(&gui);
    if ((gui.menu = CreateMenus(newmenu, TAG_END)) != NULL) {
        if (LayoutMenus(gui.menu, gui.vi, TAG_END)) SetMenuStrip(gui.win, gui.menu);
        else { FreeMenus(gui.menu); gui.menu = NULL; }
    }

    while (!done) {
        struct IntuiMessage *im;
        LONG newtop = -1;               /* scroller moved: rebuild once the queue is empty */
        int newpage = -1;               /* tab clicked: same, the old page's gadgets go away */
        WaitPort(gui.win->UserPort);
        while (!done && (im = GT_GetIMsg(gui.win->UserPort)) != NULL) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);

            /* the scroller's arrows are separate gadgets with the same ID */
            if ((cls == IDCMP_MOUSEMOVE || cls == IDCMP_GADGETUP || cls == IDCMP_GADGETDOWN) &&
                gui.scrolling && gad && gad->GadgetID == G_SCROLL) {
                newtop = code;
                continue;
            }
            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                done = TRUE;
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(gui.win);
                tabs_draw(&gui.tabs, gui.win);
                draw_frames(&gui);
                GT_EndRefresh(gui.win, TRUE);
                break;
            case IDCMP_VANILLAKEY:
                if ((code | 0x20) == 's') done = commit(&gui, TRUE);
                else if ((code | 0x20) == 'u') done = commit(&gui, FALSE);
                else if ((code | 0x20) == 'c' || code == 27) done = TRUE;
                break;
            case IDCMP_GADGETUP:
#ifdef PREFS_DEBUG_STALE
                {   /* test build: is this gadget still in the window? */
                    struct Gadget *g;
                    for (g = gui.win->FirstGadget; g && g != gad; g = g->NextGadget) ;
                    if (!g) {
                        BPTR f = Open("T:prefs-stale.log", MODE_READWRITE);
                        if (f) { Seek(f, 0, OFFSET_END); FPrintf(f, "stale GADGETUP %lx\n", (ULONG)gad); Close(f); }
                    }
                }
#endif
                if (tabs_hit(&gui.tabs, gad->GadgetID) >= 0) {
                    int pg = tabs_hit(&gui.tabs, gad->GadgetID);
                    if (pg != gui.page)
                        newpage = pg;
                    break;
                }
                switch (gad->GadgetID) {
                case G_IFLIST:
                    store_if(&gui);
                    gui.sel = code;
                    show_if(&gui);
                    break;
                case G_ADD:
                    if (P.nifs < MAXIF) {
                        struct PrefIf *pi = &P.ifs[P.nifs];
                        int n = 0, i;
                        store_if(&gui);
                        memset(pi, 0, sizeof(*pi));
                        for (;;) {          /* first free ethN */
                            sprintf(pi->name, "eth%d", n);
                            for (i = 0; i < P.nifs && !ieq(P.ifs[i].name, pi->name); i++) ;
                            if (i == P.nifs) break;
                            n++;
                        }
                        strcpy(pi->device, "cnet.device");
                        strcpy(pi->unit, "0");
                        pi->dhcp = TRUE;
                        gui.sel = P.nifs++;
                        show_all(&gui);
                        ActivateGadget(gui.g[G_DEVICE], gui.win, NULL);
                    }
                    break;
                case G_REMOVE:
                    if (gui.sel >= 0 && gui.sel < P.nifs) {
                        int i;
                        for (i = gui.sel; i < P.nifs - 1; i++) P.ifs[i] = P.ifs[i + 1];
                        P.nifs--;
                        if (gui.sel >= P.nifs) gui.sel = P.nifs - 1;
                        show_all(&gui);
                    }
                    break;
                case G_NAME:
                    store_if(&gui);
                    build_iflist(&gui);
                    break;
                case G_MODE:
                    if (gui.sel >= 0) {
                        store_if(&gui);
                        P.ifs[gui.sel].dhcp = (code == 0);
                        show_if(&gui);
                        if (code == 1) ActivateGadget(gui.g[G_ADDR], gui.win, NULL);
                    }
                    break;
                case G_PICK:   pick_device(&gui); break;
                case G_DEVICE: case G_UNIT: case G_ADDR: case G_MASK: case G_GW: case G_MTU:
                case G_BAUD:
                    store_if(&gui);
                    break;
                case G_RTSCTS:
                    if (gui.sel >= 0 && gui.sel < P.nifs)
                        P.ifs[gui.sel].rtscts = (gui.g[G_RTSCTS]->Flags & GFLG_SELECTED) != 0;
                    break;
                case G_MEMORY:
                    store_globals(&gui);
                    break;
                case G_AUTO:   P.autostart = (gui.g[G_AUTO]->Flags & GFLG_SELECTED) != 0; break;
                case G_BOOT:   P.atboot = (gui.g[G_BOOT]->Flags & GFLG_SELECTED) != 0; break;
                case G_HOSTS:  edit_hosts(&gui); break;
                case G_ROUTES:
                    edit_table(&gui, &route_table, P.routes, &P.nroutes);
                    break;
                case G_ACCOUNTS:
                    store_globals(&gui);        /* the first user takes the real name */
                    switch (ask(&gui, "Accounts", "Edit the users or the groups that\nusergroup.library reports?",
                                "Users|Groups|Cancel")) {
                    case 1: edit_table(&gui, &user_table, P.users, &P.nusers); break;
                    case 2: edit_table(&gui, &group_table, P.groups, &P.ngroups); break;
                    }
                    break;
                case G_SHLIST:
                    store_share(&gui);
                    gui.shsel = code;
                    show_share(&gui);
                    break;
                case G_SHADD:    add_share(&gui); break;
                case G_SHREMOVE: remove_share(&gui); break;
                case G_SHNAME:
                    store_share(&gui);
                    build_shlist(&gui);
                    break;
                case G_SHTYPE:
                    if (gui.shsel >= 0 && gui.shsel < P.nshares) {
                        store_share(&gui);
                        P.shares[gui.shsel].smb = (code == 1);
                        P.shares[gui.shsel].handler = sh_default_handler(code == 1);
                        build_shlist(&gui);
                        show_share(&gui);
                    }
                    break;
                case G_SHHOST: case G_SHSHARE: case G_SHVOL: case G_SHUSER: case G_SHUID: case G_SHGID:
                    store_share(&gui);
                    break;
                case G_SHBOOT:
                    if (gui.shsel >= 0 && gui.shsel < P.nshares)
                        P.shares[gui.shsel].atboot = (gui.g[G_SHBOOT]->Flags & GFLG_SELECTED) != 0;
                    break;
                case G_SHRO:
                    if (gui.shsel >= 0 && gui.shsel < P.nshares)
                        P.shares[gui.shsel].readonly = (gui.g[G_SHRO]->Flags & GFLG_SELECTED) != 0;
                    break;
                case G_SHBROWSE:  browse_shares(&gui); break;
                case G_FTPON:   P.ftp_on = (gui.g[G_FTPON]->Flags & GFLG_SELECTED) != 0; break;
                case G_FTPRO:   P.ftp_ro = (gui.g[G_FTPRO]->Flags & GFLG_SELECTED) != 0; break;
                case G_FTPANON: P.ftp_anon = (gui.g[G_FTPANON]->Flags & GFLG_SELECTED) != 0; break;
                case G_FTPPICK: pick_anon_dir(&gui); break;
                case G_FTPPORT: case G_FTPDIR: case G_FTPPASV: case G_TELPORT: store_services(&gui); break;
                case G_TELON:   P.tel_on = (gui.g[G_TELON]->Flags & GFLG_SELECTED) != 0; break;
                case G_SHMOUNT:   mount_share(&gui, TRUE); break;
                case G_SHUNMOUNT: mount_share(&gui, FALSE); break;
                case G_SAVE:   done = commit(&gui, TRUE); break;
                case G_USE:    done = commit(&gui, FALSE); break;
                case G_CANCEL: done = TRUE; break;
                }
                break;
            case IDCMP_MENUPICK:
                while (code != MENUNULL && !done) {
                    struct MenuItem *it = ItemAddress(gui.menu, code);
                    ULONG id;
                    if (!it) break;
                    id = (ULONG)GTMENUITEM_USERDATA(it);
                    if (id == M_QUIT) done = TRUE;
                    else if (id == M_DEFAULTS) { defaults(&P); P.atboot = boot_entry_present(); gui.sel = -1; show_all(&gui); }
                    else if (id == M_LASTSAVED) { load_prefs(&P, ARC_DIR); gui.sel = -1; show_all(&gui); }
                    else if (id == M_IMPORT) {
                        char summary[160];
                        store_if(&gui);
                        store_globals(&gui);
                        if (import_roadshow(&P, "DEVS:", summary)) {
                            gui.sel = -1;
                            show_all(&gui);
                            ask(&gui, "Import Roadshow settings", summary, "OK");
                        } else {
                            ask(&gui, "Import Roadshow settings",
                                "No Roadshow settings found\n(DEVS:NetInterfaces, DEVS:Internet).", "OK");
                        }
                    }
                    code = it->NextSelect;
                }
                break;
            }
        }
        if (!done && newpage >= 0 && newpage != gui.page) {
            gui.page = newpage;
            gui.tabs.active = newpage;
            gui.scroll = 0;
            relayout(&gui);
            tabs_draw(&gui.tabs, gui.win);
        } else if (!done && gui.scrolling && newtop >= 0 && newtop * gui.unit != gui.scroll) {
            gui.scroll = (WORD)(newtop * gui.unit);
            relayout(&gui);
        }
    }

out:
    if (gui.win) {
        if (gui.menu) { ClearMenuStrip(gui.win); FreeMenus(gui.menu); }
        /* AddGList chained glist onto the end of bglist: take both off the
         * window (glist first, it is last) so each list ends on its own and
         * FreeGadgets(bglist) cannot walk into glist. */
        if (gui.glist) RemoveGList(gui.win, gui.glist, -1);
        if (gui.bglist) RemoveGList(gui.win, gui.bglist, -1);
        tabs_remove(&gui.tabs, gui.win);
        CloseWindow(gui.win);
    }
    if (gui.glist) FreeGadgets(gui.glist);
    if (gui.bglist) FreeGadgets(gui.bglist);
    if (gui.vi) FreeVisualInfo(gui.vi);
    if (gui.scr) UnlockPubScreen(NULL, gui.scr);
    return 0;
}

int main(int argc, char **argv)
{
    LONG args[4] = { 0 };
    struct RDArgs *rda = NULL;
    int rc = RETURN_OK;

    (void)verstag; (void)argv;
    if (argc > 0) {                         /* CLI (argc == 0: started from Workbench) */
        rda = ReadArgs((CONST_STRPTR)"FROM/K,IMPORT/K,TO/K,BOOT/K", args, NULL);
        if (!rda) {                         /* a script's typo must not open a window */
            PrintFault(IoErr(), (CONST_STRPTR)"PrismNetPrefs");
            return RETURN_ERROR;
        }
    }

    if (args[3]) {                          /* BOOT=YES|NO: just the S:User-Startup block */
        BOOL on = ieq((const char *)args[3], "YES") || ieq((const char *)args[3], "ON");
        if (set_boot_entry(on))
            printf("PrismNet %s S:User-Startup\n", on ? "added to" : "removed from");
        else
            rc = RETURN_FAIL;
        if (rda) FreeArgs(rda);
        return rc;
    }
    if (args[0])
        load_prefs(&P, (const char *)args[0]);
    else
        load_prefs(&P, exists(ENV_DIR) ? ENV_DIR : ARC_DIR);

    if (args[2]) {                          /* no window: convert and write */
        const char *err;
        int bad;
        char summary[160];
        if (args[1]) {
            if (import_roadshow(&P, (const char *)args[1], summary))
                printf("%s\n", summary);
            else
                printf("No Roadshow settings under %s\n", (const char *)args[1]);
        }
        if ((err = validate(&P, &bad)) != NULL) {
            printf("Not saved: %s\n", err);
            rc = RETURN_ERROR;
        } else if (!save_prefs(&P, (const char *)args[2])) {
            printf("Could not write %s\n", (const char *)args[2]);
            rc = RETURN_FAIL;
        } else {
            printf("Saved %d interface%s to %s\n", P.nifs, P.nifs == 1 ? "" : "s", (const char *)args[2]);
        }
        if (rda) FreeArgs(rda);
        return rc;
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    GadToolsBase  = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
    AslBase       = OpenLibrary((CONST_STRPTR)"asl.library", 37);
    LayersBase    = OpenLibrary((CONST_STRPTR)"layers.library", 37);
    if (IntuitionBase && GfxBase && GadToolsBase)
        run_gui();
    else
        rc = RETURN_FAIL;
    if (LayersBase) CloseLibrary(LayersBase);
    if (AslBase) CloseLibrary(AslBase);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (rda) FreeArgs(rda);
    return rc;
}
