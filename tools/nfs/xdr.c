/*
 * PrismNet NFS client: XDR (RFC 4506). GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include "xdr.h"

void xdr_init(struct Xdr *x, u8 *buf, u32 len)
{
    x->buf = buf;
    x->pos = 0;
    x->len = len;
    x->err = 0;
}

static int room(struct Xdr *x, u32 n)
{
    if (x->err || x->pos + n > x->len || x->pos + n < x->pos) {
        x->err = 1;
        return 0;
    }
    return 1;
}

void xdr_put_u32(struct Xdr *x, u32 v)
{
    u8 *p;

    if (!room(x, 4))
        return;
    p = x->buf + x->pos;
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    x->pos += 4;
}

void xdr_put_u64(struct Xdr *x, u32 hi, u32 lo)
{
    xdr_put_u32(x, hi);
    xdr_put_u32(x, lo);
}

void xdr_put_opaque(struct Xdr *x, const void *p, u32 n)
{
    u32 padded = XDR_PAD(n);

    xdr_put_u32(x, n);
    if (!room(x, padded))
        return;
    memcpy(x->buf + x->pos, p, n);
    memset(x->buf + x->pos + n, 0, padded - n);
    x->pos += padded;
}

void xdr_put_string(struct Xdr *x, const char *s)
{
    xdr_put_opaque(x, s, (u32)strlen(s));
}

u32 xdr_get_u32(struct Xdr *x)
{
    const u8 *p;

    if (!room(x, 4))
        return 0;
    p = x->buf + x->pos;
    x->pos += 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

void xdr_get_u64(struct Xdr *x, u32 *hi, u32 *lo)
{
    u32 h = xdr_get_u32(x), l = xdr_get_u32(x);

    if (hi) *hi = h;
    if (lo) *lo = l;
}

const u8 *xdr_get_opaque_ref(struct Xdr *x, u32 *n)
{
    u32 len = xdr_get_u32(x);
    const u8 *p;

    /* the length itself first: XDR_PAD() of a huge length wraps to 0 */
    if (x->err || len > x->len - x->pos || !room(x, XDR_PAD(len))) {
        x->err = 1;
        *n = 0;
        return 0;
    }
    p = x->buf + x->pos;
    x->pos += XDR_PAD(len);
    *n = len;
    return p;
}

u32 xdr_get_opaque(struct Xdr *x, void *p, u32 max)
{
    u32 n;
    const u8 *src = xdr_get_opaque_ref(x, &n);

    if (!src)
        return 0;
    if (n > max) {
        x->err = 1;
        return 0;
    }
    memcpy(p, src, n);
    return n;
}

void xdr_get_fixed(struct Xdr *x, void *p, u32 n)
{
    if (x->err || n > x->len - x->pos || !room(x, XDR_PAD(n))) {
        x->err = 1;
        return;
    }
    memcpy(p, x->buf + x->pos, n);
    x->pos += XDR_PAD(n);
}

void xdr_skip(struct Xdr *x, u32 n)
{
    if (!x->err && n <= x->len - x->pos && room(x, XDR_PAD(n)))
        x->pos += XDR_PAD(n);
    else
        x->err = 1;
}
