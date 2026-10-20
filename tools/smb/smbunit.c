/*
 * smbunit - malformed-reply checks for the SMB client's parsers, run on the
 * PC with AddressSanitizer (make smbunit). It includes smb2.c so the static
 * helpers can be driven with crafted buffers; nothing touches the network.
 * Exit 0 = all pass. GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include "smb2.c"

static int fails;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int main(void)
{
    static u8 buf[4096];
    struct Fattr a;
    char name[300];
    u32 pos;
    int n, rounds;

    setvbuf(stdout, NULL, _IONBF, 0);       /* keep the log if ASan aborts */

    /* ---- QUERY_DIRECTORY entries ---- */
    /* one entry: FileNameLength so large that pos + 104 + nlen wraps */
    memset(buf, 0, sizeof(buf));
    p32(buf + 60, 0xFFFFFF00UL);
    pos = 0;
    check("dir entry with a wrapping name length is rejected",
          smb2_dir_next(buf, 200, &pos, name, sizeof(name), &a) == 0);

    /* two entries where the second one's NextEntryOffset points backwards */
    memset(buf, 0, sizeof(buf));
    p32(buf, 112);                              /* entry 0 -> entry 1 at 112 */
    p32(buf + 60, 2); buf[104] = 'a';
    p32(buf + 112, 0xFFFFFF90UL);               /* entry 1 -> "entry 0" again */
    p32(buf + 112 + 60, 2); buf[112 + 104] = 'b';
    pos = 0;
    for (n = 0, rounds = 0; rounds < 10 && smb2_dir_next(buf, 224, &pos, name, sizeof(name), &a); rounds++)
        n++;
    check("a backwards NextEntryOffset ends the listing (no loop)", n <= 2);

    /* a normal pair still walks */
    p32(buf + 112, 0);
    pos = 0;
    for (n = 0; smb2_dir_next(buf, 224, &pos, name, sizeof(name), &a); ) n++;
    check("two well-formed entries are both returned", n == 2);

    /* ---- the NTLM CHALLENGE's TargetInfo ---- */
    {
        static struct Smb s;
        static u8 ch[2048], out[NTLM_TOKEN_MAX + 64];
        u32 k, tl = 900;                        /* AV pairs longer than tmp[] can take */
        memset(&s, 0, sizeof(s));
        strcpy(s.user, "x"); strcpy(s.password, "y");
        memset(ch, 0, sizeof(ch));
        memcpy(ch, "NTLMSSP", 8); p32(ch + 8, 2);
        p32(ch + 20, NTLM_UNICODE | NTLM_NTLM);
        p16(ch + 40, tl); p16(ch + 42, tl); p32(ch + 44, 56);
        for (k = 0; k + 4 < tl; k += 4 + 32) {  /* MsvAvDnsTreeName entries */
            p16(ch + 56 + k, 5); p16(ch + 56 + k + 2, 32);
        }
        memset(out + NTLM_TOKEN_MAX, 0xa5, 64); /* canary past the real buffer */
        k = ntlm_auth(&s, ch, 56 + tl, out);
        check("a 900-byte TargetInfo is cut to fit the token buffer",
              k <= NTLM_TOKEN_MAX && out[NTLM_TOKEN_MAX] == 0xa5 && out[NTLM_TOKEN_MAX + 63] == 0xa5);

        /* TargetInfo offset that wraps past the end */
        p16(ch + 40, 32); p16(ch + 42, 32); p32(ch + 44, 0xFFFFFFF0UL);
        k = ntlm_auth(&s, ch, 56 + 64, out);
        check("a wrapping TargetInfo offset is ignored", k <= NTLM_TOKEN_MAX && k < 300);
    }
    {
        /* signed session, plain-text reply without a signature (audit 2026-10-18) */
        static struct Smb s;
        static u8 rx[256];
        u32 st, bl;
        const u8 *b;
        memset(&s, 0, sizeof(s));
        s.rx = rx;
        s.sign = 1;
        memset(rx, 0, sizeof(rx));
        rx[0] = 0xfe; rx[1] = 'S'; rx[2] = 'M'; rx[3] = 'B';
        p16(rx + 4, HDR);
        p32(rx + 16, F_REPLY);                  /* no F_SIGNED */
        p32(rx + 8, 0x80000005UL);              /* BUFFER_OVERFLOW: a read with data */
        b = reply_unit(&s, HDR + 32, 0, &st, &bl);
        check("an unsigned BUFFER_OVERFLOW on a signed session is dropped", b == NULL);
        p32(rx + 8, 0xC0000034UL);              /* OBJECT_NAME_NOT_FOUND */
        b = reply_unit(&s, HDR + 32, 0, &st, &bl);
        check("an unsigned error on a signed session is dropped", b == NULL);
        p32(rx + 8, ST_OK);
        b = reply_unit(&s, HDR + 32, 0, &st, &bl);
        check("an unsigned success on a signed session is dropped", b == NULL);
        p32(rx + 8, ST_SESSION_DELETED);
        b = reply_unit(&s, HDR + 32, 0, &st, &bl);
        check("an unsigned USER_SESSION_DELETED is let through", b != NULL && st == ST_SESSION_DELETED);
        s.sign = 0;
        p32(rx + 8, 0x80000005UL);
        b = reply_unit(&s, HDR + 32, 0, &st, &bl);
        check("without signing, unsigned replies are taken as before", b != NULL);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
