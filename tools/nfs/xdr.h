/*
 * PrismNet NFS client: XDR (RFC 4506) into a flat buffer.
 * Byte-order independent, so it also builds on the PC for host tests.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PNFS_XDR_H
#define PNFS_XDR_H

#include "nettypes.h"

struct Xdr {
    u8   *buf;
    u32   pos, len;                   /* len: bytes valid (decode) or room (encode) */
    int   err;                        /* set on overrun; sticky */
};

void  xdr_init(struct Xdr *x, u8 *buf, u32 len);

void  xdr_put_u32(struct Xdr *x, u32 v);
void  xdr_put_u64(struct Xdr *x, u32 hi, u32 lo);
void  xdr_put_opaque(struct Xdr *x, const void *p, u32 n);   /* length + data + pad */
void  xdr_put_string(struct Xdr *x, const char *s);

u32   xdr_get_u32(struct Xdr *x);
void  xdr_get_u64(struct Xdr *x, u32 *hi, u32 *lo);
/* variable-length opaque: copies at most max bytes, returns the length (0 on error) */
u32   xdr_get_opaque(struct Xdr *x, void *p, u32 max);
/* as above but returns a pointer into the buffer instead of copying */
const u8 *xdr_get_opaque_ref(struct Xdr *x, u32 *n);
void  xdr_get_fixed(struct Xdr *x, void *p, u32 n);          /* fixed opaque + pad */
void  xdr_skip(struct Xdr *x, u32 n);                        /* n bytes + pad */

#define XDR_PAD(n) (((n) + 3) & ~3UL)

#endif
