/*
 * PrismNet Prefs, Shares tab: the model and the DOSDriver files. Included
 * by prismprefs.c (same translation unit: it uses its file helpers).
 *
 * The DOSDriver files are the only copy of a share's settings: Prefs lists
 * every file in DEVS:DOSDrivers (mounted at boot) and SYS:Storage/DOSDrivers
 * (mounted on demand) whose Handler is one of the share handlers, and
 * writes them back in the same form. Unknown Startup options are kept.
 * GPL-3.0-only, see LICENSE.
 */

/* 0 when the first n characters match, ignoring case (libnix has no strnicmp) */
static int ieqn(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

#define SH_DEVS    "DEVS:DOSDrivers"
#define SH_STORAGE "SYS:Storage/DOSDrivers"

/* handler index: PrismNet's own two first, then the Aminet ones */
static const char *const sh_handlers[] = {
    "L:prismnfs-handler", "L:prismsmb-handler", "L:nfs-handler", "L:smb2-handler", NULL
};
#define SH_IS_SMB(h)  ((h) == 1 || (h) == 3)
#define SH_IS_PRISM(h) ((h) <= 1)

static const char *sh_dir(const struct PrefShare *s) { return s->atboot ? SH_DEVS : SH_STORAGE; }

/* the handler a new share of this type gets: PrismNet's if installed */
static int sh_default_handler(int smb)
{
    if (smb) return exists("L:prismsmb-handler") || !exists("L:smb2-handler") ? 1 : 3;
    return exists("L:prismnfs-handler") || !exists("L:nfs-handler") ? 0 : 2;
}

static void sh_append(char *extra, int size, const char *tok)
{
    int n = strlen(extra);
    if (n + 1 + (int)strlen(tok) >= size) return;
    if (n) extra[n++] = ' ';
    strcpy(extra + n, tok);
}

/* "Key = value" line value (trimmed, quotes removed) */
static BOOL sh_keyval(char *line, const char *key, char *out, int size)
{
    char *p = line, *e;
    while (*p == ' ' || *p == '\t') p++;
    if (ieqn(p, key, strlen(key))) return FALSE;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return FALSE;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { p++; e = strchr(p, '"'); }
    else { e = p; while (*e && *e != '\n' && *e != '\r') e++; }
    if (!e) e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e - p >= size) e = p + size - 1;
    memcpy(out, p, e - p);
    out[e - p] = 0;
    return TRUE;
}

/* parse one DOSDriver file; FALSE if it is not a share */
static BOOL sh_parse(struct PrefShare *s, const char *name, char *text, int atboot)
{
    char line[300], handler[80] = "", startup[400] = "", val[80], *p, *q, *tok;
    int h;

    memset(s, 0, sizeof(*s));
    copy(s->name, name, sizeof(s->name));
    strcpy(s->stack, "16384");
    strcpy(s->pri, "5");
    s->atboot = s->indevs = atboot;
    for (p = text; *p; ) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;
        if (sh_keyval(line, "Handler", val, sizeof(val))) copy(handler, val, sizeof(handler));
        else if (sh_keyval(line, "Startup", val, sizeof(val))) { /* keep the long form */
            char *st = strchr(strstr(line, "Startup") ? strstr(line, "Startup") : line, '"');
            if (st) {
                char *end = strrchr(st + 1, '"');
                int l = end ? (int)(end - st - 1) : (int)strlen(st + 1);
                if (l >= (int)sizeof(startup)) l = sizeof(startup) - 1;
                memcpy(startup, st + 1, l); startup[l] = 0;
            }
        }
        else if (sh_keyval(line, "StackSize", val, sizeof(val))) copy(s->stack, val, sizeof(s->stack));
        else if (sh_keyval(line, "Priority", val, sizeof(val))) copy(s->pri, val, sizeof(s->pri));
    }
    for (h = 0; sh_handlers[h] && !ieq(sh_handlers[h], handler); h++) ;
    if (!sh_handlers[h] || !startup[0]) return FALSE;
    s->handler = h;
    s->smb = SH_IS_SMB(h);
    /* the target: host:/export, host/share, smb://host/share */
    q = startup;
    while (*q == ' ') q++;
    tok = q;
    while (*q && *q != ' ') q++;
    if (*q) *q++ = 0;
    if (!ieqn(tok, "smb://", 6)) tok += 6;
    else if (!ieqn(tok, "//", 2)) tok += 2;
    {
        char *sep = s->smb ? strchr(tok, '/') : strchr(tok, ':');
        if (!sep) return FALSE;
        *sep++ = 0;
        copy(s->host, tok, sizeof(s->host));
        copy(s->share, sep, sizeof(s->share));
    }
    /* the options */
    while (token(&q, line, sizeof(line))) {
        char *v = strchr(line, '=');
        if (v && (*v = 0, ieq(line, "VOLUME")))        copy(s->volume, v + 1, sizeof(s->volume));
        else if (v && ieq(line, "USER"))               copy(s->user, v + 1, sizeof(s->user));
        else if (v && ieq(line, "UID"))                copy(s->uid, v + 1, sizeof(s->uid));
        else if (v && ieq(line, "GID"))                copy(s->gid, v + 1, sizeof(s->gid));
        else if (!v && ieq(line, "READONLY"))          s->readonly = TRUE;
        else { if (v) *v = '='; sh_append(s->extra, sizeof(s->extra), line); }
    }
    return TRUE;
}

static void sh_scan(struct Prefs *p, const char *dir, int atboot)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    if (!(lock = Lock((CONST_STRPTR)dir, ACCESS_READ)))
        return;
    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (Examine(lock, fib))
            while (ExNext(lock, fib) && p->nshares < MAXSHARES) {
                const char *fn = fib->fib_FileName;
                size_t l = strlen(fn);
                char path[160], *text;
                if (fib->fib_DirEntryType > 0 || l >= 16 || (l > 5 && ieq(fn + l - 5, ".info")))
                    continue;
                sprintf(path, "%s/%s", dir, fn);
                if ((text = load_file(path)) != NULL) {
                    if (sh_parse(&p->shares[p->nshares], fn, text, atboot)) {
                        copy(p->sh_loaded[p->nloaded], fn, 16);
                        p->sh_loaded_devs[p->nloaded++] = atboot;
                        p->nshares++;
                    }
                    FreeVec(text);
                }
            }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
}

static void load_shares(struct Prefs *p)
{
    p->nshares = p->nloaded = 0;
    sh_scan(p, SH_DEVS, 1);
    sh_scan(p, SH_STORAGE, 0);
}

static void sh_startup(const struct PrefShare *s, char *out, int size)
{
    char t[400];
    int h = s->handler;

    if (s->smb) sprintf(t, "%s%s/%s", h == 3 ? "smb://" : "", s->host, s->share);
    else sprintf(t, "%s:%s", s->host, s->share);
    if (s->volume[0]) { strcat(t, " VOLUME="); strcat(t, s->volume); }
    if (s->smb && s->user[0]) { strcat(t, " USER="); strcat(t, s->user); }
    if (!s->smb && s->uid[0]) { strcat(t, " UID="); strcat(t, s->uid); }
    if (!s->smb && s->gid[0]) { strcat(t, " GID="); strcat(t, s->gid); }
    if (s->readonly && (SH_IS_PRISM(h) || h == 3)) strcat(t, " READONLY");
    if (s->extra[0]) { strcat(t, " "); strcat(t, s->extra); }
    copy(out, t, size);
}

/* a project icon for a new file (double-click mounts): copy any DOSDriver
 * icon from the same drawer, best effort */
static void sh_icon(const char *dir, const char *name)
{
    static const char *const models[] = { "PC0", "RAD", "CD0", "AUX", NULL };
    char from[160], to[160];
    int i;

    sprintf(to, "%s/%s.info", dir, name);
    if (exists(to)) return;
    for (i = 0; models[i]; i++) {
        BPTR in, out;
        sprintf(from, SH_STORAGE "/%s.info", models[i]);
        if (!(in = Open((CONST_STRPTR)from, MODE_OLDFILE))) continue;
        if ((out = Open((CONST_STRPTR)to, MODE_NEWFILE)) != 0) {   /* binary: copy_file() is for text */
            static UBYTE buf[1024];
            LONG n;
            while ((n = Read(in, buf, sizeof(buf))) > 0)
                if (Write(out, buf, n) != n) break;
            Close(out);
        }
        Close(in);
        return;
    }
}

/* write every share's DOSDriver; files of shares that were loaded and are
 * gone (removed, renamed, moved to the other drawer) are deleted */
static BOOL save_shares(const struct Prefs *p)
{
    char path[160], text[700], startup[400];
    int i, j;
    BOOL ok = TRUE;

    for (i = 0; i < p->nloaded; i++) {
        for (j = 0; j < p->nshares; j++)
            if (ieq(p->shares[j].name, p->sh_loaded[i]) && p->shares[j].atboot == p->sh_loaded_devs[i])
                break;
        if (j == p->nshares) {
            sprintf(path, "%s/%s", p->sh_loaded_devs[i] ? SH_DEVS : SH_STORAGE, p->sh_loaded[i]);
            DeleteFile((CONST_STRPTR)path);
            strcat(path, ".info");
            DeleteFile((CONST_STRPTR)path);
        }
    }
    for (i = 0; i < p->nshares; i++) {
        const struct PrefShare *s = &p->shares[i];
        sh_startup(s, startup, sizeof(startup));
        sprintf(text, "/* PrismNet share: %s %s (PrismNet Prefs) */\n"
                      "Handler    = %s\nStackSize  = %s\nPriority   = %s\nGlobVec    = -1\n"
                      "Startup    = \"%s\"\n",
                s->smb ? "SMB" : "NFS", s->host, sh_handlers[s->handler], s->stack, s->pri, startup);
        make_dir(sh_dir(s));
        sprintf(path, "%s/%s", sh_dir(s), s->name);
        if (!save_file(path, text)) ok = FALSE;
        else sh_icon(sh_dir(s), s->name);
    }
    return ok;
}

static const char *check_shares(const struct Prefs *p, int *bad)
{
    static char msg[120];
    int i, j;

    for (i = 0; i < p->nshares; i++) {
        const struct PrefShare *s = &p->shares[i];
        *bad = i;
        if (!s->name[0] || strpbrk(s->name, ":/ "))
            { sprintf(msg, "Share %d: the device name must be set\nand contain no ':', '/' or spaces.", i + 1); return msg; }
        for (j = 0; j < i; j++)
            if (ieq(p->shares[j].name, s->name)) { sprintf(msg, "Two shares are called \"%s\".", s->name); return msg; }
        if (!s->host[0])  { sprintf(msg, "%s: no server set.", s->name); return msg; }
        if (!s->share[0]) { sprintf(msg, "%s: no %s set.", s->name, s->smb ? "share" : "export path"); return msg; }
        if (strchr(s->host, ' ') || strchr(s->share, '"'))
            { sprintf(msg, "%s: the server or share name has a\ncharacter the mountlist cannot hold.", s->name); return msg; }
        if ((s->uid[0] && !all_digits(s->uid)) || (s->gid[0] && !all_digits(s->gid)))
            { sprintf(msg, "%s: UID and GID must be numbers.", s->name); return msg; }
    }
    *bad = -1;
    return NULL;
}
