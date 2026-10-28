/* pptrace - test helper: prints the PP_DEBUG trace of prismprint.device. */
#include <stdio.h>
#include <exec/execbase.h>
#include <exec/devices.h>
#include <proto/exec.h>
int main(int argc, char **argv)
{
    struct Device *d;
    ULONG *t, i;
    Forbid();
    d = (struct Device *)FindName(&SysBase->DeviceList, (STRPTR)(argc > 1 ? argv[1] : "prismprint.device"));
    Permit();
    if (!d) { printf("%s not loaded\n", argc > 1 ? argv[1] : "prismprint.device"); return 5; }
    t = (ULONG *)((UBYTE *)d + sizeof(struct Device));
    for (i = 0; i < t[0]; i++) {
        ULONG a = t[1 + 3 * i];
        if ((a >> 8) == 0x434d44) printf("cmd %lu len %ld  mnlen %lu flags %lx\n", a & 255, (long)t[2 + 3 * i], t[3 + 3 * i] >> 16, t[3 + 3 * i] & 0xffff);
        else printf("%c%c%c%c %ld %lx\n", (char)(a >> 24), (char)(a >> 16), (char)(a >> 8), (char)a, (long)t[2 + 3 * i], t[3 + 3 * i]);
    }
    return 0;
}