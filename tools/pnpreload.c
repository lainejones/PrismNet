/*
 * pnpreload - load PrismNet's libraries from a directory before anything
 * else opens them, so that later OpenLibrary("bsdsocket.library") calls get
 * PrismNet's copy even though another stack's library sits in LIBS:.
 *
 *   pnpreload [DIR=]dir
 *
 * Opening a library by path loads it into the system library list under its
 * own name; programs that open it by name afterwards get the one already in
 * memory. The libraries are deliberately left open, so a low-memory expunge
 * can never swap the other stack's library back in. Nothing in LIBS: (or in
 * the LIBS: assign) changes, which makes this the way to try PrismNet next
 * to an installed stack, one boot at a time.
 * GPL-3.0-only, see LICENSE.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

static const char version[] = "$VER: pnpreload 1.0 (16.10.2026)";
static const char *const names[] = { "bsdsocket.library", "usergroup.library" };

int main(void)
{
    LONG arg[1] = { 0 };
    struct RDArgs *rda;
    char path[256];
    int i, rc = RETURN_OK;

    (void)version;
    if (!(rda = ReadArgs((CONST_STRPTR)"DIR/A", arg, NULL))) {
        PrintFault(IoErr(), (CONST_STRPTR)"pnpreload");
        return RETURN_FAIL;
    }
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        struct Library *lib;
        strncpy(path, (char *)arg[0], sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        AddPart((STRPTR)path, (CONST_STRPTR)names[i], sizeof(path));
        if (!(lib = OpenLibrary((CONST_STRPTR)path, 0))) {
            Printf((CONST_STRPTR)"pnpreload: cannot load %s\n", (LONG)path);
            rc = RETURN_WARN;
        }
        /* left open on purpose: see above */
    }
    FreeArgs(rda);
    return rc;
}
