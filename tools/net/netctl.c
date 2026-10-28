/*
 * NetCtl - change the running network from the Shell or a script: add and
 * remove an interface, routes and name servers. Uses Roadshow's configuration
 * calls (AddInterfaceTagList, ConfigureInterfaceTagList, AddRouteTagList ...),
 * so it works with PrismNet and Roadshow. Changes last until the stack stops.
 * Part of PrismNet. GPL-3.0-only, see LICENSE.
 *
 *   NetCtl IFUP name DEVICE=driver [UNIT=n] [ADDRESS=a] [NETMASK=m] [PEER=p] [MTU=n]
 *   NetCtl IFDOWN name
 *   NetCtl ADDROUTE dest[/bits]|DEFAULT gateway
 *   NetCtl DELROUTE dest[/bits]|DEFAULT [gateway]
 *   NetCtl GATEWAY                   prints the default gateway (WARN: none)
 *   NetCtl DNS a [b] [c] | PUSHED    replaces the name servers (PUSHED: the ones an
 *                                    OpenVPN server pushed); the old ones are kept
 *   NetCtl DNSRESTORE                puts the ones DNS replaced back
 *   NetCtl ROUTES                    lists the routes
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <net/route.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

#include "nettools.h"

static const char ver[] __attribute__((used)) = "$VER: NetCtl 1.0 (28.10.2026) PrismNet";

struct Library *SocketBase;
static LONG err;

#define TEMPLATE "ACTION/A,ARGS/M,DEVICE/K,UNIT/K/N,ADDRESS/K,NETMASK/K,PEER/K,MTU/K/N,QUIET/S"
enum { A_ACTION, A_ARGS, A_DEVICE, A_UNIT, A_ADDRESS, A_NETMASK, A_PEER, A_MTU, A_QUIET, A_N };

/* the name servers DNS replaced, one per line, for DNSRESTORE */
#define SAVED_DNS "NetCtl.dns"

static BOOL quiet;

static int fail(const char *what)
{
    printf("NetCtl: %s failed: %s\n", what, err ? (char *)strerror(err) : "error");
    return RETURN_ERROR;
}

static void ok(const char *fmt, const char *a, const char *b)
{
    if (!quiet) {
        printf(fmt, a, b ? b : "");
        printf("\n");
    }
}

static BOOL is_default(const char *s)
{
    return !stricmp(s, "DEFAULT") || !strcmp(s, "0.0.0.0/0");
}

/* "a.b.c.d/32" is a host route, as is a bare address */
static ULONG dest_tag(const char *s)
{
    const char *slash = strchr(s, '/');
    return (!slash || !strcmp(slash, "/32")) ? RTA_DestinationHost : RTA_DestinationNet;
}

static int route(BOOL add, STRPTR *a)
{
    struct TagItem t[3];
    char host[20];
    int n = 0;

    if (!a || !a[0] || (add && !a[1])) {
        printf("NetCtl: %s destination[/bits]|DEFAULT gateway\n", add ? "ADDROUTE" : "DELROUTE");
        return RETURN_ERROR;
    }
    if (is_default((char *)a[0])) {
        t[n].ti_Tag = RTA_DefaultGateway;
        t[n++].ti_Data = (ULONG)(a[1] ? a[1] : (STRPTR)"0.0.0.0");
    } else {
        const char *d = (char *)a[0];
        if (dest_tag(d) == RTA_DestinationHost && strchr(d, '/')) {
            snprintf(host, sizeof(host), "%.*s", (int)(strchr(d, '/') - d), d);
            d = host;
        }
        t[n].ti_Tag = dest_tag((char *)a[0]);
        t[n++].ti_Data = (ULONG)d;
        if (a[1]) {
            t[n].ti_Tag = RTA_Gateway;
            t[n++].ti_Data = (ULONG)a[1];
        }
    }
    t[n].ti_Tag = TAG_DONE;
    err = 0;
    if ((add ? AddRouteTagList(t) : DeleteRouteTagList(t)) != 0)
        return fail(add ? "ADDROUTE" : "DELROUTE");
    ok(add ? "route %s via %s added" : "route %s removed%s", (char *)a[0], add ? (char *)a[1] : NULL);
    return RETURN_OK;
}

/* walk GetRouteInfo(): calls fn for every route (addresses in network order) */
static int each_route(void (*fn)(ULONG dst, ULONG mask, ULONG gw, void *ud), void *ud)
{
    struct rt_msghdr *buf = GetRouteInfo(AF_INET, 0);
    UBYTE *p;
    int count = 0;

    if (!buf)
        return -1;
    for (p = (UBYTE *)buf; ((struct rt_msghdr *)p)->rtm_msglen && count < 256;
         p += ((struct rt_msghdr *)p)->rtm_msglen, count++) {
        struct rt_msghdr *rtm = (struct rt_msghdr *)p;
        struct sockaddr_in *sa = (struct sockaddr_in *)(rtm + 1);
        ULONG v[3] = { 0, 0, 0 };
        int k;
        for (k = 0; k < 3; k++)
            if (rtm->rtm_addrs & (1 << k))
                v[k] = (sa++)->sin_addr.s_addr;
        fn(v[0], v[2], v[1], ud);
    }
    FreeRouteInfo(buf);
    return count;
}

static void find_default(ULONG dst, ULONG mask, ULONG gw, void *ud)
{
    if (!dst && !mask && gw)
        *(ULONG *)ud = gw;
}

static void print_route(ULONG dst, ULONG mask, ULONG gw, void *ud)
{
    char a[16], g[16];
    int bits = 0;
    while (bits < 32 && (mask & (0x80000000UL >> bits)))
        bits++;
    nt_ipstr(dst, a);
    nt_ipstr(gw, g);
    if (!dst && !mask)
        printf("default            via %s\n", g);
    else {
        char d[20];
        snprintf(d, sizeof(d), "%s/%d", a, bits);
        printf("%-18s %s%s\n", d, gw ? "via " : "direct", gw ? g : "");
    }
}

static int gateway(void)
{
    ULONG gw = 0;
    char g[16];

    if (each_route(find_default, &gw) < 0) {
        printf("NetCtl: GetRouteInfo failed\n");
        return RETURN_ERROR;
    }
    if (!gw)
        return RETURN_WARN;             /* nothing printed: a script tests WARN */
    nt_ipstr(gw, g);
    printf("%s\n", g);
    return RETURN_OK;
}

/* a script's "${name}" stays as it is when OpenVPN didn't set that variable:
 * treat it like a value that wasn't given */
static BOOL given(LONG v)
{
    return v && ((char *)v)[0] && ((char *)v)[0] != '$';
}

static int ifup(STRPTR *a, LONG *args)
{
    struct TagItem t[7];
    int n = 0;
    LONG unit = args[A_UNIT] ? *(LONG *)args[A_UNIT] : 0;

    if (!a || !a[0]) {
        printf("NetCtl: IFUP name DEVICE=driver [UNIT=n] [ADDRESS=a] [NETMASK=m] [PEER=p] [MTU=n]\n");
        return RETURN_ERROR;
    }
    if (args[A_DEVICE]) {
        err = 0;
        if (AddInterfaceTagList(a[0], (STRPTR)args[A_DEVICE], unit, NULL) != 0 && err != EEXIST)
            return fail("adding the interface");
    }
    if (given(args[A_ADDRESS])) { t[n].ti_Tag = IFC_Address;            t[n++].ti_Data = args[A_ADDRESS]; }
    if (given(args[A_NETMASK])) { t[n].ti_Tag = IFC_NetMask;            t[n++].ti_Data = args[A_NETMASK]; }
    if (given(args[A_PEER]))    { t[n].ti_Tag = IFC_DestinationAddress; t[n++].ti_Data = args[A_PEER]; }
    if (args[A_MTU])            { t[n].ti_Tag = IFC_LimitMTU;           t[n++].ti_Data = *(LONG *)args[A_MTU]; }
    t[n].ti_Tag = IFC_State; t[n++].ti_Data = SM_Up;
    t[n].ti_Tag = TAG_DONE;
    err = 0;
    if (ConfigureInterfaceTagList(a[0], t) != 0)
        return fail("configuring the interface");
    ok("%s is up%s", (char *)a[0], NULL);
    return RETURN_OK;
}

static int ifdown(STRPTR *a)
{
    if (!a || !a[0]) {
        printf("NetCtl: IFDOWN name\n");
        return RETURN_ERROR;
    }
    err = 0;
    if (RemoveInterface(a[0], FALSE) != 0)
        return fail("IFDOWN");
    ok("%s removed%s", (char *)a[0], NULL);
    return RETURN_OK;
}

/* the current name servers into list[] (strings), returns how many */
static int dns_list(char list[][16], int max)
{
    struct List *l = ObtainDomainNameServerList();
    struct Node *nd;
    int n = 0;

    if (!l)
        return -1;
    for (nd = l->lh_Head; nd->ln_Succ && n < max; nd = nd->ln_Succ)
        snprintf(list[n++], 16, "%s", (char *)((struct DomainNameServerNode *)nd)->dnsn_Address);
    ReleaseDomainNameServerList(l);
    return n;
}

/* replace every name server by the ones in want[] */
static int dns_replace(STRPTR *want)
{
    char cur[8][16];
    int i, n = dns_list(cur, 8);

    if (n < 0) {
        printf("NetCtl: cannot read the name servers\n");
        return RETURN_ERROR;
    }
    for (i = 0; i < n; i++)
        RemoveDomainNameServer((STRPTR)cur[i]);
    for (i = 0; want[i]; i++) {
        err = 0;
        if (AddDomainNameServer(want[i]) != 0)
            return fail("adding a name server");
    }
    return RETURN_OK;
}

/* DNS PUSHED: the servers an OpenVPN server pushed, from the variables its
 * scripts get ("foreign_option_1" = "dhcp-option DNS 10.8.0.1" ...) */
static int pushed_dns(STRPTR *want, char store[][16], int max)
{
    char name[24], val[80];
    int i, n = 0;

    for (i = 1; i <= 16 && n < max; i++) {
        snprintf(name, sizeof(name), "foreign_option_%d", i);
        if (GetVar((STRPTR)name, (STRPTR)val, sizeof(val), 0) < 0)
            continue;
        if (!strnicmp(val, "dhcp-option DNS ", 16) && strlen(val + 16) < 16) {
            strcpy(store[n], val + 16);
            want[n] = (STRPTR)store[n];
            n++;
        }
    }
    want[n] = NULL;
    return n;
}

static int dns(STRPTR *a)
{
    char cur[8][16], buf[8 * 17], pushed[3][16];
    STRPTR pw[4];
    int i, n, used = 0;

    if (!a || !a[0]) {
        printf("NetCtl: DNS address [address] [address] | DNS PUSHED\n");
        return RETURN_ERROR;
    }
    if (!stricmp((char *)a[0], "PUSHED")) {
        if (!pushed_dns(pw, pushed, 3)) {
            ok("no name server was pushed%s", "", NULL);
            return RETURN_WARN;
        }
        a = pw;
    }
    /* keep the servers from before the first DNS only: a second DNS must not
     * save the VPN's servers over the real ones */
    if (GetVar((STRPTR)SAVED_DNS, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) < 0) {
        n = dns_list(cur, 8);
        buf[0] = 0;
        for (i = 0; i < n; i++)
            used += snprintf(buf + used, sizeof(buf) - used, "%s\n", cur[i]);
        SetVar((STRPTR)SAVED_DNS, (STRPTR)buf, used, GVF_GLOBAL_ONLY);
    }
    if (dns_replace(a) != RETURN_OK)
        return RETURN_ERROR;
    ok("name servers now %s%s", (char *)a[0], a[1] ? " ..." : NULL);
    return RETURN_OK;
}

static int dns_restore(void)
{
    char buf[8 * 17], *p, *line;
    STRPTR want[9];
    int n = 0;

    if (GetVar((STRPTR)SAVED_DNS, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) < 0) {
        ok("nothing to restore%s", "", NULL);
        return RETURN_WARN;
    }
    for (p = buf; (line = strtok(p, "\n")) && n < 8; p = NULL)
        if (*line)
            want[n++] = (STRPTR)line;
    want[n] = NULL;
    if (dns_replace(want) != RETURN_OK)
        return RETURN_ERROR;
    DeleteVar((STRPTR)SAVED_DNS, GVF_GLOBAL_ONLY);
    ok("name servers restored%s", "", NULL);
    return RETURN_OK;
}

static void usage(void)
{
    printf("NetCtl IFUP name DEVICE=driver [UNIT=n] [ADDRESS=a] [NETMASK=m] [PEER=p] [MTU=n]\n"
           "NetCtl IFDOWN name\n"
           "NetCtl ADDROUTE destination[/bits]|DEFAULT gateway\n"
           "NetCtl DELROUTE destination[/bits]|DEFAULT [gateway]\n"
           "NetCtl GATEWAY | ROUTES\n"
           "NetCtl DNS address [address] [address] | DNSRESTORE\n");
}

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    const char *act;
    STRPTR *a;
    int rc;

    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"NetCtl");
        usage();
        return RETURN_FAIL;
    }
    act = (const char *)args[A_ACTION];
    a = (STRPTR *)args[A_ARGS];
    quiet = args[A_QUIET] != 0;
    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("NetCtl: no TCP/IP stack (bsdsocket.library)\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(err))), (ULONG)&err, TAG_END);
    {
        LONG have = FALSE;
        SocketBaseTags(SBTM_GETREF(SBTC_HAVE_ROUTING_API), (ULONG)&have, TAG_END);
        if (!have) {
            printf("NetCtl: this TCP/IP stack has no routing API (PrismNet or Roadshow needed)\n");
            rc = RETURN_FAIL;
            goto out;
        }
    }
    if      (!stricmp(act, "IFUP"))       rc = ifup(a, args);
    else if (!stricmp(act, "IFDOWN"))     rc = ifdown(a);
    else if (!stricmp(act, "ADDROUTE"))   rc = route(TRUE, a);
    else if (!stricmp(act, "DELROUTE"))   rc = route(FALSE, a);
    else if (!stricmp(act, "GATEWAY"))    rc = gateway();
    else if (!stricmp(act, "DNS"))        rc = dns(a);
    else if (!stricmp(act, "DNSRESTORE")) rc = dns_restore();
    else if (!stricmp(act, "ROUTES"))     rc = each_route(print_route, NULL) < 0 ? RETURN_ERROR : RETURN_OK;
    else { usage(); rc = RETURN_ERROR; }
out:
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
