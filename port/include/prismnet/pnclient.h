/*
 * PrismNet - helper for tools that talk to the core directly (no library):
 * PrismNet STATUS/LOG/NETSTAT, PrismMon.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_PNCLIENT_H
#define PRISMNET_PNCLIENT_H

#include <exec/types.h>
#include "prismnet/pnmsg.h"

#define PNC_NOT_RUNNING  (-1000)
#define PNC_TIMEOUT      (-1001)

BOOL pn_core_running(void);

/*
 * Send one request (op, a0..a2) to the core and wait up to `ticks` (1/50 s).
 * Returns the core's result, PNC_NOT_RUNNING or PNC_TIMEOUT. On a timeout the
 * request is leaked on purpose and any buffer passed in a0..a2 must be leaked
 * too (the core may still write to it later).
 */
LONG pn_core_request(UWORD op, ULONG a0, ULONG a1, ULONG a2, LONG ticks);

#endif
