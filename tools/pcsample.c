/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * pcsample - a sampling profiler for one or two tasks (developer tool).
 *
 *   pcsample TASK/A,TASK2,SECONDS/N,US/N,TO/A
 *
 * Wakes every US microseconds (default 2000) at priority 30. Each time a
 * watched task has just been pre-empted (it is ready, not waiting), the first
 * 24 longwords of its saved context are recorded: the saved PC is among them.
 * Records are kept in memory and written to TO at the end, one per sample:
 * a longword with the task number (1 or 2) in the top byte, 0 for "neither
 * ready", then the 24 longwords when there is a record. TASK2 may end in *
 * to match the first task whose name starts with the rest (e.g. nh_exec*).
 * tools/pcsample.py on the host finds the PC and maps it onto the symbols.
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#define NL 24
extern struct ExecBase *SysBase;

/* a task whose name starts with prefix, in the ready or waiting list */
static struct Task *find_prefix(const char *prefix, int len)
{
    struct List *lists[2];
    struct Node *n;
    int k;
    lists[0] = &SysBase->TaskReady;
    lists[1] = &SysBase->TaskWait;
    for (k = 0; k < 2; k++)
        for (n = lists[k]->lh_Head; n->ln_Succ; n = n->ln_Succ)
            if (n->ln_Name && !strncmp(n->ln_Name, prefix, len))
                return (struct Task *)n;
    return NULL;
}

int main(void)
{
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"TASK/A,TASK2,SECONDS/N,US/N,TO/A", args, NULL);
    struct MsgPort *port = NULL;
    struct timerequest *tr = NULL;
    struct Task *t[2] = { NULL, NULL }, *me = FindTask(NULL);
    ULONG secs, us, n, i, cnt[3] = { 0, 0, 0 };
    ULONG *buf = NULL, *p, bufsize;
    BPTR f = 0;
    BYTE oldpri;
    int rc = RETURN_FAIL, k;

    if (!rda) { PrintFault(IoErr(), "pcsample"); return RETURN_FAIL; }
    secs = args[2] ? *(LONG *)args[2] : 10;
    us = args[3] ? *(LONG *)args[3] : 2000;
    n = secs * (1000000 / us);
    bufsize = n * 2 * (NL + 1) * 4;
    if (!(buf = AllocVec(bufsize, MEMF_ANY))) { Printf("pcsample: no memory for %lu bytes\n", bufsize); goto out; }
    Forbid();
    t[0] = FindTask((STRPTR)args[0]);
    if (args[1]) {
        const char *s = (const char *)args[1];
        int l = strlen(s);
        t[1] = (l && s[l - 1] == '*') ? find_prefix(s, l - 1) : FindTask((STRPTR)s);
    }
    Permit();
    if (!t[0]) { Printf("pcsample: no task %s\n", args[0]); goto out; }
    if (args[1] && !t[1]) { Printf("pcsample: no task %s\n", args[1]); goto out; }
    if (!(port = CreateMsgPort())) goto out;
    if (!(tr = (struct timerequest *)CreateIORequest(port, sizeof(*tr)))) goto out;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0)) { tr->tr_node.io_Device = NULL; goto out; }
    Printf("pcsample: %s at %08lx", args[0], (ULONG)t[0]);
    if (t[1]) Printf(", %s at %08lx", (ULONG)t[1]->tc_Node.ln_Name, (ULONG)t[1]);
    Printf(", %lu s every %lu us\n", secs, us);
    oldpri = SetTaskPri(me, 30);
    p = buf;
    for (i = 0; i < n; i++) {
        int any = 0;
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = us;
        DoIO((struct IORequest *)tr);
        Disable();
        for (k = 0; k < 2; k++)
            if (t[k] && t[k]->tc_State == TS_READY) {
                *p++ = (ULONG)(k + 1) << 24;
                memcpy(p, t[k]->tc_SPReg, NL * 4);
                p += NL;
                cnt[k + 1]++;
                any = 1;
            }
        Enable();
        if (!any) { *p++ = 0; cnt[0]++; }
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) break;
    }
    SetTaskPri(me, oldpri);
    if (!(f = Open((STRPTR)args[4], MODE_NEWFILE))) goto out;
    Write(f, buf, (UBYTE *)p - (UBYTE *)buf);
    Printf("pcsample: %lu ticks: task 1 ready %lu, task 2 ready %lu, neither %lu\n", i, cnt[1], cnt[2], cnt[0]);
    rc = RETURN_OK;
out:
    if (f) Close(f);
    if (tr) { if (tr->tr_node.io_Device) CloseDevice((struct IORequest *)tr); DeleteIORequest((struct IORequest *)tr); }
    if (port) DeleteMsgPort(port);
    if (buf) FreeVec(buf);
    FreeArgs(rda);
    return rc;
}
