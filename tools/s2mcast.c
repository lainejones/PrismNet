/*
 * s2mcast - how does a SANA-II driver take a multicast address? (diagnostic)
 * GPL-3.0-only, see LICENSE.
 *
 *   s2mcast DEVICE=x-surf.device [UNIT=0]
 *
 * Opens the unit as an extra opener and asks it to add (then delete) the
 * mDNS group 01:00:5e:00:00:fb in each way the SANA-II documents allow,
 * printing io_Error and ios2_WireError for every attempt. Run it while
 * PrismNet keeps the unit open.
 */
#include <string.h>
#include <stdio.h>
#include <exec/types.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "prismnet/sana2defs.h"

#define S2_ADDMULTICASTADDRESSES 0xc000     /* SANA-II R4: a range, SrcAddr..DstAddr */
#define S2_DELMULTICASTADDRESSES 0xc001

static const UBYTE mdns[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };

static void put(UBYTE *dst) { memset(dst, 0, SANA2_MAX_ADDR_BYTES); memcpy(dst, mdns, 6); }

static void try(struct IOSana2Req *io, const char *what, UWORD cmd, int src, int dst)
{
    memset(io->ios2_SrcAddr, 0, SANA2_MAX_ADDR_BYTES);
    memset(io->ios2_DstAddr, 0, SANA2_MAX_ADDR_BYTES);
    if (src) put(io->ios2_SrcAddr);
    if (dst) put(io->ios2_DstAddr);
    io->ios2_Req.io_Command = cmd;
    io->ios2_Req.io_Flags = 0;
    io->ios2_WireError = 0;
    DoIO((struct IORequest *)io);
    printf("  %-34s io_Error %3d  wire %ld\n", what, (int)io->ios2_Req.io_Error, (long)io->ios2_WireError);
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DEVICE/A,UNIT/K/N", args, NULL);
    struct MsgPort *port;
    struct IOSana2Req *io;
    char path[96];
    const char *dev;
    ULONG unit;
    LONG err = -1;

    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"s2mcast"); return RETURN_FAIL; }
    dev  = (const char *)args[0];
    unit = args[1] ? *(LONG *)args[1] : 0;
    port = CreateMsgPort();
    io   = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(*io)) : NULL;
    if (!io) { printf("no memory\n"); return RETURN_FAIL; }
    if (!strchr(dev, ':') && !strchr(dev, '/')) {
        strcpy(path, "DEVS:Networks/");
        strncat(path, dev, sizeof(path) - strlen(path) - 1);
        err = OpenDevice((CONST_STRPTR)path, unit, (struct IORequest *)io, 0);
    }
    if (err) err = OpenDevice((CONST_STRPTR)dev, unit, (struct IORequest *)io, 0);
    if (err) { printf("can't open %s unit %lu (%ld)\n", dev, (unsigned long)unit, (long)err); return RETURN_FAIL; }

    {
        struct Sana2DeviceQuery q;
        memset(&q, 0, sizeof(q));
        q.SizeAvailable = sizeof(q);
        io->ios2_StatData = &q;
        io->ios2_Req.io_Command = S2_DEVICEQUERY;
        DoIO((struct IORequest *)io);
        printf("%s unit %lu: query error %d, addr bits %lu, mtu %lu, hw type %lu, version %u.%u\n", dev,
               (unsigned long)unit, (int)io->ios2_Req.io_Error, (unsigned long)q.AddrFieldSize,
               (unsigned long)q.MTU, (unsigned long)q.HardwareType,
               (unsigned)io->ios2_Req.io_Device->dd_Library.lib_Version,
               (unsigned)io->ios2_Req.io_Device->dd_Library.lib_Revision);
    }
    printf("add 01:00:5e:00:00:fb:\n");
    try(io, "ADDMULTICAST, address in SrcAddr", S2_ADDMULTICASTADDRESS, 1, 0);
    try(io, "  DELMULTICAST (SrcAddr)", S2_DELMULTICASTADDRESS, 1, 0);
    try(io, "ADDMULTICAST, address in DstAddr", S2_ADDMULTICASTADDRESS, 0, 1);
    try(io, "  DELMULTICAST (DstAddr)", S2_DELMULTICASTADDRESS, 0, 1);
    try(io, "ADDMULTICAST, in both", S2_ADDMULTICASTADDRESS, 1, 1);
    try(io, "  DELMULTICAST (both)", S2_DELMULTICASTADDRESS, 1, 1);
    try(io, "ADDMULTICASTADDRESSES (R4 range)", S2_ADDMULTICASTADDRESSES, 1, 1);
    try(io, "  DELMULTICASTADDRESSES", S2_DELMULTICASTADDRESSES, 1, 1);
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return RETURN_OK;
}
