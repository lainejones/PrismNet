/*
 * ugdump - show the raw struct passwd a usergroup.library returns, to see
 * which layout it uses (AmiTCP's 7 fields or the BSD 10). Part of PrismNet.
 * GPL-3.0-only, see LICENSE.
 *
 *   ugdump [NAME]      (default: the first entry, getpwent)
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/usergroup.h>
#include <../ndk-include/inline/usergroup.h>

struct Library *UserGroupBase;

/* Print a long word: as a string if it looks like a pointer to text. */
static void show(int off, ULONG v)
{
    const UBYTE *s = (const UBYTE *)v;
    int i, ok = v > 0x400 && v < 0x10000000UL && !(v & 0);
    printf("  +%-2d %08lx", off, (unsigned long)v);
    if (ok) {
        for (i = 0; i < 40 && s[i]; i++)
            if (s[i] < 32 || s[i] > 126) { ok = 0; break; }
        if (ok) printf("  \"%.*s\"", i, (const char *)s);
    }
    if (!ok && v < 100000) printf("  (%lu)", (unsigned long)v);
    printf("\n");
}

int main(int argc, char **argv)
{
    struct passwd *pw;
    const ULONG *w;
    int i;

    if (!(UserGroupBase = OpenLibrary((CONST_STRPTR)"usergroup.library", 0))) {
        printf("ugdump: no usergroup.library\n");
        return RETURN_FAIL;
    }
    printf("ugdump: %s\n", UserGroupBase->lib_IdString ? (char *)UserGroupBase->lib_IdString : "?");
    if (argc > 1) {
        pw = getpwnam((STRPTR)argv[1]);
    } else {
        setpwent();
        pw = getpwent();
    }
    if (!pw) {
        printf("ugdump: no such user\n");
        CloseLibrary(UserGroupBase);
        return RETURN_WARN;
    }
    w = (const ULONG *)pw;
    for (i = 0; i < 10; i++)
        show(i * 4, w[i]);
    printf("7-field AmiTCP: name passwd uid gid gecos dir shell\n");
    printf("10-field BSD:   name passwd uid gid change class gecos dir shell expire\n");
    CloseLibrary(UserGroupBase);
    return RETURN_OK;
}
