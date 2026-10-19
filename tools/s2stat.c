/*
 * s2stat - print a SANA-II unit's global statistics (diagnostic).
 * GPL-3.0-only, see LICENSE.
 *
 *   s2stat DEVICE=slip.device [UNIT=0]
 *
 * Opens the unit as an extra opener (no buffer hooks), asks for
 * S2_GETGLOBALSTATS and prints one line. Run it only while another
 * program keeps the unit open: closing the last opener shuts some
 * drivers' unit down.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "prismnet/sana2defs.h"

struct gstats {                 /* struct Sana2DeviceStats, from the spec */
    ULONG PacketsReceived;
    ULONG PacketsSent;
    ULONG BadData;
    ULONG Overruns;
    ULONG Unused;
    ULONG UnknownTypesReceived;
    ULONG Reconfigurations;
    ULONG LastStartSecs;
    ULONG LastStartMicro;
};

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda;
    struct MsgPort *port;
    struct IOSana2Req *io;
    struct gstats st;
    char path[96];
    const char *dev;
    ULONG unit;
    int rc = RETURN_FAIL;

    rda = ReadArgs((CONST_STRPTR)"DEVICE/A,UNIT/K/N", args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"s2stat");
        return RETURN_FAIL;
    }
    dev  = (const char *)args[0];
    unit = args[1] ? *(LONG *)args[1] : 0;
    port = CreateMsgPort();
    io   = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(*io)) : NULL;
    if (io) {
        LONG err = -1;
        if (!strchr(dev, ':') && !strchr(dev, '/')) {
            strcpy(path, "DEVS:Networks/");
            strncat(path, dev, sizeof(path) - strlen(path) - 1);
            err = OpenDevice((CONST_STRPTR)path, unit, (struct IORequest *)io, 0);
        }
        if (err)
            err = OpenDevice((CONST_STRPTR)dev, unit, (struct IORequest *)io, 0);
        if (err) {
            Printf("s2stat: cannot open %s unit %ld (%ld)\n", (LONG)dev, (LONG)unit, err);
        } else {
            memset(&st, 0, sizeof(st));
            io->ios2_Req.io_Command = S2_GETGLOBALSTATS;
            io->ios2_StatData = &st;
            err = DoIO((struct IORequest *)io);
            Printf("s2stat: err %ld rcvd %lu sent %lu baddata %lu overruns %lu unknown %lu\n",
                   err, st.PacketsReceived, st.PacketsSent, st.BadData, st.Overruns,
                   st.UnknownTypesReceived);
            CloseDevice((struct IORequest *)io);
            rc = err ? RETURN_WARN : RETURN_OK;
        }
        DeleteIORequest((struct IORequest *)io);
    }
    if (port)
        DeleteMsgPort(port);
    FreeArgs(rda);
    return rc;
}
