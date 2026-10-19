/*
 * PrismNet core task: gethostbyname()/gethostbyaddr() on lwIP's DNS client.
 * GPL-3.0-only, see LICENSE.
 *
 * Lookups are asynchronous: the request stays pending until lwIP's DNS
 * callback fires. The callback gets a small slot, not the request itself,
 * so an aborted request (Ctrl-C) can be detached safely.
 */
#include <string.h>
#include <ctype.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "core.h"
#include "config.h"

#define NUM_SLOTS 16
#define RETRY_MS  10000   /* keep retrying a lookup lwIP had no room for */

struct lookup {
    struct PnReq *req;      /* NULL = free or detached */
    BOOL          busy;     /* lwIP still owns the callback */
};
static struct lookup slots[NUM_SLOTS];

static void fill_hostent(struct PnHostBuf *hb, const char *name, const ip_addr_t *addr)
{
    memset(hb, 0, sizeof(*hb));
    strncpy(hb->name, name, sizeof(hb->name) - 1);
    hb->h_name       = hb->name;
    hb->alias_ptrs[0] = NULL;
    hb->h_aliases    = hb->alias_ptrs;
    hb->h_addrtype   = PN_AF_INET;
    hb->h_length     = 4;
    hb->addrs[0]     = ip4_addr_get_u32(ip_2_ip4(addr));   /* network order */
    hb->addr_ptrs[0] = (char *)&hb->addrs[0];
    hb->addr_ptrs[1] = NULL;
    hb->h_addr_list  = hb->addr_ptrs;
}

/* ---------------------------------------------------------------------- */
/* ENV:PrismNet/hosts: "a.b.c.d name [alias...]"                           */
/* ---------------------------------------------------------------------- */

static int hieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

/*
 * The hosts file is parsed once and kept; each lookup only checks whether
 * the file changed (Open + ExamineFH, cheap on RAM:) instead of re-reading
 * and re-parsing 16 KB per name. Opening by name also pulls the file into
 * the lazy ENV: of OS 3.2.
 */
#define HOSTS_MAX_LINES 96
#define HOSTS_MAX_NAMES 7
struct hosts_line {
    ULONG addr;
    char *names[HOSTS_MAX_NAMES];
    int   nnames;
};
static char             *hosts_buf;
static struct hosts_line hosts_ent[HOSTS_MAX_LINES];
static int               hosts_n;
static BOOL              hosts_loaded;
static LONG              hosts_size;
static struct DateStamp  hosts_date;

static void hosts_parse(char *buf)
{
    char *p;

    hosts_n = 0;
    for (p = buf; *p && hosts_n < HOSTS_MAX_LINES; ) {
        char *line = p, *eol = p;
        struct hosts_line *h = &hosts_ent[hosts_n];
        char *tok[HOSTS_MAX_NAMES + 1];
        int nt = 0, i;

        while (*eol && *eol != '\n') eol++;
        p = *eol ? eol + 1 : eol;
        *eol = 0;
        if ((eol = strchr(line, '#')) != NULL) *eol = 0;
        for (eol = line; *eol && nt < HOSTS_MAX_NAMES + 1; ) {
            while (*eol == ' ' || *eol == '\t' || *eol == '\r') *eol++ = 0;
            if (!*eol) break;
            tok[nt++] = eol;
            while (*eol && *eol != ' ' && *eol != '\t' && *eol != '\r') eol++;
        }
        if (nt < 2 || !pn_parse_ip(tok[0], &h->addr))
            continue;
        for (i = 1; i < nt; i++)
            h->names[i - 1] = tok[i];
        h->nnames = nt - 1;
        hosts_n++;
    }
}

static void hosts_refresh(void)
{
    BPTR fh = Open((CONST_STRPTR)PN_HOSTS_FILE, MODE_OLDFILE);
    struct FileInfoBlock *fib;
    BOOL changed = TRUE;
    LONG n;

    if (!fh) {                          /* no file: forget what we had */
        if (hosts_buf) { pn_free(hosts_buf); hosts_buf = NULL; }
        hosts_n = 0;
        hosts_loaded = FALSE;
        return;
    }
    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (ExamineFH(fh, fib)) {
            changed = !hosts_loaded || fib->fib_Size != hosts_size ||
                      memcmp(&fib->fib_Date, &hosts_date, sizeof(hosts_date)) != 0;
            hosts_size = fib->fib_Size;
            hosts_date = fib->fib_Date;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    if (changed) {
        /* sized to the file (up to 16 KB): a few lines cost a few bytes */
        LONG want = hosts_size > 0 && hosts_size < 16383 ? hosts_size : 16383;
        if (hosts_buf) pn_free(hosts_buf);
        hosts_n = 0;
        if (!(hosts_buf = pn_alloc(want + 1, MEMF_ANY))) {
            hosts_loaded = FALSE;
            Close(fh);
            return;
        }
        n = Read(fh, hosts_buf, want);
        hosts_buf[n > 0 ? n : 0] = 0;
        hosts_parse(hosts_buf);
        hosts_loaded = TRUE;
    }
    Close(fh);
}

/*
 * Look a name up (name != NULL) or an address (name == NULL, addr in) in the
 * hosts file. On a match fills *addr and copies the canonical (first) name
 * into canon.
 */
static BOOL hosts_lookup(const char *name, ULONG *addr, char *canon, int canonsize)
{
    int i, j;

    hosts_refresh();
    for (i = 0; i < hosts_n; i++) {
        struct hosts_line *h = &hosts_ent[i];
        BOOL found = FALSE;

        if (name) {
            for (j = 0; j < h->nnames && !found; j++)
                found = hieq(h->names[j], name);
        } else {
            found = (h->addr == *addr);
        }
        if (found) {
            *addr = h->addr;
            strncpy(canon, h->names[0], canonsize - 1);
            canon[canonsize - 1] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void reply_host(struct PnReq *r, const char *name, ULONG addr_n)
{
    ip_addr_t a;
    ip_addr_set_ip4_u32(&a, addr_n);
    fill_hostent((struct PnHostBuf *)r->a[1], name, &a);
    r->herror = 0;
    core_reply(r, 0, 0);
}

static void dns_found(const char *name, const ip_addr_t *addr, void *arg)
{
    struct lookup *l = (struct lookup *)arg;
    struct PnReq *r = l->req;

    l->busy = FALSE;
    l->req  = NULL;
    if (!r)
        return;                 /* aborted meanwhile */
    Remove((struct Node *)&r->node);
    if (addr) {
        fill_hostent((struct PnHostBuf *)r->a[1], name, addr);   /* queried (FQDN) name */
        r->herror = 0;
        core_reply(r, 0, 0);
    } else if (!(r->flags & PNREQ_RETRIED)) {
        /* lwIP reports SERVFAIL like NXDOMAIN; forwarders (slirp, home
         * routers) often SERVFAIL a cold query. Ask once more. */
        r->flags   |= PNREQ_RETRIED;
        r->progress = 0;                /* not attached: core re-runs resolv_op */
        core_pend(r);
    } else {
        r->herror = PN_HOST_NOT_FOUND;
        core_reply(r, -1, 0);
    }
}

static BOOL is_localhost(const char *n)
{
    const char *l = "localhost";
    while (*l && tolower((unsigned char)*n) == *l) { n++; l++; }
    return *l == 0 && *n == 0;
}

/* No room right now: stay pending (progress 0 = not attached to a slot) and
 * let the core re-run us after the next event, up to RETRY_MS. */
static BOOL retry_later(struct PnReq *r)
{
    if (!r->has_deadline) {
        core_set_deadline(r, RETRY_MS);
    } else if ((LONG)(sys_now() - r->deadline) >= 0) {
        r->herror = PN_TRY_AGAIN;
        core_reply(r, -1, 0);
        return TRUE;
    }
    r->progress = 0;
    return FALSE;
}

BOOL resolv_op(struct PnClient *c, struct PnReq *r)
{
    struct PnHostBuf *hb = (struct PnHostBuf *)r->a[1];
    ip_addr_t addr;
    err_t e;
    int i;

    char canon[128], query[192];
    ULONG ha;

    LWIP_UNUSED_ARG(c);
    if (r->op == PNOP_GETHOSTBYADDR) {
        /* No reverse DNS in lwIP: the hosts file, then loopback. */
        ha = r->a[0];
        ip_addr_set_ip4_u32(&addr, r->a[0]);
        if (hosts_lookup(NULL, &ha, canon, sizeof(canon))) {
            reply_host(r, canon, ha);
        } else if (ip4_addr_isloopback(ip_2_ip4(&addr))) {
            fill_hostent(hb, "localhost", &addr);
            r->herror = 0;
            core_reply(r, 0, 0);
        } else {
            r->herror = PN_HOST_NOT_FOUND;
            core_reply(r, -1, 0);
        }
        return TRUE;
    }

    if (!r->a[0] || !*(const char *)r->a[0]) {
        r->herror = PN_HOST_NOT_FOUND;
        core_reply(r, -1, 0);
        return TRUE;
    }
    /* hosts file first (also "name.domain" for a bare name) */
    query[0] = 0;
    if (!strchr((const char *)r->a[0], '.') && core.domain[0]) {
        strncpy(query, (const char *)r->a[0], 100);
        query[100] = 0;
        strcat(query, ".");
        strncat(query, core.domain, sizeof(query) - strlen(query) - 1);
    }
    if (hosts_lookup((const char *)r->a[0], &ha, canon, sizeof(canon)) ||
        (query[0] && hosts_lookup(query, &ha, canon, sizeof(canon)))) {
        reply_host(r, canon, ha);
        return TRUE;
    }
    if (is_localhost((const char *)r->a[0]) ||
        !strcmp((const char *)r->a[0], core.hostname)) {
        /* localhost is loopback; the host's own name is its interface
         * address when it has one (FTP PORT and servers advertise it) */
        if (!is_localhost((const char *)r->a[0]) && netif_default &&
            !ip4_addr_isany_val(*netif_ip4_addr(netif_default)))
            ip_addr_copy(addr, *netif_ip_addr4(netif_default));
        else
            IP_ADDR4(&addr, 127, 0, 0, 1);
        fill_hostent(hb, (const char *)r->a[0], &addr);
        r->herror = 0;
        core_reply(r, 0, 0);
        return TRUE;
    }

    for (i = 0; i < NUM_SLOTS && slots[i].busy; i++)
        ;
    if (i == NUM_SLOTS)
        return retry_later(r);
    /* A bare name is completed with the search domain for DNS. */
    e = dns_gethostbyname(query[0] ? query : (const char *)r->a[0], &addr, dns_found, &slots[i]);
    if (e == ERR_MEM)
        return retry_later(r);              /* lwIP's request table is full */
    if (e == ERR_OK) {                      /* numeric or cached */
        fill_hostent(hb, query[0] ? query : (const char *)r->a[0], &addr);
        r->herror = 0;
        core_reply(r, 0, 0);
        return TRUE;
    }
    if (e != ERR_INPROGRESS) {
        r->herror = e == ERR_ARG ? PN_HOST_NOT_FOUND : PN_TRY_AGAIN;
        core_reply(r, -1, 0);
        return TRUE;
    }
    slots[i].busy = TRUE;
    slots[i].req  = r;
    r->progress   = 1;                      /* attached: dns_found completes it */
    r->has_deadline = FALSE;                /* lwIP's own DNS timeout applies */
    return FALSE;
}

void resolv_abort(struct PnReq *r)
{
    int i;
    for (i = 0; i < NUM_SLOTS; i++)
        if (slots[i].req == r)
            slots[i].req = NULL;    /* slot stays busy until lwIP calls back */
}
