/*
 * rsquery - exercise Roadshow's query API on any bsdsocket.library:
 * SBTC_HAVE_* tags, ObtainInterfaceList, QueryInterfaceTags,
 * GetNetworkStatistics (TCP/UDP sockets), the name server list and
 * GetDefaultDomainName. Built against the NDK headers only, like a
 * Roadshow program would be.
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <bsdsocket/socketbasetags.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

struct Library *SocketBase;
static int fails;

static void check(int ok, const char *what)
{
    printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
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

static LONG have(LONG code)
{
    LONG v = -1;
    if (SocketBaseTags(SBTM_GETREF(code), (ULONG)&v, TAG_END) != 0)
        return -2;                      /* tag refused */
    return v;
}

static void query(STRPTR name)
{
    STRPTR dev = NULL;
    LONG unit = -1, bits = -1, mtu = 0, bps = -1, hwtype = -1, state = -1, bind = -1;
    ULONG rxp = 0, txp = 0, rxq = 0, txq = 0;
    UBYTE hw[16];
    struct sockaddr_in addr, mask, bcast, dns1;
    SBQUAD_T in, out;
    LONG res;

    memset(hw, 0, sizeof(hw));
    memset(&addr, 0, sizeof(addr)); memset(&mask, 0, sizeof(mask));
    memset(&bcast, 0, sizeof(bcast)); memset(&dns1, 0, sizeof(dns1));
    res = QueryInterfaceTags(name,
                             IFQ_DeviceName, (ULONG)&dev, IFQ_DeviceUnit, (ULONG)&unit,
                             IFQ_HardwareAddressSize, (ULONG)&bits, IFQ_HardwareAddress, (ULONG)hw,
                             IFQ_MTU, (ULONG)&mtu, IFQ_BPS, (ULONG)&bps,
                             IFQ_HardwareType, (ULONG)&hwtype,
                             IFQ_PacketsReceived, (ULONG)&rxp, IFQ_PacketsSent, (ULONG)&txp,
                             IFQ_Address, (ULONG)&addr, IFQ_NetMask, (ULONG)&mask,
                             IFQ_BroadcastAddress, (ULONG)&bcast,
                             IFQ_PrimaryDNSAddress, (ULONG)&dns1,
                             IFQ_State, (ULONG)&state, IFQ_AddressBindType, (ULONG)&bind,
                             IFQ_GetBytesIn, (ULONG)&in, IFQ_GetBytesOut, (ULONG)&out,
                             IFQ_NumReadRequestsPending, (ULONG)&rxq,
                             IFQ_NumWriteRequestsPending, (ULONG)&txq,
                             TAG_END);
    printf("  %s: QueryInterfaceTags -> %ld\n", name, (long)res);
    check(res == 0, "QueryInterfaceTags succeeds");
    if (res != 0)
        return;
    printf("    device %s unit %ld, hw type %ld, address %ld bits %02x:%02x:%02x:%02x:%02x:%02x\n",
           dev ? (char *)dev : "(null)", (long)unit, (long)hwtype, (long)bits,
           hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
    printf("    mtu %ld, bps %ld, state %ld, bind type %ld\n", (long)mtu, (long)bps, (long)state,
           (long)bind);
    printf("    address %s netmask %s broadcast %s dns %s\n", ip(addr.sin_addr.s_addr),
           ip(mask.sin_addr.s_addr), ip(bcast.sin_addr.s_addr), ip(dns1.sin_addr.s_addr));
    printf("    packets in %lu out %lu, bytes in %lu out %lu, pending reads %lu writes %lu\n",
           (unsigned long)rxp, (unsigned long)txp, (unsigned long)in.sbq_Low,
           (unsigned long)out.sbq_Low, (unsigned long)rxq, (unsigned long)txq);
    check(dev != NULL && mtu > 0, "device name and MTU filled in");
    check(addr.sin_family == AF_INET && addr.sin_len == sizeof(addr), "addresses are sockaddr_in");
    check(state == SM_Up || state == SM_Down, "state is SM_Up or SM_Down");
    check(rxp > 0 && in.sbq_Low > 0, "receive counters running");
}

static void sockets(LONG type, const char *what)
{
    LONG need = GetNetworkStatistics(type, NETWORKSTATUS_VERSION, NULL, 0), got, i;
    struct protocol_connection_data pcd[32];

    printf("  %s: %ld bytes (%ld entries)\n", what, (long)need,
           need > 0 ? (long)(need / sizeof(pcd[0])) : 0L);
    if (need < 0) {
        check(0, "GetNetworkStatistics size query");
        return;
    }
    got = GetNetworkStatistics(type, NETWORKSTATUS_VERSION, pcd, sizeof(pcd));
    for (i = 0; i < got / (LONG)sizeof(pcd[0]); i++)
        printf("    %s:%u -> %s:%u  state %ld  rxq %lu txq %lu\n",
               ip(pcd[i].pcd_local_address.s_addr), (unsigned)pcd[i].pcd_local_port,
               ip(pcd[i].pcd_foreign_address.s_addr), (unsigned)pcd[i].pcd_foreign_port,
               (long)pcd[i].pcd_tcp_state, (unsigned long)pcd[i].pcd_receive_queue_size,
               (unsigned long)pcd[i].pcd_send_queue_size);
}

int main(void)
{
    struct List *l;
    struct Node *n;
    LONG s, i, found = 0;
    char dom[64];
    struct sockaddr_in sin;

    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        printf("rsquery: no bsdsocket.library\n");
        return RETURN_FAIL;
    }
    printf("rsquery: %s\n", SocketBase->lib_IdString ? (char *)SocketBase->lib_IdString : "?");

    printf("[capabilities]\n");
    printf("  interface %ld, status %ld, dns %ld, routing %ld, monitoring %ld\n",
           (long)have(SBTC_HAVE_INTERFACE_API), (long)have(SBTC_HAVE_STATUS_API),
           (long)have(SBTC_HAVE_DNS_API), (long)have(SBTC_HAVE_ROUTING_API),
           (long)have(SBTC_HAVE_MONITORING_API));
    check(have(SBTC_HAVE_INTERFACE_API) == TRUE, "SBTC_HAVE_INTERFACE_API");
    check(have(SBTC_HAVE_STATUS_API) == TRUE, "SBTC_HAVE_STATUS_API");
    check(have(SBTC_HAVE_DNS_API) == TRUE, "SBTC_HAVE_DNS_API");

    printf("[interfaces]\n");
    l = ObtainInterfaceList();
    check(l != NULL, "ObtainInterfaceList");
    if (l) {
        for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ) {
            found++;
            query((STRPTR)n->ln_Name);
        }
        ReleaseInterfaceList(l);
    }
    check(found > 0, "at least one interface listed");
    check(QueryInterfaceTags((STRPTR)"nosuch9", IFQ_MTU, (ULONG)&i, TAG_END) == -1,
          "unknown interface fails");

    printf("[sockets]\n");
    s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = 4321;
    bind(s, (struct sockaddr *)&sin, sizeof(sin));
    listen(s, 1);
    sockets(NETSTATUS_tcp_sockets, "tcp sockets");
    {
        struct protocol_connection_data pcd[32];
        LONG got = GetNetworkStatistics(NETSTATUS_tcp_sockets, NETWORKSTATUS_VERSION, pcd, sizeof(pcd));
        int seen = 0;
        for (i = 0; i < got / (LONG)sizeof(pcd[0]); i++)
            if (pcd[i].pcd_local_port == 4321 && pcd[i].pcd_tcp_state == 1)
                seen = 1;
        check(seen, "our listener on port 4321 is listed, LISTEN");
    }
    sockets(NETSTATUS_udp_sockets, "udp sockets");
    CloseSocket(s);
    {
        /* raw ULONG views of struct ipstat (24 ULONGs) and tcpstat (52) */
        ULONG st[64];
        check(GetNetworkStatistics(NETSTATUS_ip, NETWORKSTATUS_VERSION, NULL, 0) == 24 * 4,
              "ipstat size query");
        check(GetNetworkStatistics(NETSTATUS_tcp, NETWORKSTATUS_VERSION, NULL, 0) == 52 * 4,
              "tcpstat size query");
        check(GetNetworkStatistics(NETSTATUS_ip, NETWORKSTATUS_VERSION, st, 8) == 8,
              "short buffer gets a partial copy");
        check(GetNetworkStatistics(NETSTATUS_ip, NETWORKSTATUS_VERSION, st, sizeof(st)) == 24 * 4,
              "ipstat filled");
        printf("  ip: total %lu delivered %lu out %lu noroute %lu\n",
               (unsigned long)st[0], (unsigned long)st[13], (unsigned long)st[14],
               (unsigned long)st[21]);
        check(GetNetworkStatistics(NETSTATUS_tcp, NETWORKSTATUS_VERSION, st, sizeof(st)) == 52 * 4,
              "tcpstat filled");
        printf("  tcp: attempts %lu accepts %lu connects %lu sent %lu rexmit %lu recv %lu\n",
               (unsigned long)st[0], (unsigned long)st[1], (unsigned long)st[2],
               (unsigned long)st[15], (unsigned long)st[18], (unsigned long)st[25]);
        check(GetNetworkStatistics(NETSTATUS_udp, NETWORKSTATUS_VERSION, st, sizeof(st)) == 9 * 4,
              "udpstat filled");
        check(GetNetworkStatistics(8, NETWORKSTATUS_VERSION, st, sizeof(st)) == -1,
              "unknown statistics type fails");
    }

    printf("[name servers]\n");
    l = ObtainDomainNameServerList();
    check(l != NULL, "ObtainDomainNameServerList");
    if (l) {
        found = 0;
        for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ) {
            struct DomainNameServerNode *d = (struct DomainNameServerNode *)n;
            printf("  %s (size %ld, use %ld)\n", (char *)d->dnsn_Address, (long)d->dnsn_Size,
                   (long)d->dnsn_UseCount);
            found++;
        }
        ReleaseDomainNameServerList(l);
        check(found > 0, "at least one name server");
    }
    i = GetDefaultDomainName((STRPTR)dom, sizeof(dom));
    printf("  default domain: %s\n", i ? dom : "(none)");

    printf("rsquery: %d failed -> %s\n", fails, fails ? "FAIL" : "PASS");
    CloseLibrary(SocketBase);
    return fails ? RETURN_WARN : RETURN_OK;
}
