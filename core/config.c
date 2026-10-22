/*
 * PrismNet core: configuration from ENV:PrismNet/ (see config.h).
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "lwip/ip4_addr.h"

#include "config.h"

BOOL pn_parse_ip(const char *s, ULONG *out)
{
    ip4_addr_t a;
    if (!s || !ip4addr_aton(s, &a))
        return FALSE;
    *out = ip4_addr_get_u32(&a);
    return TRUE;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

static char *load(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    char *buf = NULL;
    LONG n;

    if (!fh)
        return NULL;
    if ((buf = AllocVec(16384, MEMF_ANY)) != NULL) {
        n = Read(fh, buf, 16383);
        buf[n > 0 ? n : 0] = 0;
    }
    Close(fh);
    return buf;
}

/* Next whitespace-separated token; '#' comments; "quoted strings". */
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
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (n < size - 1) out[n++] = *p; p++; }
            if (*p) p++;
        } else {
            if (n < size - 1) out[n++] = *p;
            p++;
        }
    }
    out[n] = 0;
    *pp = p;
    return TRUE;
}

static void warnf(void (*warn)(const char *), const char *fmt, const char *a, const char *b)
{
    char msg[160];
    if (!warn) return;
    snprintf(msg, sizeof(msg), fmt, a, b);
    warn(msg);
}

/* 1 = loaded, 0 = bad (err says why), -1 = empty file: the interface is
 * switched off (Prefs writes an empty file where it cannot delete one, on
 * the lazy ENV: of OS 3.2). */
static int load_iface(struct PnIfConfig *ic, const char *name, const char *path,
                      char *err, int errsize, void (*warn)(const char *))
{
    char *data = load(path), *p, tok[128];
    int ntok = 0;

    memset(ic, 0, sizeof(*ic));
    strncpy(ic->name, name, sizeof(ic->name) - 1);
    ic->dhcp = TRUE;
    if (!data) {
        snprintf(err, errsize, "cannot read %s", path);
        return 0;
    }
    for (p = data; token(&p, tok, sizeof(tok)); ) {
        char *v = strchr(tok, '=');
        ntok++;
        if (!v) { warnf(warn, "%s: ignoring '%s'", name, tok); continue; }
        *v++ = 0;
        if (ieq(tok, "DEVICE"))        strncpy(ic->device, v, sizeof(ic->device) - 1);
        else if (ieq(tok, "UNIT"))     ic->unit = strtoul(v, NULL, 10);
        else if (ieq(tok, "MTU"))      ic->mtu = strtoul(v, NULL, 10);
        else if (ieq(tok, "STALL"))    ic->stall = strtoul(v, NULL, 10);
        else if (ieq(tok, "SLIP"))     ic->slip = ieq(v, "YES") || ieq(v, "ON") || ieq(v, "1");
        else if (ieq(tok, "BAUD"))     ic->baud = strtoul(v, NULL, 10);
        else if (ieq(tok, "RTSCTS"))   ic->rtscts = ieq(v, "YES") || ieq(v, "ON") || ieq(v, "1");
        else if (ieq(tok, "CONFIG"))   ic->dhcp = !ieq(v, "STATIC");
        else if (ieq(tok, "ADDRESS")) {
            if (ieq(v, "DHCP")) ic->dhcp = TRUE;
            else if (pn_parse_ip(v, &ic->address)) ic->dhcp = FALSE;
            else warnf(warn, "%s: bad ADDRESS '%s'", name, v);
        }
        else if (ieq(tok, "NETMASK")) { if (!pn_parse_ip(v, &ic->netmask)) warnf(warn, "%s: bad NETMASK '%s'", name, v); }
        else if (ieq(tok, "GATEWAY")) { if (!pn_parse_ip(v, &ic->gateway)) warnf(warn, "%s: bad GATEWAY '%s'", name, v); }
        else warnf(warn, "%s: unknown key '%s'", name, tok);
    }
    FreeVec(data);
    if (ntok == 0)
        return -1;
    if (!ic->device[0]) {
        snprintf(err, errsize, "interface %s: no DEVICE=", name);
        return 0;
    }
    {   /* serial.device is never a SANA-II driver: SLIP over it is meant */
        size_t n = strlen(ic->device);
        if (n >= 13 && ieq(ic->device + n - 13, "serial.device"))
            ic->slip = TRUE;
    }
    if (!ic->dhcp && !ic->address) {
        snprintf(err, errsize, "interface %s: CONFIG=STATIC needs ADDRESS=", name);
        return 0;
    }
    if (ic->mtu && (ic->mtu < 576 || ic->mtu > 16384)) {
        char num[12];
        snprintf(num, sizeof(num), "%lu", (unsigned long)ic->mtu);
        warnf(warn, "%s: MTU %s out of range, using the driver's", name, num);
        ic->mtu = 0;
    }
    if (ic->stall && (ic->stall < 500 || ic->stall > 60000)) {
        char num[12];
        snprintf(num, sizeof(num), "%lu", (unsigned long)ic->stall);
        warnf(warn, "%s: STALL %s ms out of range (500-60000), using the default", name, num);
        ic->stall = 0;
    }
    if (!ic->dhcp && !ic->netmask)
        ic->netmask = PP_HTONL(0xffffff00UL);
    return 1;
}

/* Add the interface file names in `dir` to names[] (no duplicates). */
static int scan_names(const char *dir, char names[][16], int n, int max)
{
    BPTR lock = Lock((CONST_STRPTR)dir, ACCESS_READ);
    struct FileInfoBlock *fib;
    int i;

    if (!lock)
        return n;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib) && n < max) {
            const char *fn = fib->fib_FileName;
            size_t l = strlen(fn);
            if (fib->fib_DirEntryType > 0 || fn[0] == '.' || l >= 16 ||
                (l > 5 && ieq(fn + l - 5, ".info")) || (l > 4 && ieq(fn + l - 4, ".bak")) ||
                (l > 4 && ieq(fn + l - 4, ".new")))      /* Prefs' half-written file */
                continue;
            for (i = 0; i < n && !ieq(names[i], fn); i++)
                ;
            if (i == n)
                strcpy(names[n++], fn);
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return n;
}

/*
 * Interface files, alphabetical, max PN_MAX_IFACES.
 *
 * OS 3.2 links RAM:ENV to ENVARC: and copies files in on first access by
 * name, so listing ENV:PrismNet/interfaces after a boot shows nothing. The
 * names are therefore taken from ENV: *and* ENVARC:, and each file is opened
 * through ENV: (which pulls it in), falling back to ENVARC:.
 */
static BOOL load_ifaces(struct PnConfig *cfg, char *err, int errsize, void (*warn)(const char *))
{
    char names[PN_MAX_IFACES * 2][16];
    int n = 0, i, j;

    n = scan_names(PN_CONFIG_DIR "/interfaces", names, n, PN_MAX_IFACES * 2);
    n = scan_names(PN_CONFIG_ARC "/interfaces", names, n, PN_MAX_IFACES * 2);

    for (i = 0; i < n; i++)             /* tiny sort */
        for (j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) < 0) {
                char t[16];
                strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t);
            }
    if (n > PN_MAX_IFACES) {
        warnf(warn, "only the first %s interfaces are used (%s)", "4", names[PN_MAX_IFACES]);
        n = PN_MAX_IFACES;
    }
    for (i = 0; i < n; i++) {
        char path[96];
        struct PnIfConfig *ic = &cfg->ifs[cfg->nifs];
        BPTR l;
        int rc;
        snprintf(path, sizeof(path), PN_CONFIG_DIR "/interfaces/%s", names[i]);
        if (!(l = Lock((CONST_STRPTR)path, ACCESS_READ)))
            snprintf(path, sizeof(path), PN_CONFIG_ARC "/interfaces/%s", names[i]);
        else
            UnLock(l);
        rc = load_iface(ic, names[i], path, err, errsize, warn);
        if (rc < 0)
            continue;                   /* empty: switched off */
        if (rc == 0) {                  /* bad: the others still come up */
            warnf(warn, "%s - skipped%s", err, "");
            continue;
        }
        for (j = 0; j < cfg->nifs; j++)   /* an editor copy of eth0 is not a second card */
            if (ieq(cfg->ifs[j].device, ic->device) && cfg->ifs[j].unit == ic->unit)
                break;
        if (j < cfg->nifs) {
            warnf(warn, "%s: same device and unit as %s, skipped", names[i], cfg->ifs[j].name);
            continue;
        }
        cfg->nifs++;
    }
    if (cfg->nifs == 0 && n > 0 && err[0])
        return FALSE;                   /* every file was bad: say so */
    return TRUE;
}

static BOOL config_load(struct PnConfig *cfg, char *err, int errsize, void (*warn)(const char *));

/* No "Please insert volume" requesters while reading the configuration: a
 * missing ENVARC: (a bare boot has none) or ENV:
 * just means no file. */
BOOL pn_config_load(struct PnConfig *cfg, char *err, int errsize, void (*warn)(const char *))
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    BOOL ok;

    me->pr_WindowPtr = (APTR)-1;
    ok = config_load(cfg, err, errsize, warn);
    me->pr_WindowPtr = win;
    return ok;
}

static BOOL config_load(struct PnConfig *cfg, char *err, int errsize, void (*warn)(const char *))
{
    char *data, *p, tok[128], val[128];

    memset(cfg, 0, sizeof(*cfg));
    cfg->autostart = TRUE;
    strcpy(cfg->inetd, "C:inetd");
    strcpy(cfg->hostname, "amiga");
    err[0] = 0;

    if (!load_ifaces(cfg, err, errsize, warn))
        return FALSE;

    if ((data = load(PN_CONFIG_DIR "/routes")) != NULL) {
        for (p = data; token(&p, tok, sizeof(tok)); ) {
            if (ieq(tok, "DEFAULT") && token(&p, val, sizeof(val))) {
                char *v = strchr(val, '=');      /* DEFAULT GATEWAY=x also accepted */
                if (!pn_parse_ip(v ? v + 1 : val, &cfg->default_gw))
                    warnf(warn, "routes: bad default gateway '%s'%s", val, "");
            } else if (ieq(tok, "ROUTE") && token(&p, val, sizeof(val))) {
                /* ROUTE a.b.c.d[/bits] [GATEWAY=]gw - a host route without /bits */
                char gw[64], *slash = strchr(val, '/'), *v;
                struct PnRoute *rt = &cfg->routes[cfg->nroutes];
                ULONG bits = 32;
                if (!token(&p, gw, sizeof(gw))) {
                    warnf(warn, "routes: ROUTE %s needs a gateway%s", val, "");
                    continue;
                }
                if (slash) { *slash++ = 0; bits = strtoul(slash, NULL, 10); }
                v = strchr(gw, '=');
                if (cfg->nroutes >= PN_MAX_ROUTES) {
                    warnf(warn, "routes: only 16 routes are used (ignoring %s)%s", val, "");
                } else if (bits > 32 || !pn_parse_ip(val, &rt->net) || !pn_parse_ip(v ? v + 1 : gw, &rt->gw)) {
                    warnf(warn, "routes: bad ROUTE %s %s", val, gw);
                } else {
                    rt->bits = (UBYTE)bits;
                    rt->mask = bits ? PP_HTONL(0xffffffffUL << (32 - bits)) : 0;
                    rt->net &= rt->mask;
                    cfg->nroutes++;
                }
            } else {
                warnf(warn, "routes: unknown keyword '%s'%s", tok, "");
                while (*p && *p != '\n') p++;
            }
        }
        FreeVec(data);
    }

    if ((data = load(PN_CONFIG_DIR "/dns")) != NULL) {
        for (p = data; token(&p, tok, sizeof(tok)); ) {
            if (ieq(tok, "NAMESERVER") && token(&p, val, sizeof(val))) {
                if (cfg->nns < 3 && pn_parse_ip(val, &cfg->nameservers[cfg->nns]))
                    cfg->nns++;
                else
                    warnf(warn, "dns: ignoring nameserver '%s'%s", val, "");
            } else if ((ieq(tok, "DOMAIN") || ieq(tok, "SEARCH")) && token(&p, val, sizeof(val))) {
                strncpy(cfg->domain, val, sizeof(cfg->domain) - 1);
            } else {
                warnf(warn, "dns: unknown keyword '%s'%s", tok, "");
            }
        }
        FreeVec(data);
    }

    if ((data = load(PN_CONFIG_DIR "/hostname")) != NULL) {
        p = data;
        if (token(&p, tok, sizeof(tok)))
            strncpy(cfg->hostname, tok, sizeof(cfg->hostname) - 1);
        FreeVec(data);
    }

    if ((data = load(PN_CONFIG_DIR "/time")) != NULL) {
        cfg->setclock = TRUE;           /* the file's presence switches it on */
        cfg->ntp_dhcp = TRUE;
        strcpy(cfg->ntp_server, "pool.ntp.org");
        for (p = data; token(&p, tok, sizeof(tok)); ) {
            char *v = strchr(tok, '=');
            if (v && (*v++ = 0, ieq(tok, "SERVER"))) {
                if (ieq(v, "DHCP")) cfg->ntp_dhcp = TRUE;
                else { cfg->ntp_dhcp = FALSE; strncpy(cfg->ntp_server, v, sizeof(cfg->ntp_server) - 1); }
            } else if (v && ieq(tok, "SETCLOCK"))
                cfg->setclock = ieq(v, "YES") || ieq(v, "ON") || ieq(v, "1");
            else
                warnf(warn, "time: unknown key '%s'%s", tok, "");
        }
        FreeVec(data);
    }

    if ((data = load(PN_CONFIG_DIR "/startup")) != NULL) {
        for (p = data; token(&p, tok, sizeof(tok)); ) {
            char *v = strchr(tok, '=');
            if (v && (*v++ = 0, ieq(tok, "AUTOSTART")))
                cfg->autostart = ieq(v, "YES") || ieq(v, "ON") || ieq(v, "1");
            else if (v && ieq(tok, "MEMORY"))     /* KB lwIP may use */
                cfg->mem_budget = (ULONG)atoi(v) * 1024;
            else if (v && ieq(tok, "COMMAND"))
                ;                       /* used by bsdsocket.library's auto-start */
            else if (v && ieq(tok, "INETD"))      /* the service dispatcher's path */
                strncpy(cfg->inetd, v, sizeof(cfg->inetd) - 1);
            else
                warnf(warn, "startup: unknown key '%s'%s", tok, "");
        }
        FreeVec(data);
    }
    return TRUE;
}
