/*
 * nfstest - drive a filesystem through DOS calls one step at a time and
 * print what comes back, to compare PrismNet's network handlers with
 * another (e.g. AmiNFSv3, smb2fs) on the same share.
 *   nfstest DIR=PN0: [FILE=name] [PARENT] [MD5] [BUF=bytes]
 * MD5 prints the file's MD5 instead of the byte sum; BUF sets the Read size
 * (default 4096). GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "crypto.h"

static void show_fib(const char *what, struct FileInfoBlock *fib)
{
    printf("%-8s %-30s %s size %ld prot %08lx key %08lx days %ld\n", what,
           fib->fib_FileName, fib->fib_DirEntryType > 0 ? "dir " : "file",
           (long)fib->fib_Size, (unsigned long)fib->fib_Protection,
           (unsigned long)fib->fib_DiskKey, (long)fib->fib_Date.ds_Days);
}

int main(void)
{
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DIR/A,FILE/K,PARENT/S,MD5/S,BUF/K/N", args, NULL);
    struct FileInfoBlock *fib;
    BPTR lock, fh;
    int n = 0;

    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"nfstest"); return 20; }
    fib = AllocDosObject(DOS_FIB, NULL);
    printf("Lock %s\n", (char *)args[0]); fflush(stdout);
    if (!(lock = Lock((CONST_STRPTR)args[0], SHARED_LOCK))) {
        PrintFault(IoErr(), (CONST_STRPTR)"Lock"); return 20;
    }
    printf("Examine\n"); fflush(stdout);
    if (!Examine(lock, fib)) { PrintFault(IoErr(), (CONST_STRPTR)"Examine"); return 20; }
    show_fib("examine", fib); fflush(stdout);
    if (!args[3]) {                     /* MD5 runs skip the listing */
        while (ExNext(lock, fib)) {
            show_fib("exnext", fib); fflush(stdout);
            if (++n > 500) break;
        }
        printf("ExNext ended: %ld (232 = no more entries)\n", (long)IoErr()); fflush(stdout);
    }
    if (args[2]) {
        BPTR p = ParentDir(lock);
        printf("ParentDir: %s\n", p ? "a lock" : "none (root)");
        if (p) UnLock(p);
    }
    if (args[1]) {
        BPTR old = CurrentDir(lock);
        static UBYTE sbuf[4096];
        UBYTE *buf = sbuf;
        LONG got, total = 0, sum = 0, i, bsize = args[4] ? *(LONG *)args[4] : 4096;
        struct Md5 md;

        if (bsize > 4096 && !(buf = AllocVec(bsize, 0))) { buf = sbuf; bsize = 4096; }
        md5_init(&md);
        printf("Open %s\n", (char *)args[1]); fflush(stdout);
        if ((fh = Open((CONST_STRPTR)args[1], MODE_OLDFILE))) {
            while ((got = Read(fh, buf, bsize)) > 0) {
                if (args[3]) md5_update(&md, buf, got);
                else for (i = 0; i < got; i++) sum += buf[i];
                total += got;
            }
            printf("read %lu bytes, byte sum %ld, last Read %ld\n",
                   (unsigned long)total, (long)sum, (long)got);
            if (args[3]) {
                UBYTE d[16];
                md5_final(&md, d);
                printf("md5 ");
                for (i = 0; i < 16; i++) printf("%02x", d[i]);
                printf("\n");
            }
            if (ExamineFH(fh, fib)) show_fib("examfh", fib);
            Seek(fh, 0, OFFSET_END);
            printf("Seek end -> %ld\n", (long)Seek(fh, 0, OFFSET_BEGINNING));
            Close(fh);
        } else {
            PrintFault(IoErr(), (CONST_STRPTR)"Open");
        }
        if (buf != sbuf) FreeVec(buf);
        CurrentDir(old);
    }
    UnLock(lock);
    FreeDosObject(DOS_FIB, fib);
    FreeArgs(rda);
    return 0;
}
