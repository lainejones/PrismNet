/*
 * rscfg - exercise Roadshow's configuration API on any bsdsocket.library:
 * AddRouteTagList / ChangeRouteTagList / DeleteRouteTagList, GetRouteInfo,
 * the default gateway, AddDomainNameServer / RemoveDomainNameServer,
 * SetDefaultDomainName, ConfigureInterfaceTagList, AddInterfaceTagList and
 * RemoveInterface. Every change is put back before it ends. Built against
 * the NDK headers only, like a Roadshow program would be.
 *
 *   rscfg [IF=eth0] [GW=10.0.2.2]      (the defaults suit WinUAE's slirp)
 *
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/route.h>
#include <bsdsocket/socketbasetags.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>
#include <inline/macros.h>

/* this NDK's inline/bsdsocket.h leaves it out (its pragmas list it at -426) */
#ifndef ChangeRouteTagList
#define ChangeRouteTagList(tags)     LP1(0x1aa, LONG, ChangeRouteTagList, struct TagItem *, tags, a0, , SocketBase)
#endif

struct Library *SocketBase;
static int fails;
static LONG err;

static void check(int ok, const char *what)
{
    printf("  %-50s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static const char *ip(ULONG a)
{
    static char buf[4][16];
    static int n;
    char *b = buf[n++ & 3];
    sprintf(b, "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24), (unsigned long)(a >> 16) & 255,
            (unsigned long)(a >> 8) & 255, (unsigned long)a & 255);
    return b;
}

/* the route for net/bits in GetRouteInfo(): its gateway (0 = none), or -1 if missing.
 * net = 0 and bits = 0: the default route. */
static LONG find_route(ULONG net, int bits, ULONG *flags)
{
    struct rt_msghdr *buf = GetRouteInfo(AF_INET, 0), *rtm;
    UBYTE *p;
    LONG found = -1;
    int count = 0;

    if (!buf)
        return -2;
    for (p = (UBYTE *)buf; ((struct rt_msghdr *)p)->rtm_msglen; p += ((struct rt_msghdr *)p)->rtm_msglen) {
        struct sockaddr_in *sa;
        ULONG dst = 0, gw = 0, mask = 0;
        int k, b = 0;
        rtm = (struct rt_msghdr *)p;
        sa = (struct sockaddr_in *)(rtm + 1);
        for (k = 0; k < 3; k++) {
            if (!(rtm->rtm_addrs & (1 << k)))
                continue;
            if (k == 0) dst = sa->sin_addr.s_addr;
            if (k == 1) gw = sa->sin_addr.s_addr;
            if (k == 2) mask = sa->sin_addr.s_addr;
            sa++;
        }
        while (b < 32 && (mask & (0x80000000UL >> b))) b++;
        if (++count > 64)
            break;
        if (dst == net && b == bits) {
            found = (LONG)gw;
            if (flags) *flags = rtm->rtm_flags;
        }
    }
    FreeRouteInfo(buf);
    return found;
}

static void show_routes(void)
{
    struct rt_msghdr *buf = GetRouteInfo(AF_INET, 0);
    UBYTE *p;

    if (!buf) { printf("  (GetRouteInfo failed)\n"); return; }
    for (p = (UBYTE *)buf; ((struct rt_msghdr *)p)->rtm_msglen; p += ((struct rt_msghdr *)p)->rtm_msglen) {
        struct rt_msghdr *rtm = (struct rt_msghdr *)p;
        struct sockaddr_in *sa = (struct sockaddr_in *)(rtm + 1);
        ULONG v[3] = { 0, 0, 0 };
        int k;
        for (k = 0; k < 3; k++)
            if (rtm->rtm_addrs & (1 << k))
                v[k] = (sa++)->sin_addr.s_addr;
        printf("  route %-15s mask %-15s gw %-15s flags %04lx\n", ip(v[0]), ip(v[2]), ip(v[1]),
               (unsigned long)rtm->rtm_flags);
    }
    FreeRouteInfo(buf);
}

static int have_dns(const char *addr)
{
    struct List *l = ObtainDomainNameServerList();
    struct Node *n;
    int found = 0;
    if (!l)
        return -1;
    for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (!strcmp((char *)((struct DomainNameServerNode *)n)->dnsn_Address, addr))
            found = 1;
    ReleaseDomainNameServerList(l);
    return found;
}

static int can_resolve(void)
{
    return gethostbyname((STRPTR)"aminet.net") != NULL;
}

/* A TCP connect to 1.1.1.1:53, off the local subnet. 1 = connected,
 * 0 = refused at once as unreachable (no route), -1 = failed otherwise.
 * (A name lookup can't tell: answers are cached, and slirp's name server
 * is on the local subnet.) */
static int reach(void)
{
    struct sockaddr_in sa;
    LONG s = socket(AF_INET, SOCK_STREAM, 0), rc;
    if (s < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = 53;
    sa.sin_addr.s_addr = inet_addr((STRPTR)"1.1.1.1");
    err = 0;
    rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    CloseSocket(s);
    if (rc == 0)
        return 1;
    return (err == 65 || err == 51) ? 0 : -1;   /* EHOSTUNREACH, ENETUNREACH */
}

int main(int argc, char **argv)
{
    const char *ifname = "eth0", *gw = "10.0.2.2";
    struct TagItem t[4];
    ULONG oldgw, gwaddr, flags = 0;
    LONG v = -1, mtu = 0;
    char dom[64], olddom[64];
    int i;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "IF=", 3)) ifname = argv[i] + 3;
        if (!strncmp(argv[i], "GW=", 3)) gw = argv[i] + 3;
    }
    if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) {
        printf("rscfg: cannot open bsdsocket.library\n");
        return RETURN_FAIL;
    }
    gwaddr = inet_addr((STRPTR)gw);     /* a library call: only after OpenLibrary */
    SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(err))), (ULONG)&err, TAG_END);
    printf("rscfg: %s, interface %s, gateway %s\n", (char *)SocketBase->lib_IdString, ifname, gw);

    printf("[capabilities]\n");
    SocketBaseTags(SBTM_GETREF(SBTC_HAVE_ROUTING_API), (ULONG)&v, TAG_END);
    check(v == TRUE, "SBTC_HAVE_ROUTING_API");

    printf("[routes]\n");
    show_routes();
    oldgw = (ULONG)find_route(0, 0, NULL);
    check(oldgw == gwaddr, "default route via the gateway");
    t[0].ti_Tag = RTA_DestinationNet; t[0].ti_Data = (ULONG)"192.168.77.0/24";
    t[1].ti_Tag = RTA_Gateway;        t[1].ti_Data = (ULONG)gw;
    t[2].ti_Tag = TAG_DONE;
    check(AddRouteTagList(t) == 0, "AddRouteTagList 192.168.77.0/24");
    check(find_route(inet_addr((STRPTR)"192.168.77.0"), 24, &flags) == (LONG)gwaddr &&
          (flags & RTF_STATIC) && (flags & RTF_GATEWAY), "... listed by GetRouteInfo (static, gateway)");
    t[1].ti_Data = (ULONG)"10.0.2.3";
    check(ChangeRouteTagList(t) == 0, "ChangeRouteTagList to 10.0.2.3");
    check(find_route(inet_addr((STRPTR)"192.168.77.0"), 24, NULL) == (LONG)inet_addr((STRPTR)"10.0.2.3"),
          "... gateway changed");
    t[1].ti_Tag = TAG_DONE;
    check(DeleteRouteTagList(t) == 0, "DeleteRouteTagList");
    check(find_route(inet_addr((STRPTR)"192.168.77.0"), 24, NULL) == -1, "... gone");
    err = 0;
    check(DeleteRouteTagList(t) == -1 && err == 3, "deleting it again: ESRCH");
    t[0].ti_Tag = RTA_DestinationHost; t[0].ti_Data = (ULONG)"192.168.77.9";
    t[1].ti_Tag = RTA_Gateway;         t[1].ti_Data = (ULONG)gw;
    t[2].ti_Tag = TAG_DONE;
    check(AddRouteTagList(t) == 0 && find_route(inet_addr((STRPTR)"192.168.77.9"), 32, &flags) == (LONG)gwaddr &&
          (flags & RTF_HOST), "host route (RTF_HOST)");
    t[1].ti_Tag = TAG_DONE;
    check(DeleteRouteTagList(t) == 0, "... deleted");
    t[0].ti_Tag = RTA_Destination; t[0].ti_Data = (ULONG)"no.such.address";
    t[1].ti_Tag = RTA_Gateway;     t[1].ti_Data = (ULONG)gw;
    err = 0;
    check(AddRouteTagList(t) == -1 && err == 22, "a bad address: EINVAL");

    printf("[default gateway]\n");
    t[0].ti_Tag = RTA_DefaultGateway; t[0].ti_Data = (ULONG)gw;
    t[1].ti_Tag = TAG_DONE;
    check(DeleteRouteTagList(t) == 0, "DeleteRouteTagList RTA_DefaultGateway");
    check(find_route(0, 0, NULL) == -1, "... no default route");
    check(reach() == 0, "... 1.1.1.1 unreachable at once (EHOSTUNREACH)");
    check(AddRouteTagList(t) == 0, "AddRouteTagList RTA_DefaultGateway");
    check(find_route(0, 0, NULL) == (LONG)gwaddr, "... default route back");
    check(reach() == 1 && can_resolve(), "... 1.1.1.1 connects, aminet.net resolves");

    printf("[name servers]\n");
    check(have_dns("9.9.9.9") == 0, "9.9.9.9 not in the list yet");
    check(AddDomainNameServer((STRPTR)"9.9.9.9") == 0, "AddDomainNameServer 9.9.9.9");
    check(have_dns("9.9.9.9") == 1, "... in ObtainDomainNameServerList");
    check(RemoveDomainNameServer((STRPTR)"9.9.9.9") == 0, "RemoveDomainNameServer");
    check(have_dns("9.9.9.9") == 0, "... gone");
    err = 0;
    check(RemoveDomainNameServer((STRPTR)"9.9.9.9") == -1 && err == 3, "removing it again: ESRCH");
    check(can_resolve(), "names still resolve");

    printf("[default domain]\n");
    olddom[0] = 0;
    GetDefaultDomainName((STRPTR)olddom, sizeof(olddom));
    SetDefaultDomainName((STRPTR)"rscfg.test");
    check(GetDefaultDomainName((STRPTR)dom, sizeof(dom)) && !strcmp(dom, "rscfg.test"),
          "SetDefaultDomainName / GetDefaultDomainName");
    SetDefaultDomainName((STRPTR)olddom);

    printf("[interfaces]\n");
    t[0].ti_Tag = IFC_LimitMTU; t[0].ti_Data = 1400;
    t[1].ti_Tag = TAG_DONE;
    check(ConfigureInterfaceTagList((STRPTR)ifname, t) == 0, "ConfigureInterfaceTagList IFC_LimitMTU 1400");
    QueryInterfaceTags((STRPTR)ifname, IFQ_MTU, (ULONG)&mtu, TAG_END);
    check(mtu == 1400, "... IFQ_MTU says 1400");
    t[0].ti_Tag = IFC_State; t[0].ti_Data = SM_Down;
    check(ConfigureInterfaceTagList((STRPTR)ifname, t) == 0 && reach() == 0, "IFC_State SM_Down: unreachable");
    t[0].ti_Data = SM_Up;
    check(ConfigureInterfaceTagList((STRPTR)ifname, t) == 0 && reach() == 1, "IFC_State SM_Up: connects again");
    err = 0;
    check(ConfigureInterfaceTagList((STRPTR)"nosuch0", t) == -1 && err == 19, "an unknown interface: ENODEV");
    t[0].ti_Tag = IFC_Address; t[0].ti_Data = (ULONG)"not-an-address";
    err = 0;
    check(ConfigureInterfaceTagList((STRPTR)ifname, t) == -1 && err == 22, "a bad IFC_Address: EINVAL");
    err = 0;
    check(AddInterfaceTagList((STRPTR)"test1", (STRPTR)"nosuch.device", 0, NULL) == -1 && err == 19,
          "AddInterfaceTagList with no such driver: ENODEV");
    err = 0;
    check(RemoveInterface((STRPTR)ifname, FALSE) == -1 && err == 16,
          "RemoveInterface of the only interface: EBUSY");

    printf("[after]\n");
    show_routes();
    printf("rscfg: %d failed -> %s\n", fails, fails ? "FAIL" : "PASS");
    CloseLibrary(SocketBase);
    return fails ? RETURN_WARN : RETURN_OK;
}
