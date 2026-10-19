/*
 * walk - depth-first directory walk that keeps every parent's ExNext() in
 * progress while it descends (the way recursive scanners do, unlike C:List
 * which reads a whole directory before recursing). Counts entries and stops
 * if it sees more than LIMIT, which is how a listing that restarts shows up.
 *   walk DIR/A,LIMIT/N
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

static LONG limit = 2000, files, dirs, depth_max;
static int stopped;

static void walk(BPTR lock, int depth)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) return;
    if (depth > depth_max) depth_max = depth;
    if (Examine(lock, fib)) {
        while (!stopped && ExNext(lock, fib)) {
            if (fib->fib_DirEntryType > 0) {
                BPTR sub;
                dirs++;
                if (depth < 16) {
                    BPTR old = CurrentDir(lock);
                    sub = Lock((CONST_STRPTR)fib->fib_FileName, SHARED_LOCK);
                    CurrentDir(old);
                    if (sub) { BPTR o2 = CurrentDir(sub); walk(sub, depth + 1); CurrentDir(o2); UnLock(sub); }
                    else printf("walk: cannot lock %s (%ld)\n", fib->fib_FileName, (long)IoErr());
                }
            } else
                files++;
            if (files + dirs > limit) { stopped = 1; break; }
        }
    }
    FreeDosObject(DOS_FIB, fib);
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DIR/A,LIMIT/N", args, NULL);
    BPTR lock, old;

    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"walk"); return 20; }
    if (args[1]) limit = *(LONG *)args[1];
    if (!(lock = Lock((CONST_STRPTR)args[0], SHARED_LOCK))) {
        printf("walk: cannot lock %s (%ld)\n", (const char *)args[0], (long)IoErr());
        FreeArgs(rda); return 10;
    }
    old = CurrentDir(lock);
    walk(lock, 1);
    CurrentDir(old);
    UnLock(lock);
    printf("%s: %ld files, %ld directories, depth %ld%s\n", (const char *)args[0], (long)files,
           (long)dirs, (long)depth_max, stopped ? " - STOPPED: more entries than LIMIT, the listing repeats" : "");
    FreeArgs(rda);
    return stopped ? 5 : 0;
}
