/*
 * SMBShares - list the shares an SMB server offers (PrismNet SMB client).
 *   SMBShares HOST/A,USER/K,DOMAIN/K,ALL/S
 * Guest unless USER is given; then the password the SMB handler saved for
 * that server and user is used, else it is asked for in the shell (not
 * echoed) and kept in memory only. ALL also shows the hidden "$"
 * shares. HOST may be a NetBIOS name. GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "smb2.h"
#include "net.h"

static const char version[] __attribute__((used)) = "$VER: SMBShares 1.0 (18.10.2026) PrismNet";

static int show_all;

static void show(void *ctx, const char *name, u32 type, const char *remark)
{
    static const char *kind[] = { "disk", "printer", "device", "IPC" };
    (void)ctx;
    if ((type & 0x80000000UL) && !show_all)
        return;
    Printf((CONST_STRPTR)"%-24s %-8s %s\n", (ULONG)name, (ULONG)kind[type & 3], (ULONG)remark);
}

/* the password the SMB handler saved for host and user, if any */
static int saved_password(const char *host, const char *user, char *out, int max)
{
    static const char *const dirs[2] = { "ENV:PrismNet/smb/", "ENVARC:PrismNet/smb/" };
    char name[200];
    int i;
    LONG n;

    for (i = 0; i < 2; i++) {
        BPTR f;
        strcpy(name, dirs[i]);
        strncat(name, host, 64);
        strcat(name, ".");
        strncat(name, user, 64);
        if ((f = Open((CONST_STRPTR)name, MODE_OLDFILE))) {
            n = Read(f, out, max - 1);
            Close(f);
            if (n < 0) n = 0;
            out[n] = 0;
            while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
            if (n > 0) return 1;
        }
    }
    return 0;
}

/* read a line from the console without echoing it */
static void ask_password(char *out, int max)
{
    BPTR in = Input(), o = Output();
    int n = 0;
    char c;

    Write(o, "Password: ", 10);
    SetMode(in, 1);                     /* raw: no echo */
    while (Read(in, &c, 1) == 1 && c != '\r' && c != '\n' && c != 3) {
        if ((c == 8 || c == 127) && n) n--;
        else if (c >= ' ' && n < max - 1) out[n++] = c;
    }
    SetMode(in, 0);
    out[n] = 0;
    Write(o, "\n", 1);
}

int main(void)
{
    LONG args[4] = { 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"HOST/A,USER/K,DOMAIN/K,ALL/S", args, NULL);
    static struct Smb s;
    u8 *tx, *rx, *buf;
    u32 ip;
    long e;
    int rc = RETURN_FAIL;

    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"SMBShares");
        return RETURN_FAIL;
    }
    show_all = args[3] != 0;
    tx = AllocVec(4096, MEMF_PUBLIC | MEMF_CLEAR);
    rx = AllocVec(9216, MEMF_PUBLIC | MEMF_CLEAR);
    buf = AllocVec(32768, MEMF_PUBLIC | MEMF_CLEAR);
    if (!tx || !rx || !buf) {
        PutStr((CONST_STRPTR)"SMBShares: not enough memory\n");
        goto out;
    }
    if (net_up()) {
        PutStr((CONST_STRPTR)"SMBShares: no TCP/IP stack (bsdsocket.library)\n");
        goto out;
    }
    if (!(ip = net_resolve((const char *)args[0]))) {
        Printf((CONST_STRPTR)"SMBShares: cannot find %s\n", args[0]);
        goto out;
    }
    smb2_init(&s, tx, 4096, rx, 9216);
    s.timeout = 15;
    if (args[1]) {
        strncpy(s.user, (const char *)args[1], sizeof(s.user) - 1);
        if (args[2]) strncpy(s.domain, (const char *)args[2], sizeof(s.domain) - 1);
        if (!saved_password((const char *)args[0], s.user, s.password, sizeof(s.password)))
            ask_password(s.password, sizeof(s.password));
    } else
        s.want_guest = 1;
    e = smb2_shares(&s, ip, 445, (const char *)args[0], buf, 32768, show, NULL);
    memset(s.password, 0, sizeof(s.password));
    if (e == FS_EDENIED)
        PutStr((CONST_STRPTR)"SMBShares: logon refused (user or password)\n");
    else if (e)
        Printf((CONST_STRPTR)"SMBShares: failed (%ld, NTSTATUS %08lx)\n", e, (ULONG)s.ntstatus);
    else
        rc = RETURN_OK;
out:
    net_down();
    if (buf) FreeVec(buf);
    if (rx) FreeVec(rx);
    if (tx) FreeVec(tx);
    FreeArgs(rda);
    return rc;
}
