/*
 * PrismNet - Exec glue for lwIP: time base and the core task's wakeup timer.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_PNSYS_H
#define PRISMNET_PNSYS_H

#include <exec/types.h>
#include "lwip/arch.h"

/* Open timer.device (E-clock time base + one wakeup request). */
BOOL  pn_sys_init(void);
void  pn_sys_cleanup(void);

/* Signal bit set when the wakeup timer fires. */
ULONG pn_timer_sigmask(void);
/* (Re)arm the wakeup timer to fire in `ms` milliseconds. */
void  pn_timer_arm(u32_t ms);
/* Call after every Wait(): marks the clock sys_now() returns stale (the next
 * sys_now() re-reads it) and collects a fired timer; TRUE if it had fired. */
BOOL  pn_timer_fired(void);
/* TRUE while the wakeup timer is pending; when it is due (sys_now() time). */
BOOL  pn_timer_armed(void);
u32_t pn_timer_due(void);
/* Re-read the clock now (sys_now() does this when it is stale). */
void  pn_clock_update(void);
void  pn_clock_hold(BOOL hold);     /* direct calls: no clock reads meanwhile */
/* the system clock (timer.device, seconds since 1978 local time): read it,
 * and set it when set is TRUE */
BOOL  pn_systime(BOOL set, ULONG *secs, ULONG *micros);

#endif
