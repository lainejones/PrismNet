/*
 * PrismNet - SANA-II definitions.
 * GPL-3.0-only, see LICENSE.
 *
 * Written from the public SANA-II Network Device Driver Specification so
 * PrismNet does not need the (non-free) NDK/SDK header. Only what PrismNet
 * uses is defined. Names follow the specification so code reads the same.
 */
#ifndef PRISMNET_SANA2DEFS_H
#define PRISMNET_SANA2DEFS_H

#include <exec/types.h>
#include <exec/io.h>
#include <utility/tagitem.h>

#define SANA2_MAX_ADDR_BITS   128
#define SANA2_MAX_ADDR_BYTES  ((SANA2_MAX_ADDR_BITS + 7) / 8)

struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG  ios2_WireError;
    ULONG  ios2_PacketType;
    UBYTE  ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];
    UBYTE  ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
    ULONG  ios2_DataLength;
    APTR   ios2_Data;
    APTR   ios2_StatData;
    APTR   ios2_BufferManagement;
};

/* io_Flags */
#define SANA2IOB_RAW    7
#define SANA2IOF_RAW    (1 << SANA2IOB_RAW)
#define SANA2IOB_BCAST  6
#define SANA2IOF_BCAST  (1 << SANA2IOB_BCAST)
#define SANA2IOB_MCAST  5
#define SANA2IOF_MCAST  (1 << SANA2IOB_MCAST)

/* OpenDevice() flags */
#define SANA2OPB_MINE   0
#define SANA2OPF_MINE   (1 << SANA2OPB_MINE)
#define SANA2OPB_PROM   1
#define SANA2OPF_PROM   (1 << SANA2OPB_PROM)

/* Buffer-management tags (ios2_BufferManagement at OpenDevice time) */
#define S2_Dummy            (TAG_USER + 0xB0000)
#define S2_CopyToBuff       (S2_Dummy + 1)
#define S2_CopyFromBuff     (S2_Dummy + 2)
#define S2_PacketFilter     (S2_Dummy + 3)
#define S2_DMACopyToBuff32  (S2_Dummy + 4)   /* SANA-II R3: a0 = ios2_Data -> buffer or NULL */
#define S2_DMACopyFromBuff32 (S2_Dummy + 5)

/* Commands (CMD_READ / CMD_WRITE are the standard exec ones) */
#define S2_DEVICEQUERY          (CMD_NONSTD + 0)
#define S2_GETSTATIONADDRESS    (CMD_NONSTD + 1)
#define S2_CONFIGINTERFACE      (CMD_NONSTD + 2)
#define S2_ADDMULTICASTADDRESS  (CMD_NONSTD + 5)
#define S2_DELMULTICASTADDRESS  (CMD_NONSTD + 6)
#define S2_MULTICAST            (CMD_NONSTD + 7)
#define S2_BROADCAST            (CMD_NONSTD + 8)
#define S2_TRACKTYPE            (CMD_NONSTD + 9)
#define S2_UNTRACKTYPE          (CMD_NONSTD + 10)
#define S2_GETTYPESTATS         (CMD_NONSTD + 11)
#define S2_GETSPECIALSTATS      (CMD_NONSTD + 12)
#define S2_GETGLOBALSTATS       (CMD_NONSTD + 13)
#define S2_ONEVENT              (CMD_NONSTD + 14)
#define S2_READORPHAN           (CMD_NONSTD + 15)
#define S2_ONLINE               (CMD_NONSTD + 16)
#define S2_OFFLINE              (CMD_NONSTD + 17)

/* io_Error values */
#define S2ERR_NO_ERROR       0
#define S2ERR_NO_RESOURCES   1
#define S2ERR_BAD_ARGUMENT   3
#define S2ERR_BAD_STATE      4
#define S2ERR_BAD_ADDRESS    5
#define S2ERR_MTU_EXCEEDED   6
#define S2ERR_NOT_SUPPORTED  8
#define S2ERR_SOFTWARE       9
#define S2ERR_OUTOFSERVICE   10
#define S2ERR_TX_FAILURE     11

/* Wire types (Sana2DeviceQuery.HardwareType) */
#define S2WireType_Ethernet  1
#define S2WireType_IEEE802   6
#define S2WireType_PPP       253
#define S2WireType_SLIP      254
#define S2WireType_CSLIP     255
#define S2WireType_PLIP      420

struct Sana2DeviceQuery {
    ULONG SizeAvailable;
    ULONG SizeSupplied;
    ULONG DevQueryFormat;
    ULONG DeviceLevel;
    UWORD AddrFieldSize;    /* in bits */
    ULONG MTU;
    ULONG BPS;
    ULONG HardwareType;
};

#endif /* PRISMNET_SANA2DEFS_H */
