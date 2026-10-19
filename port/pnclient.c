/*
 * PrismNet - helper for tools that talk to the core directly (see pnclient.h).
 * GPL-3.0-only, see LICENSE.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "prismnet/pnclient.h"

BOOL pn_core_running(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((CONST_STRPTR)PN_PORTNAME);
    Permit();
    return p != NULL;
}

LONG pn_core_request(UWORD op, ULONG a0, ULONG a1, ULONG a2, LONG ticks)
{
    struct MsgPort *core, *reply = CreateMsgPort();
    /* Heap, not stack: if the core never answers the request is leaked. */
    struct PnReq *r = AllocVec(sizeof(*r), MEMF_PUBLIC | MEMF_CLEAR);
    LONG t, result;

    if (!reply || !r) {
        FreeVec(r);
        if (reply) DeleteMsgPort(reply);
        return PNC_TIMEOUT;
    }
    r->msg.mn_ReplyPort = reply;
    r->msg.mn_Length    = sizeof(*r);
    r->op   = op;
    r->a[0] = a0; r->a[1] = a1; r->a[2] = a2;
    Forbid();
    if ((core = FindPort((CONST_STRPTR)PN_PORTNAME)) != NULL)
        PutMsg(core, &r->msg);
    Permit();
    if (!core) {
        FreeVec(r);
        DeleteMsgPort(reply);
        return PNC_NOT_RUNNING;
    }
    for (t = 0; !GetMsg(reply); t++) {
        if (t >= ticks) {
            /* r and reply are deliberately leaked; the reply, when it comes,
             * must only be linked in, not Signal() a task that has exited. */
            Forbid();
            reply->mp_Flags   = PA_IGNORE;
            reply->mp_SigTask = NULL;
            Permit();
            return PNC_TIMEOUT;
        }
        Delay(1);
    }
    result = r->result;
    FreeVec(r);
    DeleteMsgPort(reply);
    return result;
}
