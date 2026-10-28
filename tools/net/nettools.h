/*
 * PrismNet network tools - shared helpers (ping, traceroute, nslookup).
 * GPL-3.0-only, see LICENSE.
 *
 * Plain bsdsocket.library clients: they work with any AmiTCP-compatible
 * stack (PrismNet, Roadshow, AmiTCP, Miami).
 */
#ifndef PRISMNET_NETTOOLS_H
#define PRISMNET_NETTOOLS_H

#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

/* No __stack: libnix's stack swap hangs on exit here. Big buffers are static. */

/* The tools handle Ctrl-C themselves (WaitSelect / SetSignal) and must close
 * their socket and timer on the way out: no libnix abort from inside printf. */
void __chkabort(void) {}

struct Device *TimerBase = NULL;
static struct timerequest nt_treq;
static ULONG nt_freq;

/* E-clock based microsecond clock (needs timer.device). */
static __attribute__((unused)) BOOL nt_clock_init(void)
{
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)&nt_treq, 0) != 0)
        return FALSE;
    TimerBase = nt_treq.tr_node.io_Device;
    {
        struct EClockVal ev;
        nt_freq = ReadEClock(&ev);
    }
    return TRUE;
}

static __attribute__((unused)) void nt_clock_exit(void)
{
    if (TimerBase)
        CloseDevice((struct IORequest *)&nt_treq);
    TimerBase = NULL;
}

static __attribute__((unused)) unsigned long long nt_now_us(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((((unsigned long long)ev.ev_hi << 32) | ev.ev_lo) * 1000000ULL) / nt_freq;
}

/* "12.3" style milliseconds from microseconds into buf. */
static __attribute__((unused)) char *nt_ms(unsigned long long us, char *buf)
{
    unsigned long ms = (unsigned long)(us / 1000), tenth = (unsigned long)((us % 1000) / 100);
    sprintf(buf, "%lu.%lu", ms, tenth);
    return buf;
}

/* Internet checksum. */
static __attribute__((unused)) UWORD nt_cksum(const UBYTE *p, int len)
{
    ULONG sum = 0;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (UWORD)~sum;
}

static void nt_ipstr(ULONG a, char *out)
{
    sprintf(out, "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24) & 255, (unsigned long)(a >> 16) & 255,
            (unsigned long)(a >> 8) & 255, (unsigned long)a & 255);
}

#endif
