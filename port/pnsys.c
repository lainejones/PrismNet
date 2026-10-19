/*
 * PrismNet - Exec glue for lwIP: time base and the core task's wakeup timer.
 * GPL-3.0-only, see LICENSE.
 *
 * sys_now() is derived from ReadEClock(), which is monotonic and available
 * from OS 2.0, so lwIP timers keep working across clock changes.
 *
 * The clock is read at most once per wakeup: the first sys_now() after a
 * Wait (pn_timer_fired() marks it stale) reads it, later calls in the same
 * pass get that value. lwIP calls sys_now() several times per pass and an
 * E-clock read (CIA access under Disable) costs ~40 us on a 68030; within one
 * pass the time may as well stand still, and a pass that needs no time (a
 * bare client request) reads no clock at all. The conversion only uses the
 * delta since the previous read, in 32-bit arithmetic; the wakeup timer makes
 * the core read it at least every 250 ms, far below the ~6000 s it takes the
 * low E-clock word to wrap.
 */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/var.h>
#include <proto/timer.h>

#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "prismnet/lwiphooks.h"
#include "prismnet/pnsys.h"

struct Device *TimerBase;

static struct MsgPort     *timer_port;
static struct timerequest *timer_req;   /* wakeup request */
static BOOL                timer_busy;
static u32_t               timer_due;   /* sys_now() when the wakeup fires */
static ULONG               eclock_freq;
static ULONG               eclock_last; /* low E-clock word at the last sys_now() */
static u32_t               now_ms;
static ULONG               now_frac;    /* leftover ticks * 1000, < eclock_freq */
static ULONG               eclock_inv;  /* 2^32 / eclock_freq */
static BOOL                clock_fresh; /* read since the last Wait() */
static BOOL                clock_held;  /* a direct call: no timer I/O from its task */
static struct timerequest *sys_req;     /* TR_GETSYSTIME / TR_SETSYSTIME, synchronous */
static unsigned long       rand_state;

static ULONG eclock_lo(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ev.ev_lo;
}

/* Test aid: ENV:PrismNet/testisn (hex) makes every connection start at that
 * sequence number, e.g. 7FF00000 to cross 2^31 after 1 MB (a stall seen in
 * other stacks on that crossing: BlitterStudio zz9000-firmware issue 29). */
static u32_t forced_isn;

static void read_forced_isn(void)
{
    char buf[16];
    if (GetVar((CONST_STRPTR)"PrismNet/testisn", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        const char *p = buf;
        u32_t v = 0;
        for (; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (u32_t)d;
        }
        forced_isn = v;
    }
}

BOOL pn_sys_init(void)
{
    read_forced_isn();
    timer_port = CreateMsgPort();
    if (!timer_port)
        return FALSE;
    timer_req = (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer_req));
    if (!timer_req)
        goto fail;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer_req, 0) != 0) {
        DeleteIORequest((struct IORequest *)timer_req);
        timer_req = NULL;
        goto fail;
    }
    TimerBase = timer_req->tr_node.io_Device;
    if ((sys_req = (struct timerequest *)CreateIORequest(timer_port, sizeof(*sys_req))) != NULL) {
        sys_req->tr_node.io_Device = timer_req->tr_node.io_Device;
        sys_req->tr_node.io_Unit   = timer_req->tr_node.io_Unit;
    }

    {
        struct EClockVal ev;
        eclock_freq = ReadEClock(&ev);
        eclock_last = ev.ev_lo;
        rand_state  = ev.ev_lo ^ (ev.ev_hi << 16) ^ 0x9E3779B9UL;
    }
    eclock_inv  = 0xffffffffUL / eclock_freq;
    now_ms      = 0;
    now_frac    = 0;
    if (rand_state == 0)
        rand_state = 0x2545F491UL;
    return TRUE;


fail:
    DeleteMsgPort(timer_port);
    timer_port = NULL;
    return FALSE;
}

void pn_sys_cleanup(void)
{
    if (timer_req) {
        if (timer_busy) {
            AbortIO((struct IORequest *)timer_req);
            WaitIO((struct IORequest *)timer_req);
            timer_busy = FALSE;
        }
        CloseDevice((struct IORequest *)timer_req);
        DeleteIORequest((struct IORequest *)timer_req);
        timer_req = NULL;
    }
    if (sys_req) {
        DeleteIORequest((struct IORequest *)sys_req);
        sys_req = NULL;
    }
    TimerBase = NULL;
    if (timer_port) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
    }
}

/* While a request runs in a client's task (direct call), sys_now() keeps the
 * time of the core's last pass: reading the clock means timer.device I/O on
 * the core's port, which only the core task may wait on. */
void pn_clock_hold(BOOL hold)
{
    clock_held = hold;
}

u32_t sys_now(void)
{
    if (!clock_fresh && !clock_held)
        pn_clock_update();
    return now_ms;
}

void pn_clock_update(void)
{
    ULONG lo = eclock_lo(), d = lo - eclock_last;

    clock_fresh = TRUE;
    eclock_last = lo;
    if (d >= eclock_freq) {             /* whole seconds first: d * 1000 must fit */
        ULONG secs = d / eclock_freq;
        now_ms += secs * 1000;
        d -= secs * eclock_freq;
    }
    now_frac += d * 1000;               /* < 1001 * eclock_freq (E-clock < 1 MHz) */
    if (now_frac >= eclock_freq) {
        /* now_frac / eclock_freq without a divide: a reciprocal estimate
         * that is never too high, then at most a step or two up. */
        ULONG ms = ((now_frac >> 16) * eclock_inv) >> 16;
        now_frac -= ms * eclock_freq;
        while (now_frac >= eclock_freq) {
            now_frac -= eclock_freq;
            ms++;
        }
        now_ms += ms;
    }
}

/* TCP initial sequence number: random, stirred with the connection's
 * 4-tuple and the clock so two connections never start alike. */
u32_t prismnet_tcp_isn(const ip_addr_t *local_ip, u16_t local_port,
                       const ip_addr_t *remote_ip, u16_t remote_port)
{
    u32_t h = ip4_addr_get_u32(ip_2_ip4(remote_ip)) ^ (ip4_addr_get_u32(ip_2_ip4(local_ip)) << 7);
    h ^= ((u32_t)local_port << 16) | remote_port;
    if (forced_isn)
        return forced_isn;                  /* test aid, see pn_sys_init */
    return (u32_t)prismnet_rand() ^ h ^ (sys_now() * 64u);
}

unsigned long prismnet_rand(void)
{
    /* xorshift32, re-stirred with the E-clock each call. */
    unsigned long x = rand_state ^ (clock_held ? now_ms * 2654435761UL : eclock_lo());
    if (x == 0)
        x = 0x2545F491UL;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rand_state = x;
    return x;
}

ULONG pn_timer_sigmask(void)
{
    return 1UL << timer_port->mp_SigBit;
}

void pn_timer_arm(u32_t ms)
{
    u32_t due = sys_now() + ms;

    if (timer_busy) {
        /* Already due no later: keep it. Aborting and re-sending a
         * timer.device request on every pass of the core loop cost more
         * than the loop's real work. */
        if ((LONG)(timer_due - due) <= 0)
            return;
        AbortIO((struct IORequest *)timer_req);
        WaitIO((struct IORequest *)timer_req);
    }
    timer_due = due;
    timer_req->tr_node.io_Command = TR_ADDREQUEST;
    timer_req->tr_time.tv_secs    = ms / 1000;
    timer_req->tr_time.tv_micro   = (ms % 1000) * 1000;
    SetSignal(0, pn_timer_sigmask());
    SendIO((struct IORequest *)timer_req);
    timer_busy = TRUE;
}

BOOL pn_timer_armed(void)
{
    return timer_busy;
}

u32_t pn_timer_due(void)
{
    return timer_due;
}

BOOL pn_timer_fired(void)
{
    clock_fresh = FALSE;
    if (timer_busy && CheckIO((struct IORequest *)timer_req)) {
        WaitIO((struct IORequest *)timer_req);
        timer_busy = FALSE;
        return TRUE;
    }
    return FALSE;
}

BOOL pn_systime(BOOL set, ULONG *secs, ULONG *micros)
{
    if (!sys_req)
        return FALSE;
    if (set) {
        sys_req->tr_time.tv_secs  = *secs;
        sys_req->tr_time.tv_micro = *micros;
        sys_req->tr_node.io_Command = TR_SETSYSTIME;
    } else
        sys_req->tr_node.io_Command = TR_GETSYSTIME;
    if (DoIO((struct IORequest *)sys_req) != 0)
        return FALSE;
    *secs = sys_req->tr_time.tv_secs;
    *micros = sys_req->tr_time.tv_micro;
    return TRUE;
}
