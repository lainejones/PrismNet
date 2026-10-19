/*
 * PrismNet network shares: NetBIOS name query packets (RFC 1001/1002), so
 * a server can be named the way Windows shows it (\\DROBO) when DNS does
 * not know it. Portable, no sockets: net_amiga.c / net_posix.c send them.
 * GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include "nbname.h"

int nb_wanted(const char *host)
{
    u32 n = 0;

    for (; host[n]; n++)
        if (host[n] == '.' || host[n] == ':')
            return 0;                   /* a DNS name or an address */
    return n > 0 && n <= 15;
}

u32 nb_query(u8 *out, const char *name, int bcast, u32 id)
{
    u8 padded[16];
    u32 i, n = 0;

    memset(out, 0, 12);
    out[0] = (u8)(id >> 8); out[1] = (u8)id;
    out[2] = 0x01;                      /* query, recursion desired */
    out[3] = bcast ? 0x10 : 0x00;       /* broadcast */
    out[5] = 1;                         /* one question */
    memset(padded, ' ', 15);
    for (i = 0; name[i] && i < 15; i++) {
        u8 c = (u8)name[i];
        padded[i] = c >= 'a' && c <= 'z' ? (u8)(c - 32) : c;
    }
    padded[15] = 0x20;                  /* the file server service */
    out[12] = 32;                       /* first-level encoding: a nibble per letter */
    for (i = 0; i < 16; i++) {
        out[13 + 2 * i] = (u8)('A' + (padded[i] >> 4));
        out[14 + 2 * i] = (u8)('A' + (padded[i] & 15));
    }
    n = 45;
    out[n++] = 0;                       /* end of name */
    out[n++] = 0; out[n++] = 0x20;      /* NB */
    out[n++] = 0; out[n++] = 0x01;      /* IN */
    return n;                           /* 50 */
}

u32 nb_answer(const u8 *p, u32 len, u32 id)
{
    u32 pos = 12, rdlen;

    if (len < 12 || p[0] != (u8)(id >> 8) || p[1] != (u8)id)
        return 0;
    if (!(p[2] & 0x80) || (p[3] & 0x0f) || !(p[6] | p[7]))
        return 0;                       /* not a positive answer */
    if (pos < len && (p[pos] & 0xc0) == 0xc0)
        pos += 2;                       /* compressed name */
    else {
        while (pos < len && p[pos])
            pos += 1 + p[pos];
        pos++;
    }
    if (pos + 10 + 6 > len || p[pos + 1] != 0x20)
        return 0;
    rdlen = (u32)p[pos + 8] << 8 | p[pos + 9];
    if (rdlen < 6)
        return 0;
    pos += 10 + 2;                      /* first entry: flags, then the address */
    return (u32)p[pos] << 24 | (u32)p[pos + 1] << 16 | (u32)p[pos + 2] << 8 | p[pos + 3];
}
