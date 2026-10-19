/*
 * cryptotest - published test vectors for tools/smb/crypto.c, run on the PC
 * (make cryptotest) and joinable to the gate. Exit 0 = all pass.
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"

static int fails;

static void check(const char *what, const u8 *got, u32 n, const char *hex)
{
    char buf[400];
    u32 i;
    for (i = 0; i < n; i++) sprintf(buf + 2 * i, "%02x", got[i]);
    if (strcmp(buf, hex)) { printf("FAIL %s: %s\n", what, buf); fails++; }
    else printf("ok   %s\n", what);
}

/* ASCII -> UTF-16LE */
static u32 utf16(const char *s, u8 *out)
{
    u32 n = 0;
    while (*s) { out[n++] = (u8)*s++; out[n++] = 0; }
    return n;
}

int main(void)
{
    u8 out[32], key[32], u[64], nthash[16];
    struct Sha256 c;
    u32 n;
    int i;

    md4("abc", 3, out);
    check("MD4(abc) RFC 1320", out, 16, "a448017aaf21d8525fc10ae87aa6729d");
    md4("", 0, out);
    check("MD4('') RFC 1320", out, 16, "31d6cfe0d16ae931b73c59d7e0c089c0");
    {
        struct Md5 m;
        md5_init(&m); md5_update(&m, "abc", 3); md5_final(&m, out);
        check("MD5(abc) RFC 1321", out, 16, "900150983cd24fb0d6963f7d28e17f72");
    }
    sha256_init(&c); sha256_update(&c, "abc", 3); sha256_final(&c, out);
    check("SHA256(abc) FIPS 180-4", out, 32,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256_init(&c);
    sha256_update(&c, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    sha256_final(&c, out);
    check("SHA256(448-bit) FIPS 180-4", out, 32,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        static u8 a[1000];
        memset(a, 'a', sizeof(a));
        sha256_init(&c);
        for (i = 0; i < 1000; i++) sha256_update(&c, a, 1000);   /* 10^6 'a' */
        sha256_final(&c, out);
        check("SHA256(million a) FIPS 180-4", out, 32,
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    memset(key, 0x0b, 16);
    hmac_md5(key, 16, "Hi There", 8, 0, 0, out);
    check("HMAC-MD5 RFC 2104 #1", out, 16, "9294727a3638bb1c13f48ef8158bfc9d");
    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi ", 3, "There", 5, out);    /* two pieces */
    check("HMAC-SHA256 RFC 4231 #1", out, 32,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_sha256((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, 0, 0, out);
    check("HMAC-SHA256 RFC 4231 #2", out, 32,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* MS-NLMP 4.2.4.1.1: NTOWFv2("Password", "User", "Domain") */
    n = utf16("Password", u);
    md4(u, n, nthash);
    check("NT hash MS-NLMP 4.2.2.1.2", nthash, 16, "a4f49c406510bdcab6824ee7c30fd852");
    n = utf16("USER", u);
    n += utf16("Domain", u + n);
    hmac_md5(nthash, 16, u, n, 0, 0, out);
    check("NTOWFv2 MS-NLMP 4.2.4.1.1", out, 16, "0c868a403bfd7a93a3001ef22ef02e3f");

    /* MS-NLMP 4.2.4.2.2 / 4.2.4.1.2: NTProofStr and SessionBaseKey for
     * server challenge 0123456789abcdef, client challenge aa..aa, time 0,
     * AvPairs NbDomainName "Domain", NbComputerName "Server", EOL - with
     * the blob laid out exactly as smb2.c's ntlm_auth() builds it */
    {
        static const u8 schal[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
        u8 blob[128], proof[16], key[16], ntowf[16];
        u32 bl = 0, t;
        memcpy(ntowf, out, 16);                 /* NTOWFv2 from just above */
        memset(blob, 0, sizeof(blob));
        blob[0] = 1; blob[1] = 1;               /* 0..7: version, zeros; 8..15: time 0 */
        memset(blob + 16, 0xaa, 8);             /* client challenge */
        bl = 28;                                /* 24..27: zero */
        blob[bl++] = 2; blob[bl++] = 0; blob[bl++] = 12; blob[bl++] = 0;
        bl += utf16("Domain", blob + bl);
        blob[bl++] = 1; blob[bl++] = 0; blob[bl++] = 12; blob[bl++] = 0;
        bl += utf16("Server", blob + bl);
        bl += 4;                                /* MsvAvEOL */
        bl += 4;                                /* the trailing zero ntlm_auth adds */
        t = bl;
        hmac_md5(ntowf, 16, schal, 8, blob, t, proof);
        check("NTProofStr MS-NLMP 4.2.4.2.2", proof, 16, "68cd0ab851e51c96aabc927bebef6a1c");
        hmac_md5(ntowf, 16, proof, 16, 0, 0, key);
        check("SessionBaseKey MS-NLMP 4.2.4.1.2", key, 16, "8de40ccadbc14a82f15cb0ad0de95ca3");
    }

    /* ---- SMB 3.x pieces ---- */
    {
        struct Sha512 c5;
        u8 h[64];
        sha512_init(&c5); sha512_update(&c5, "abc", 3); sha512_final(&c5, h);
        check("SHA512(abc) FIPS 180-4", h, 64,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
        sha512_init(&c5);
        sha512_update(&c5, "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn", 56);
        sha512_update(&c5, "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 56);
        sha512_final(&c5, h);
        check("SHA512(896-bit) FIPS 180-4", h, 64,
              "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    }
    {
        static const u8 k197[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
        static const u8 p197[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                     0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
        static const u8 k4493[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                                      0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
        static const u8 m4493[16] = { 0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
                                      0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a };
        static u8 msg[77], aad[32], nonce[12], tag[16], z[16];
        static struct Aes a;
        static struct Gcm g;
        u8 b[16];
        aes_setkey(&a, k197);
        aes_encrypt(&a, p197, b);
        check("AES-128 FIPS 197 C.1", b, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
        aes_setkey(&a, k4493);
        aes_cmac(&a, "", 0, b);
        check("AES-CMAC RFC 4493 #1 (empty)", b, 16, "bb1d6929e95937287fa37d129b756746");
        aes_cmac(&a, m4493, 16, b);
        check("AES-CMAC RFC 4493 #2 (16 bytes)", b, 16, "070a16b46b4d4144f79bdd9dd04a287c");
        /* odd lengths, checked against Python's cryptography package */
        for (i = 0; i < 77; i++) msg[i] = (u8)(i * 7 + 3);
        for (i = 0; i < 32; i++) aad[i] = (u8)(0x40 + i);
        for (i = 0; i < 12; i++) nonce[i] = (u8)(0x10 + i);
        aes_setkey(&a, k197);
        aes_cmac(&a, msg, 77, b);
        check("AES-CMAC 77 bytes", b, 16, "e9098ace740b0d6f1b9b5871ef6bc368");
        aes_ccm_encrypt(&a, nonce, aad, 32, msg, 77, tag);
        check("AES-CCM 77 bytes: data", msg, 77, "4f656cc3df9b7c9fb54ea033753dfbc2521259d1c04be74b41f23fc35a360736ea208542bc4462748351e8b3d0751f2b27a41a3a67812559c6aeafe2fbb707fd0a8d7d9c5a3f5c784be74db001");
        check("AES-CCM 77 bytes: tag", tag, 16, "3c38d9057bbc229f1bf78e23939dbeda");
        n = aes_ccm_decrypt(&a, nonce, aad, 32, msg, 77, tag);
        check("AES-CCM decrypt", msg, 4, "030a1118");
        if (n) { printf("FAIL AES-CCM tag check\n"); fails++; }
        tag[0] ^= 1;
        aes_ccm_encrypt(&a, nonce, aad, 32, msg, 77, z);       /* re-encrypt, then */
        if (!aes_ccm_decrypt(&a, nonce, aad, 32, msg, 77, tag)) /* a bad tag must fail */
            { printf("FAIL AES-CCM accepted a bad tag\n"); fails++; }
        else printf("ok   AES-CCM refuses a bad tag\n");
        for (i = 0; i < 77; i++) msg[i] = (u8)(i * 7 + 3);
        gcm_setkey(&g, k197);
        aes_gcm_encrypt(&g, nonce, aad, 32, msg, 77, tag);
        check("AES-GCM 77 bytes: data", msg, 77, "c72412b710699bdb2c9f14a590798e5249c6f50fb962f61b2e7990d1a0cbc4713133d0c77ea0f608f882fcf8edee1457dae2904396b5c66ce087b416203ef3249d92c37cfd71906f35827350ba");
        check("AES-GCM 77 bytes: tag", tag, 16, "55775636e45bca0cee2c3f66dfc79352");
        if (aes_gcm_decrypt(&g, nonce, aad, 32, msg, 77, tag)) { printf("FAIL AES-GCM tag check\n"); fails++; }
        check("AES-GCM decrypt", msg, 4, "030a1118");
        /* the GCM spec's test case 2: K = 0, P = 0^128, IV = 0^96 */
        memset(z, 0, 16); memset(nonce, 0, 12); memset(b, 0, 16);
        gcm_setkey(&g, z);
        aes_gcm_encrypt(&g, nonce, NULL, 0, b, 16, tag);
        check("AES-GCM spec test case 2: C", b, 16, "0388dace60b6a392f328c2b971b2fe78");
        check("AES-GCM spec test case 2: T", tag, 16, "ab6e47d42cec13bdf53a67b21257bddf");
    }
    {
        /* the same at an odd address: the 68k word-load path must fall back */
        static u8 odd[1 + 77 + 3];
        static struct Aes a;
        static struct Gcm g;
        static const u8 k197[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
        u8 b[16], nonce[12], aad[32], tag[16];
        for (i = 0; i < 77; i++) odd[1 + i] = (u8)(i * 7 + 3);
        for (i = 0; i < 32; i++) aad[i] = (u8)(0x40 + i);
        for (i = 0; i < 12; i++) nonce[i] = (u8)(0x10 + i);
        aes_setkey(&a, k197);
        aes_cmac(&a, odd + 1, 77, b);
        check("AES-CMAC 77 bytes, odd address", b, 16, "e9098ace740b0d6f1b9b5871ef6bc368");
        gcm_setkey(&g, k197);
        aes_gcm_encrypt(&g, nonce, aad, 32, odd + 1, 77, tag);
        check("AES-GCM 77 bytes, odd address: tag", tag, 16, "55775636e45bca0cee2c3f66dfc79352");
        check("AES-GCM 77 bytes, odd address: data", odd + 1, 16, "c72412b710699bdb2c9f14a590798e52");
    }
    {
        u8 ki[16], k[16];
        for (i = 0; i < 16; i++) ki[i] = (u8)(0xa0 + i);
        smb3_kdf(ki, "SMB2AESCMAC", 12, "SmbSign", 8, k);
        check("SMB 3.0 KDF (signing key)", k, 16, "e0c4a33850c06397dfc4c9d19257f4a5");
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
