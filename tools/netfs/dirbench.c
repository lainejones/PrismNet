/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * dirbench - how long a folder takes to show, the way Workbench reads it
 * (developer tool for the share handlers).
 *
 *   dirbench DIR/A,ROUNDS/N,RAW/S
 *
 * RAW: instead of icon.library, open, read and close each .info file and
 * Lock + Examine each other entry - the file system's share of the work.
 *
 * Text view: Examine + ExNext over the folder. Icon view: the same, then
 * icon.library GetDiskObjectNew() for every entry that is not itself an
 * .info - Workbench's own call, which reads the .info or, without one,
 * finds a default icon for the file's type. Prints milliseconds per round.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <workbench/workbench.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/timer.h>
#include <string.h>

struct Library *IconBase;
struct Device *TimerBase;
static ULONG efreq;

static ULONG now_ms(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return (ULONG)(((unsigned long long)e.ev_hi << 32 | e.ev_lo) * 1000ULL / efreq);
}

#define MAXN 1024
static char names[MAXN][108];

/* list the folder; returns the number of entries, -1 on failure */
static LONG scan(const char *dir)
{
    BPTR l = Lock((STRPTR)dir, ACCESS_READ);
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    LONG n = -1;
    if (l && fib && Examine(l, fib)) {
        n = 0;
        while (ExNext(l, fib))
            if (n < MAXN) strcpy(names[n++], fib->fib_FileName);
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (l) UnLock(l);
    return n;
}

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DIR/A,ROUNDS/N,RAW/S", args, NULL);
    static UBYTE rbuf[8192];
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    struct timerequest tr;
    ULONG rounds, r, t0, t1, t2;
    static char path[512];

    if (!rda) { PrintFault(IoErr(), "dirbench"); return RETURN_FAIL; }
    rounds = args[1] ? *(LONG *)args[1] : 3;
    if (!(IconBase = OpenLibrary((STRPTR)"icon.library", 44))) { Printf("dirbench: needs icon.library 44\n"); return RETURN_FAIL; }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) return RETURN_FAIL;
    TimerBase = tr.tr_node.io_Device;
    { struct EClockVal e; efreq = ReadEClock(&e); }
    for (r = 1; r <= rounds; r++) {
        LONG n, i, icons = 0, got = 0;
        t0 = now_ms();
        n = scan((const char *)args[0]);
        t1 = now_ms();
        if (n < 0) { Printf("dirbench: can't read %s\n", args[0]); break; }
        for (i = 0; i < n; i++) {
            int l = strlen(names[i]);
            struct DiskObject *d;
            int isinfo = l > 5 && !stricmp(names[i] + l - 5, ".info");
            strcpy(path, (const char *)args[0]);
            AddPart((STRPTR)path, (STRPTR)names[i], sizeof(path));
            if (args[2]) {                      /* RAW */
                icons++;
                if (isinfo) {
                    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
                    if (f) { while (Read(f, rbuf, sizeof(rbuf)) > 0) ; Close(f); got++; }
                } else {
                    BPTR k = Lock((STRPTR)path, ACCESS_READ);
                    if (k) { if (fib && Examine(k, fib)) got++; UnLock(k); }
                }
                continue;
            }
            if (isinfo) continue;
            icons++;
            if ((d = GetDiskObjectNew((STRPTR)path))) { got++; FreeDiskObject(d); }
        }
        t2 = now_ms();
        Printf("round %lu: %ld entries, text view %lu ms; icons for %ld (%ld found) +%lu ms = icon view %lu ms\n",
               r, n, t1 - t0, icons, got, t2 - t1, t2 - t0);
    }
    CloseDevice((struct IORequest *)&tr);
    if (fib) FreeDosObject(DOS_FIB, fib);
    CloseLibrary(IconBase);
    FreeArgs(rda);
    return 0;
}
