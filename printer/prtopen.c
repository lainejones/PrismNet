/*
 * prtopen - test helper: opens printer.device (or with DEVICE=name a port
 * device directly), prints the error, and with TEXT writes a line through
 * it (as PRT: would). Part of PrismNet. GPL-3.0-only, see LICENSE.
 *   prtopen [TEXT] [DEVICE=parallel.device]
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <devices/printer.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    struct MsgPort *mp = CreateMsgPort();
    struct IOStdReq *io = (struct IOStdReq *)CreateIORequest(mp, sizeof(struct IODRPReq) + 16);
    static const char text[] = "Hello from printer.device through prismprint.device\r\n\f";
    const char *dev = "printer.device";
    BOOL write = FALSE;
    BYTE err;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "TEXT")) write = TRUE;
        else if (!strncmp(argv[i], "DEVICE=", 7)) dev = argv[i] + 7;
    }
    if (!io) return 20;
    err = OpenDevice((STRPTR)dev, 0, (struct IORequest *)io, 0);
    printf("OpenDevice %s: %d\n", dev, err);
    if (!err) {
        if (write) {
            io->io_Command = CMD_WRITE;
            io->io_Data = (APTR)text;
            io->io_Length = strlen(text);
            err = DoIO((struct IORequest *)io);
            printf("CMD_WRITE: %d, %ld bytes\n", err, (long)io->io_Actual);
        }
        CloseDevice((struct IORequest *)io);
    }
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(mp);
    return err ? 10 : 0;
}
