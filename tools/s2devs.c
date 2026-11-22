/*
 * S2Devs - which SANA-II network drivers are already loaded, and does one
 * of them work? For the installer (docs/manual/08-tools.md): a card
 * whose driver comes from its boot ROM has no file in DEVS:Networks.
 * Part of PrismNet. GPL-3.0-only, see LICENSE.
 *
 *   S2Devs LIST [ALL]                  the loaded devices that may be network
 *                                      drivers, one per line. Opens nothing:
 *                                      it only reads names from Exec's list.
 *   S2Devs CHECK DEVICE=name [UNIT=n]  opens that one driver and unit, asks it
 *                                      S2_DEVICEQUERY, closes it again.
 *
 * LIST leaves out devices that are never network drivers (the table below)
 * and, without ALL, the ones with a file in DEVS:Networks: those are offered
 * as a file, and PrismNet opens the file first anyway (port/sanaif.c).
 *
 * The answers also go to ENV:PrismNet-Install/, for the installer's getenv:
 *   count       how many LIST found (0-8 slots are filled)
 *   dev1..dev8  their names; an unused slot is empty
 *   list        all of them on one line, separated by ", "
 *   why         CHECK: the reason it failed, one line (else empty)
 *   info        CHECK: what the driver reported, one line (else empty)
 * CHECK returns 0 for a driver PrismNet can use, 10 (ERROR) when the device
 * opened but is not one (no SANA-II, or not Ethernet/SLIP/PPP/PLIP), and
 * 20 (FAIL) when OpenDevice failed.
 */
#include <string.h>
#include <stdio.h>
#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "prismnet/sana2defs.h"

static const char ver[] __attribute__((used)) = "$VER: S2Devs 1.0 (22.11.2026) PrismNet";

#define ENVDIR   "ENV:PrismNet-Install"
#define SLOTS    8              /* dev1..dev8: the installer's askchoice has 8 */
#define MAXDEVS  48
#define NAMELEN  64

/* Devices that are never network drivers. A name not in this table stays on
 * the list: the probe of the one the user picks decides. Case does not matter. */
static const char *const not_net[] = {
    "trackdisk.device", "carddisk.device", "serial.device", "parallel.device",
    "printer.device", "timer.device", "input.device", "keyboard.device",
    "gameport.device", "console.device", "audio.device", "clipboard.device",
    "narrator.device", "ramdrive.device", "scsi.device", "2nd.scsi.device",
    "ide.device", "prismprint.device",
    /* more of the same kind, seen on real and emulated Amigas */
    "cd.device", "trackfile.device", "mfm.device", "diskimage.device",
    "ahi.device", "uaehf.device", "uaescsi.device", "printtofile.device",
    NULL
};

static char names[MAXDEVS][NAMELEN];
static int  nnames;

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return *a == *b;
}

static int is_filtered(const char *n)
{
    int i;
    for (i = 0; not_net[i]; i++)
        if (same(n, not_net[i])) return 1;
    return 0;
}

static BOOL setenv_(const char *name, const char *val)
{
    char path[48];
    strcpy(path, "PrismNet-Install/");
    strcat(path, name);
    /* never deleted, only emptied: OS 3.1's Installer 43.3 stops the script
     * ("GETENV: String too long") when getenv reads a variable that is not there */
    return SetVar((CONST_STRPTR)path, (STRPTR)(val ? val : ""), -1, GVF_GLOBAL_ONLY);
}

static void envdir(void)
{
    BPTR l = Lock((CONST_STRPTR)ENVDIR, ACCESS_READ);
    if (!l) l = CreateDir((CONST_STRPTR)ENVDIR);
    if (l) UnLock(l);
}

/* Copy the device names out of Exec's list. Forbid() keeps the list still;
 * nothing is allocated, printed or opened until Permit(). */
static void snapshot(void)
{
    struct ExecBase *eb = (struct ExecBase *)SysBase;
    struct Node *n;

    nnames = 0;
    Forbid();
    for (n = eb->DeviceList.lh_Head; n->ln_Succ && nnames < MAXDEVS; n = n->ln_Succ) {
        const char *s = n->ln_Name;
        int i;
        if (!s) continue;
        for (i = 0; i < NAMELEN - 1 && s[i]; i++)
            names[nnames][i] = s[i];
        names[nnames][i] = 0;
        if (!s[i]) nnames++;            /* a longer name is no driver we can name in a file */
    }
    Permit();
}

static int on_disk(const char *n)
{
    char path[NAMELEN + 16];
    BPTR l;
    strcpy(path, "DEVS:Networks/");
    strcat(path, n);
    l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

static int do_list(int all)
{
    char list[SLOTS * (NAMELEN + 2) + 8];
    char num[8];
    int i, j, found = 0;

    snapshot();
    list[0] = 0;
    envdir();
    for (i = 0; i < nnames; i++) {
        const char *n = names[i];
        if (is_filtered(n)) continue;
        for (j = 0; j < i; j++)                 /* the same name twice: once */
            if (!strcmp(names[j], n)) break;
        if (j < i) continue;
        if (!all && on_disk(n)) continue;
        printf("%s\n", n);
        if (found < SLOTS) {
            sprintf(num, "dev%d", found + 1);
            setenv_(num, n);
            if (found) strcat(list, ", ");
            strcat(list, n);
        }
        found++;
    }
    for (i = found; i < SLOTS; i++) {
        sprintf(num, "dev%d", i + 1);
        setenv_(num, NULL);
    }
    if (found > SLOTS) found = SLOTS;
    sprintf(num, "%d", found);
    setenv_("list", list);
    /* written last: without it the installer must not read the others */
    if (!setenv_("count", num)) {
        printf("S2Devs: can't write ENV:PrismNet-Install/count\n");
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int loaded(const char *n)
{
    struct ExecBase *eb = (struct ExecBase *)SysBase;
    int r;
    Forbid();
    r = FindName(&eb->DeviceList, (CONST_STRPTR)n) != NULL;
    Permit();
    return r;
}

static const char *hwname(ULONG t)
{
    switch (t) {
    case S2WireType_Ethernet: return "Ethernet";
    case S2WireType_IEEE802:  return "IEEE 802.3";
    case S2WireType_PPP:      return "PPP";
    case S2WireType_SLIP:     return "SLIP";
    case S2WireType_CSLIP:    return "CSLIP";
    case S2WireType_PLIP:     return "PLIP";
    }
    return NULL;
}

/* PrismNet's own buffer hooks are not needed for a query, but some drivers
 * refuse an open without the buffer management tags: give them do-nothing
 * ones (nothing is sent or received before CloseDevice). */
static ULONG no_copy(register APTR to __asm("a0"), register APTR from __asm("a1"),
                     register ULONG n __asm("d0"))
{
    (void)to; (void)from; (void)n;
    return FALSE;
}
static struct TagItem buffer_tags[] = {
    { S2_CopyToBuff,   (ULONG)no_copy },
    { S2_CopyFromBuff, (ULONG)no_copy },
    { TAG_DONE,        0 }
};

static int do_check(const char *dev, ULONG unit, int quiet)
{
    struct MsgPort *port;
    struct IOSana2Req *io;
    struct Sana2DeviceQuery q;
    char line[160];
    const char *hw;
    LONG err;
    int rc;

    envdir();
    setenv_("why", NULL);
    setenv_("info", NULL);
    if (is_filtered(dev)) {
        sprintf(line, "%s is not a network driver", dev);
        rc = RETURN_ERROR;
        goto out;
    }
    port = CreateMsgPort();
    io = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(*io)) : NULL;
    if (!io) {
        if (port) DeleteMsgPort(port);
        strcpy(line, "not enough memory");
        rc = RETURN_FAIL;
        goto out;
    }
    io->ios2_BufferManagement = buffer_tags;
    err = OpenDevice((CONST_STRPTR)dev, unit, (struct IORequest *)io, 0);
    if (err) {
        if (!loaded(dev))
            sprintf(line, "%s is not loaded, and DEVS: has no file of that name", dev);
        else if (err == IOERR_OPENFAIL)
            sprintf(line, "%s has no unit %lu (or its card is not there)", dev, (unsigned long)unit);
        else
            sprintf(line, "%s unit %lu does not open (error %ld, wire error %ld)", dev,
                    (unsigned long)unit, (long)err, (long)io->ios2_WireError);
        rc = RETURN_FAIL;
    } else {
        memset(&q, 0, sizeof(q));
        q.SizeAvailable = sizeof(q);
        io->ios2_StatData = &q;
        io->ios2_Req.io_Command = S2_DEVICEQUERY;
        io->ios2_Req.io_Flags = 0;
        DoIO((struct IORequest *)io);
        err = io->ios2_Req.io_Error;
        CloseDevice((struct IORequest *)io);
        hw = hwname(q.HardwareType);
        if (err == IOERR_NOCMD || err == IOERR_BADLENGTH) {
            sprintf(line, "%s is not a SANA-II network driver", dev);
            rc = RETURN_ERROR;
        } else if (err) {
            sprintf(line, "%s does not answer a SANA-II device query (error %ld): not a network driver?",
                    dev, (long)err);
            rc = RETURN_ERROR;
        } else if (!hw || ((q.HardwareType == S2WireType_Ethernet || q.HardwareType == S2WireType_IEEE802)
                           && q.AddrFieldSize != 48)) {
            sprintf(line, "%s is a SANA-II driver, but neither Ethernet nor SLIP/PPP/PLIP "
                    "(hardware type %lu, %u-bit address)", dev, (unsigned long)q.HardwareType,
                    (unsigned)q.AddrFieldSize);
            rc = RETURN_ERROR;
        } else {
            sprintf(line, "%s, MTU %lu, %u-bit address", hw, (unsigned long)q.MTU, (unsigned)q.AddrFieldSize);
            rc = RETURN_OK;
        }
    }
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
out:
    setenv_(rc ? "why" : "info", line);
    if (!quiet) {
        if (rc) printf("%s\n", line);
        else printf("%s unit %lu: %s\n", dev, (unsigned long)unit, line);
    }
    return rc;
}

int main(void)
{
    LONG args[6] = { 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    int rc;

    rda = ReadArgs((CONST_STRPTR)"LIST/S,CHECK/S,DEVICE/K,UNIT/K/N,ALL/S,QUIET/S", args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"S2Devs");
        return RETURN_FAIL;
    }
    me->pr_WindowPtr = (APTR)-1;        /* no "insert volume" requester for DEVS:Networks */
    if (args[1] && args[2]) {
        rc = do_check((const char *)args[2], args[3] ? *(ULONG *)args[3] : 0, args[5] != 0);
    } else if (args[0]) {
        rc = do_list(args[4] != 0);
    } else {
        printf("S2Devs LIST [ALL]\nS2Devs CHECK DEVICE=name [UNIT=n] [QUIET]\n");
        rc = RETURN_WARN;
    }
    me->pr_WindowPtr = oldwin;
    FreeArgs(rda);
    return rc;
}
