/*
 * PrismNet core: protocol statistics in Roadshow's GetNetworkStatistics()
 * layouts (the BSD structs: icmpstat, igmpstat, ipstat, mbstat, mrtstat,
 * rtstat, tcpstat, udpstat), built from lwIP's counters (LWIP_STATS with
 * MIB2_STATS). Fields lwIP does not count stay 0.
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "lwip/opt.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/prot/icmp.h"

#include "prismnet/pnmem.h"
#include "core.h"

/* Roadshow's NETSTATUS_* types */
enum { NS_ICMP, NS_IGMP, NS_IP, NS_MB, NS_MRT, NS_RT, NS_TCP, NS_UDP };

#define ICMP_MAXTYPE_ 18

struct rs_icmpstat {
    ULONG icps_error, icps_oldshort, icps_oldicmp, icps_outhist[ICMP_MAXTYPE_ + 1];
    ULONG icps_badcode, icps_tooshort, icps_checksum, icps_badlen, icps_reflect;
    ULONG icps_inhist[ICMP_MAXTYPE_ + 1];
};
struct rs_igmpstat {
    ULONG rcv_total, rcv_tooshort, rcv_badsum, rcv_queries, rcv_badqueries;
    ULONG rcv_reports, rcv_badreports, rcv_ourreports, snd_reports;
};
struct rs_ipstat {
    ULONG total, badsum, tooshort, toosmall, badhlen, badlen, fragments, fragdropped;
    ULONG fragtimeout, forward, cantforward, redirectsent, noproto, delivered, localout;
    ULONG odropped, reassembled, fragmented, ofragments, cantfrag, badoptions, noroute;
    ULONG badvers, rawout;
};
struct rs_mbstat { ULONG mbufs, clusters, spare, clfree, drops, wait, drain; };
struct rs_mrtstat { ULONG v[8]; };
struct rs_rtstat { WORD badredirect, dynamic, newgateway, unreach, wildcard; };
struct rs_tcpstat {
    ULONG connattempt, accepts, connects, drops, conndrops, closed, segstimed, rttupdated;
    ULONG delack, timeoutdrop, rexmttimeo, persisttimeo, keeptimeo, keepprobe, keepdrops;
    ULONG sndtotal, sndpack, sndbyte, sndrexmitpack, sndrexmitbyte, sndacks, sndprobe;
    ULONG sndurg, sndwinup, sndctrl;
    ULONG rcvtotal, rcvpack, rcvbyte, rcvbadsum, rcvbadoff, rcvshort, rcvduppack;
    ULONG rcvdupbyte, rcvpartduppack, rcvpartdupbyte, rcvoopack, rcvoobyte;
    ULONG rcvpackafterwin, rcvbyteafterwin, rcvafterclose, rcvwinprobe, rcvdupack;
    ULONG rcvacktoomuch, rcvackpack, rcvackbyte, rcvwinupd, pawsdrop, predack, preddat;
    ULONG pcbcachemiss, persistdrop, badsyn;
};
struct rs_udpstat {
    ULONG ipackets, hdrops, badsum, badlen, noport, noportbcast, fullsock, pcbcachemiss;
    ULONG opackets;
};

static const UWORD sizes[] = {
    sizeof(struct rs_icmpstat), sizeof(struct rs_igmpstat), sizeof(struct rs_ipstat),
    sizeof(struct rs_mbstat), sizeof(struct rs_mrtstat), sizeof(struct rs_rtstat),
    sizeof(struct rs_tcpstat), sizeof(struct rs_udpstat)
};

/* Fill `dest` (up to `size` bytes) with statistics of `type`. NULL dest:
 * the size a complete copy needs. Returns bytes, or -1 for an unknown type. */
LONG core_netstats(LONG type, APTR dest, LONG size)
{
    union {
        struct rs_icmpstat icmp; struct rs_igmpstat igmp; struct rs_ipstat ip;
        struct rs_mbstat mb; struct rs_mrtstat mrt; struct rs_rtstat rt;
        struct rs_tcpstat tcp; struct rs_udpstat udp;
    } u;
    const struct stats_mib2 *m = &lwip_stats.mib2;
    LONG n;

    if (type < 0 || type > NS_UDP)
        return -1;
    n = sizes[type];
    if (!dest)
        return n;
    memset(&u, 0, sizeof(u));

    switch (type) {
    case NS_ICMP:
        u.icmp.icps_error    = m->icmpouterrors;
        u.icmp.icps_outhist[ICMP_ER] = m->icmpoutechoreps;
        u.icmp.icps_outhist[ICMP_DUR] = m->icmpoutdestunreachs;
        u.icmp.icps_outhist[ICMP_ECHO] = m->icmpoutechos;
        u.icmp.icps_outhist[ICMP_TE] = m->icmpouttimeexcds;
        u.icmp.icps_tooshort = lwip_stats.icmp.lenerr;
        u.icmp.icps_checksum = lwip_stats.icmp.chkerr;
        u.icmp.icps_reflect  = m->icmpoutechoreps;
        u.icmp.icps_inhist[ICMP_ER]   = m->icmpinechoreps;
        u.icmp.icps_inhist[ICMP_DUR]  = m->icmpindestunreachs;
        u.icmp.icps_inhist[ICMP_SQ]   = m->icmpinsrcquenchs;
        u.icmp.icps_inhist[ICMP_RD]   = m->icmpinredirects;
        u.icmp.icps_inhist[ICMP_ECHO] = m->icmpinechos;
        u.icmp.icps_inhist[ICMP_TE]   = m->icmpintimeexcds;
        u.icmp.icps_inhist[ICMP_PP]   = m->icmpinparmprobs;
        u.icmp.icps_inhist[ICMP_TS]   = m->icmpintimestamps;
        u.icmp.icps_inhist[ICMP_TSR]  = m->icmpintimestampreps;
        u.icmp.icps_inhist[ICMP_AM]   = m->icmpinaddrmasks;
        u.icmp.icps_inhist[ICMP_AMR]  = m->icmpinaddrmaskreps;
        break;
    case NS_IGMP:
#if LWIP_IGMP
        u.igmp.rcv_total    = lwip_stats.igmp.recv;
        u.igmp.rcv_tooshort = lwip_stats.igmp.lenerr;
        u.igmp.rcv_badsum   = lwip_stats.igmp.chkerr;
        u.igmp.rcv_queries  = lwip_stats.igmp.rx_general + lwip_stats.igmp.rx_group +
                              lwip_stats.igmp.rx_v1;
        u.igmp.rcv_badqueries = lwip_stats.igmp.proterr;
        u.igmp.rcv_reports  = lwip_stats.igmp.rx_report;
        u.igmp.snd_reports  = lwip_stats.igmp.tx_report + lwip_stats.igmp.tx_join;
#endif
        break;
    case NS_IP:
        u.ip.total       = m->ipinreceives;
        u.ip.badsum      = lwip_stats.ip.chkerr;
        u.ip.tooshort    = lwip_stats.ip.lenerr;
        u.ip.badhlen     = m->ipinhdrerrors;
        u.ip.fragments   = m->ipreasmreqds;
        u.ip.fragdropped = m->ipreasmfails;
        u.ip.forward     = m->ipforwdatagrams;
        u.ip.cantforward = m->ipinaddrerrors;
        u.ip.noproto     = m->ipinunknownprotos;
        u.ip.delivered   = m->ipindelivers;
        u.ip.localout    = m->ipoutrequests;
        u.ip.odropped    = m->ipoutdiscards;
        u.ip.reassembled = m->ipreasmoks;
        u.ip.fragmented  = m->ipfragoks;
        u.ip.ofragments  = m->ipfragcreates;
        u.ip.cantfrag    = m->ipfragfails;
        u.ip.badoptions  = lwip_stats.ip.opterr;
        u.ip.noroute     = m->ipoutnoroutes;
        break;
    case NS_MB: {
        /* the pbuf pool plays the part of mbuf clusters */
        const struct stats_mem *pool = lwip_stats.memp[MEMP_PBUF_POOL];
        u.mb.mbufs    = pool->used;
        u.mb.clusters = pool->avail;
        u.mb.clfree   = pool->avail - pool->used;
        u.mb.drops    = pool->err + pn_mem.fails;
        break;
    }
    case NS_RT:
        u.rt.unreach = (WORD)m->ipoutnoroutes;
        break;
    case NS_TCP:
        u.tcp.connattempt   = m->tcpactiveopens;
        u.tcp.accepts       = m->tcppassiveopens;
        u.tcp.connects      = m->tcpactiveopens + m->tcppassiveopens - m->tcpattemptfails;
        u.tcp.drops         = m->tcpestabresets;
        u.tcp.conndrops     = m->tcpattemptfails;
        u.tcp.sndtotal      = m->tcpoutsegs;
        u.tcp.sndrexmitpack = m->tcpretranssegs;
        u.tcp.sndctrl       = m->tcpoutrsts;
        u.tcp.rcvtotal      = m->tcpinsegs;
        u.tcp.rcvbadsum     = lwip_stats.tcp.chkerr;
        u.tcp.rcvshort      = lwip_stats.tcp.lenerr;
        break;
    case NS_UDP:
        u.udp.ipackets = m->udpindatagrams + m->udpnoports + m->udpinerrors;
        u.udp.hdrops   = lwip_stats.udp.lenerr;
        u.udp.badsum   = lwip_stats.udp.chkerr;
        u.udp.noport   = m->udpnoports;
        u.udp.opackets = m->udpoutdatagrams;
        break;
    }
    if (size < n)
        n = size > 0 ? size : 0;
    memcpy(dest, &u, n);
    return n;
}

/* Bytes of code, data and BSS the core was loaded with: the segment sizes of
 * this process's command (the core runs as "PrismNet RUN", a CLI process). */
static ULONG image_size(void)
{
    static ULONG size;
    struct CommandLineInterface *cli;
    BPTR seg;

    if (size || !(cli = Cli()))
        return size;
    for (seg = cli->cli_Module; seg; seg = *(BPTR *)BADDR(seg))
        size += ((ULONG *)BADDR(seg))[-1];  /* hunk length, header included */
    return size;
}

/* Stack high-water mark: the unused stack is filled with a pattern at start-up
 * and STATUS reports how far down it has been overwritten. */
#define STACK_FILL 0xA5A5A5A5UL

ULONG core_stack_mark(void)
{
    struct Task *me = FindTask(NULL);
    volatile ULONG here;
    ULONG *p   = (ULONG *)(((ULONG)me->tc_SPLower + 3) & ~3UL);
    ULONG *end = (ULONG *)(((ULONG)&here - 256) & ~3UL);    /* stay clear of our frame */

    while (p < end)
        *p++ = STACK_FILL;
    return (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
}

static ULONG stack_peak(void)
{
    struct Task *me = FindTask(NULL);
    ULONG *p = (ULONG *)(((ULONG)me->tc_SPLower + 3) & ~3UL);

    while ((APTR)p < me->tc_SPUpper && *p == STACK_FILL)
        p++;
    return (ULONG)me->tc_SPUpper - (ULONG)p;
}

static void pool(const struct stats_mem *st, UWORD *max, UWORD *used, UWORD *peak)
{
    *max = (UWORD)st->avail;
    *used = (UWORD)st->used;
    *peak = (UWORD)st->max;
}

void core_memstats(struct PnMemStatus *m)
{
    const struct stats_mem *pb = lwip_stats.memp[MEMP_PBUF_POOL];

    memset(m, 0, sizeof(*m));
    m->image      = image_size();
    {
        struct Task *me = FindTask(NULL);
        m->stack = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    }
    m->stack_peak = stack_peak();
    m->dyn_used   = pn_mem_used;
    m->dyn_peak   = pn_mem_peak;
    m->budget     = pn_mem.budget;
    m->mem_used   = pn_mem.held - pn_mem.cached;
    m->mem_peak   = pn_mem.peak;
    m->mem_fails  = pn_mem.fails;
    m->mem_cached = pn_mem.cached;
    m->pool_max   = pb->avail;
    m->pool_used  = pb->used;
    m->pool_peak  = pb->max;
    m->pool_fails = pb->err;
    pool(lwip_stats.memp[MEMP_TCP_PCB], &m->tcp_max, &m->tcp_used, &m->tcp_peak);
    pool(lwip_stats.memp[MEMP_TCP_SEG], &m->seg_max, &m->seg_used, &m->seg_peak);
    pool(lwip_stats.memp[MEMP_UDP_PCB], &m->udp_max, &m->udp_used, &m->udp_peak);
    m->sys_free    = AvailMem(MEMF_ANY);
    m->sys_chip    = AvailMem(MEMF_CHIP);
    m->sys_largest = AvailMem(MEMF_ANY | MEMF_LARGEST);
}
