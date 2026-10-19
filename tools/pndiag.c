/*
 * pndiag - read the core's loop state without waking it (test builds).
 * Part of PrismNet. GPL-3.0-only, see LICENSE.
 *
 * A core built with "make diag" (PN_DIAGPORT) publishes its main-loop state
 * in a public port "PrismNet.diag". Asking the core itself (PrismNet STATUS,
 * LOG) wakes it, which can hide a lost-wakeup bug; this only reads memory.
 */
#include <stdio.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <proto/exec.h>

/* keep in step with struct PnDiag in core/core.c */
struct PnDiag {
    struct MsgPort mp;
    ULONG magic, passes, stage, waitmask, sigs, timed, timer_busy, timer_due, now,
          msgs, pending, txbusy, rxbusy, lastop, polls;
};

int main(void)
{
    struct PnDiag d, *p;

    Forbid();
    p = (struct PnDiag *)FindPort((CONST_STRPTR)"PrismNet.diag");
    if (p)
        CopyMem(p, &d, sizeof(d));
    Permit();
    if (!p) {
        printf("pndiag: no PrismNet.diag port (core not running, or not a diag build)\n");
        return 5;
    }
    printf("pass %lu %s wait %08lx sigs %08lx timed %lu timer %s due %lu now %lu "
           "msgs %lu pending %lu tx %lu rx %lu lastop %lu\n",
           (unsigned long)d.passes, d.stage == 2 ? "waiting" : "running",
           (unsigned long)d.waitmask, (unsigned long)d.sigs, (unsigned long)d.timed,
           d.timer_busy ? "armed" : "idle", (unsigned long)d.timer_due, (unsigned long)d.now,
           (unsigned long)d.msgs, (unsigned long)d.pending, (unsigned long)d.txbusy,
           (unsigned long)d.rxbusy, (unsigned long)d.lastop);
    return 0;
}
