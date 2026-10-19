/*
 * carddiag - PrismNet developer tool: PCMCIA slot diagnostics.
 * GPL-3.0-only, see LICENSE.
 *
 * Reports who owns the A600/A1200 PCMCIA slot and dumps the card's CIS
 * identity tuples, then (optionally) tries to OpenDevice a SANA-II driver
 * and prints io_Error / ios2_WireError.
 *
 *   carddiag [DEVICE=cnet.device] [UNIT=0]
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/interrupts.h>
#include <resources/card.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
/* proto/cardres.h in this NDK points at a missing clib/card_protos.h */
#include <clib/cardres_protos.h>
#include <inline/cardres.h>

#include "prismnet/sana2defs.h"

struct Library *CardResource;

static ULONG dummy_int(register APTR data __asm("a1"))
{
    (void)data;
    return 0;
}

static void dump_tuple(struct CardHandle *h, ULONG code, const char *name)
{
    UBYTE buf[258];
    int i, n;

    memset(buf, 0, sizeof(buf));
    if (!CopyTuple(h, buf, code, sizeof(buf) - 2)) {
        printf("  %-8s (0x%02lx): not found\n", name, (unsigned long)code);
        return;
    }
    n = buf[1] + 2;
    printf("  %-8s (0x%02lx):", name, (unsigned long)code);
    for (i = 0; i < n && i < 40; i++)
        printf(" %02x", buf[i]);
    printf("\n           text: ");
    for (i = 2; i < n; i++)
        putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
    printf("\n");
}

int main(void)
{
    LONG args[2] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DEVICE/K,UNIT/K/N", args, NULL);
    static struct Interrupt ins, rem, sta;
    static struct CardHandle h;
    struct CardHandle *owner;

    CardResource = (struct Library *)OpenResource((CONST_STRPTR)"card.resource");
    if (!CardResource) {
        printf("no card.resource (not an A600/A1200?)\n");
        return RETURN_FAIL;
    }
    printf("card status register: 0x%02lx\n", (unsigned long)ReadCardStatus());

    ins.is_Node.ln_Type = rem.is_Node.ln_Type = sta.is_Node.ln_Type = NT_INTERRUPT;
    ins.is_Code = rem.is_Code = sta.is_Code = (void (*)())dummy_int;
    h.cah_CardNode.ln_Name = (char *)"carddiag";
    h.cah_CardInserted = &ins;
    h.cah_CardRemoved  = &rem;
    h.cah_CardFlags    = CARDF_IFAVAILABLE;

    owner = (struct CardHandle *)OwnCard(&h);
    if (owner == NULL) {
        printf("OwnCard: OK (slot was free)\n");
        BeginCardAccess(&h);
        dump_tuple(&h, 0x15, "VERS_1");
        dump_tuple(&h, 0x20, "MANFID");
        dump_tuple(&h, 0x21, "FUNCID");
        dump_tuple(&h, 0x1A, "CONFIG");
        EndCardAccess(&h);
        ReleaseCard(&h, CARDF_REMOVEHANDLE);
    } else if ((LONG)owner == -1) {
        printf("OwnCard: card not available (no card inserted?)\n");
    } else {
        printf("OwnCard: slot owned by '%s'\n",
               owner->cah_CardNode.ln_Name ? owner->cah_CardNode.ln_Name : "(no name)");
    }

    if (args[0]) {
        struct MsgPort *port = CreateMsgPort();
        struct IOSana2Req *io = (struct IOSana2Req *)CreateIORequest(port, sizeof(*io));
        static struct TagItem none[] = { { TAG_DONE, 0 } };
        ULONG unit = args[1] ? *(LONG *)args[1] : 0;
        LONG err;

        io->ios2_BufferManagement = none;
        err = OpenDevice((CONST_STRPTR)args[0], unit, (struct IORequest *)io, 0);
        printf("OpenDevice(%s, %lu): io_Error %ld, wire error %lu\n", (char *)args[0],
               (unsigned long)unit, (long)err, (unsigned long)io->ios2_WireError);
        if (err == 0)
            CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
    }
    if (rda)
        FreeArgs(rda);
    return RETURN_OK;
}
