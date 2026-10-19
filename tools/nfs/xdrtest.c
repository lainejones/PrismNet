/*
 * xdrtest - malformed-reply checks for the NFS client's XDR decoder, run on
 * the PC (make xdrtest, built with AddressSanitizer). Exit 0 = all pass.
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include "xdr.h"

static int fails;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int main(void)
{
    struct Xdr x;
    u8 buf[16];
    const u8 *p;
    u32 n;
    unsigned long long big;

    /* an opaque whose length pads to 0: 0xFFFFFFFF + 3 wraps, so the room
     * check must look at the length itself, not only the padded size */
    memset(buf, 0, sizeof(buf));
    buf[0] = buf[1] = buf[2] = buf[3] = 0xff;
    xdr_init(&x, buf, 8);
    p = xdr_get_opaque_ref(&x, &n);
    check("opaque length 0xFFFFFFFF in an 8-byte reply is refused", p == NULL && n == 0 && x.err);

    buf[3] = 0xfd;                              /* 0xFFFFFFFD pads to 0 too */
    xdr_init(&x, buf, 8);
    p = xdr_get_opaque_ref(&x, &n);
    check("opaque length 0xFFFFFFFD is refused", p == NULL && n == 0 && x.err);

    /* a sane one still works: length 3 ("abc"), padded to 4 */
    memset(buf, 0, sizeof(buf));
    buf[3] = 3; buf[4] = 'a'; buf[5] = 'b'; buf[6] = 'c';
    xdr_init(&x, buf, 8);
    p = xdr_get_opaque_ref(&x, &n);
    check("opaque length 3 decodes", p == buf + 4 && n == 3 && !x.err && x.pos == 8);

    /* a length one over what is left */
    buf[3] = 5;
    xdr_init(&x, buf, 8);
    p = xdr_get_opaque_ref(&x, &n);
    check("opaque length 5 in 4 remaining bytes is refused", p == NULL && x.err);

    /* xdr_get_fixed / xdr_skip with a wrapping count */
    xdr_init(&x, buf, 8);
    xdr_skip(&x, 0xFFFFFFFEUL);
    check("skip 0xFFFFFFFE does not move past the end", x.pos <= 8);
    big = 0;
    (void)big;
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
