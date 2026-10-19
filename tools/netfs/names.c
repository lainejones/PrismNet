/*
 * PrismNet NFS client: Latin-1 <-> UTF-8 names. GPL-3.0-only, see LICENSE.
 */
#include "names.h"

u32 name_to_amiga(const char *src, u32 len, char *dst, u32 max, int raw)
{
    const u8 *s = (const u8 *)src, *end = s + len;
    u32 n = 0;

    while (s < end && n + 1 < max) {
        u32 c = *s++;

        if (!raw && c >= 0x80) {
            u32 more = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
            u32 i;

            if (!more || s + more > end) {
                c = '?';             /* not UTF-8 after all */
            } else {
                c &= 0x3f >> more;
                for (i = 0; i < more; i++) {
                    if ((s[i] & 0xc0) != 0x80) { c = '?'; more = i; break; }
                    c = (c << 6) | (s[i] & 0x3f);
                }
                s += more;
                /* overlong forms (C0 AF for '/', C0 BA for ':', C0 80 for NUL)
                 * would slip past the separator checks done on the raw bytes */
                if (c > 0xff || c < 0x20 || c < (more == 1 ? 0x80UL : more == 2 ? 0x800UL : 0x10000UL))
                    c = '?';
            }
        }
        dst[n++] = (char)c;
    }
    dst[n] = 0;
    return n;
}

u32 name_to_server(const char *src, char *dst, u32 max, int raw)
{
    const u8 *s = (const u8 *)src;
    u32 n = 0;

    for (; *s; s++) {
        if (raw || *s < 0x80) {
            if (n + 1 >= max) return 0;
            dst[n++] = (char)*s;
        } else {
            if (n + 2 >= max) return 0;
            dst[n++] = (char)(0xc0 | (*s >> 6));
            dst[n++] = (char)(0x80 | (*s & 0x3f));
        }
    }
    dst[n] = 0;
    return n;
}

static u32 fold(u32 c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
        return c + 32;
    return c;
}

int name_equal(const char *a, const char *b)
{
    const u8 *p = (const u8 *)a, *q = (const u8 *)b;

    while (*p && fold(*p) == fold(*q)) { p++; q++; }
    return fold(*p) == fold(*q);
}
