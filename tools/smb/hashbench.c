/*
 * hashbench - how fast this Amiga can sign and encrypt SMB traffic: HMAC-
 * SHA256 (2.x signing), AES-CMAC (3.x signing), AES-CCM/GCM (encryption)
 * and MD5, each over 1 MB in 16 KB messages.
 *   hashbench [KB=n]
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "crypto.h"

static ULONG ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 4320000UL + ds.ds_Minute * 3000UL + ds.ds_Tick;
}

int main(void)
{
    LONG args[1] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"KB/K/N", args, NULL);
    ULONG kb = args[0] ? *(LONG *)args[0] : 1024, t0, t1, i;
    static UBYTE buf[16384], key[16], out[32];
    struct Md5 m;

    memset(buf, 0x5a, sizeof(buf));
    memset(key, 0x0b, sizeof(key));

    /* signing works per message: 16 KB messages, like a 16 KB READ reply */
    t0 = ticks();
    for (i = 0; i < kb / 16; i++)
        hmac_sha256(key, 16, buf, sizeof(buf), 0, 0, out);
    t1 = ticks();
    printf("HMAC-SHA256: %lu KB in %lu ticks = %lu KB/s\n", kb, t1 - t0,
           (unsigned long)(t1 > t0 ? kb * 50 / (t1 - t0) : 0));

    t0 = ticks();
    md5_init(&m);
    for (i = 0; i < kb / 16; i++)
        md5_update(&m, buf, sizeof(buf));
    md5_final(&m, out);
    t1 = ticks();
    printf("MD5:         %lu KB in %lu ticks = %lu KB/s\n", kb, t1 - t0,
           (unsigned long)(t1 > t0 ? kb * 50 / (t1 - t0) : 0));
    {
        static struct Aes a;
        static struct Gcm g;
        static UBYTE nonce[12];
        aes_setkey(&a, key);
        gcm_setkey(&g, key);
        t0 = ticks();
        for (i = 0; i < kb / 16; i++)
            aes_cmac(&a, buf, sizeof(buf), out);
        t1 = ticks();
        printf("AES-CMAC:    %lu KB in %lu ticks = %lu KB/s\n", kb, t1 - t0,
               (unsigned long)(t1 > t0 ? kb * 50 / (t1 - t0) : 0));
        t0 = ticks();
        for (i = 0; i < kb / 16; i++)
            aes_ccm_encrypt(&a, nonce, nonce, 12, buf, sizeof(buf), out);
        t1 = ticks();
        printf("AES-CCM:     %lu KB in %lu ticks = %lu KB/s\n", kb, t1 - t0,
               (unsigned long)(t1 > t0 ? kb * 50 / (t1 - t0) : 0));
        t0 = ticks();
        for (i = 0; i < kb / 16; i++)
            aes_gcm_encrypt(&g, nonce, nonce, 12, buf, sizeof(buf), out);
        t1 = ticks();
        printf("AES-GCM:     %lu KB in %lu ticks = %lu KB/s\n", kb, t1 - t0,
               (unsigned long)(t1 > t0 ? kb * 50 / (t1 - t0) : 0));
    }
    if (rda) FreeArgs(rda);
    return 0;
}
