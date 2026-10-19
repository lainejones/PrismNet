/*
 * ShareUnmount - stop a network-share handler (PrismNet's NFS client, and
 * later its SMB client) cleanly: sends ACTION_DIE to the device. The device
 * stays in the DOS list; the next access starts the handler again.
 *   ShareUnmount PNFS0:
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version[] = "$VER: ShareUnmount 1.0 (17.10.2026) PrismNet";

int main(void)
{
    LONG arg[1] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DEVICE/A", arg, NULL);
    struct DosList *dol;
    struct MsgPort *port = NULL;
    char name[64];
    int found = 0;
    size_t n;
    LONG res;

    (void)version;
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"ShareUnmount");
        return RETURN_ERROR;
    }
    strncpy(name, (char *)arg[0], sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    n = strlen(name);
    if (n && name[n - 1] == ':')
        name[--n] = 0;

    dol = LockDosList(LDF_DEVICES | LDF_READ);
    if ((dol = FindDosEntry(dol, (CONST_STRPTR)name, LDF_DEVICES)) != NULL) {
        found = 1;
        port = dol->dol_Task;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (!found) {
        Printf((CONST_STRPTR)"ShareUnmount: no device called %s:\n", (LONG)name);
        FreeArgs(rda);
        return RETURN_WARN;
    }
    if (!port) {
        Printf((CONST_STRPTR)"%s: not running (nothing to unmount)\n", (LONG)name);
        FreeArgs(rda);
        return RETURN_OK;
    }
    res = DoPkt(port, ACTION_DIE, 0, 0, 0, 0, 0);
    if (res) {
        Printf((CONST_STRPTR)"%s: unmounted\n", (LONG)name);
    } else {
        Printf((CONST_STRPTR)"%s: ", (LONG)name);
        PrintFault(IoErr(), NULL);      /* e.g. "object in use": files still open */
    }
    FreeArgs(rda);
    return res ? RETURN_OK : RETURN_WARN;
}
