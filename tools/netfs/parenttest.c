/*
 * parenttest - ParentDir / NameFromLock / NameFromFH on a file of a network
 * share (PrismNet netfs audit, 2026-10-18).
 *   parenttest FILE/A
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(void)
{
    LONG args[1] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"FILE/A", args, NULL);
    const char *file = (const char *)args[0];
    char name[256];
    BPTR lk, p, fh;
    int fails = 0;

    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"parenttest"); return 20; }
    lk = Lock((CONST_STRPTR)file, SHARED_LOCK);
    if (!lk) { printf("FAIL Lock %s: %ld\n", file, IoErr()); FreeArgs(rda); return 10; }
    p = ParentDir(lk);
    printf("%s ParentDir(file lock): %s (IoErr %ld)\n", p ? "ok  " : "FAIL", p ? "lock" : "NULL", IoErr());
    if (!p) fails++;
    if (p) {
        name[0] = 0;
        if (NameFromLock(p, (STRPTR)name, sizeof(name))) printf("ok   parent is %s\n", name);
        else { printf("FAIL NameFromLock(parent): %ld\n", IoErr()); fails++; }
        UnLock(p);
    }
    name[0] = 0;
    if (NameFromLock(lk, (STRPTR)name, sizeof(name))) printf("ok   NameFromLock(file): %s\n", name);
    else { printf("FAIL NameFromLock(file): %ld\n", IoErr()); fails++; }
    UnLock(lk);
    fh = Open((CONST_STRPTR)file, MODE_OLDFILE);
    if (fh) {
        name[0] = 0;
        if (NameFromFH(fh, (STRPTR)name, sizeof(name))) printf("ok   NameFromFH: %s\n", name);
        else { printf("FAIL NameFromFH: %ld\n", IoErr()); fails++; }
        p = ParentOfFH(fh);
        printf("%s ParentOfFH: %s (IoErr %ld)\n", p ? "ok  " : "FAIL", p ? "lock" : "NULL", IoErr());
        if (p) UnLock(p); else fails++;
        Close(fh);
    } else { printf("FAIL Open: %ld\n", IoErr()); fails++; }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    FreeArgs(rda);
    return fails ? 5 : 0;
}
