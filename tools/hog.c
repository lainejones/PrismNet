/*
 * hog - keep the CPU busy at a given priority for a few seconds, so tasks
 * below it see their input queue up (test helper for GUI message races).
 *   hog SECS/N PRI/N
 * GPL-3.0-only, see LICENSE.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"SECS/N,PRI/N", args, NULL);
    LONG secs = args[0] ? *(LONG *)args[0] : 5, pri = args[1] ? *(LONG *)args[1] : 1;
    struct DateStamp a, b;
    volatile ULONG x = 0;

    if (!rda) return 20;
    SetTaskPri(FindTask(NULL), pri);
    DateStamp(&a);
    for (;;) {
        ULONG i;
        for (i = 0; i < 20000; i++) x += i;
        DateStamp(&b);
        if ((b.ds_Days - a.ds_Days) * 86400L + (b.ds_Minute - a.ds_Minute) * 60L +
            (b.ds_Tick - a.ds_Tick) / 50L >= secs)
            break;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) break;
    }
    FreeArgs(rda);
    return 0;
}
