/*
 * PrismNet SMB client: the hashes SMB2 and NTLMv2 need, plain portable C.
 * MD4 (RFC 1320), MD5 (RFC 1321), HMAC (RFC 2104), SHA-256/512 (FIPS 180-4),
 * AES-128 (FIPS 197) with CMAC (RFC 4493), CCM and GCM, SP800-108 KDF.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef SMB_CRYPTO_H
#define SMB_CRYPTO_H

#include "nettypes.h"

typedef unsigned int c32;            /* exactly 32 bits on the Amiga and the PC */

struct Md5 { c32 s[4]; c32 lo, hi; u8 buf[64]; };
struct Sha256 { c32 s[8]; c32 lo, hi; u8 buf[64]; };

void md4(const void *data, u32 len, u8 out[16]);

void md5_init(struct Md5 *c);
void md5_update(struct Md5 *c, const void *data, u32 len);
void md5_final(struct Md5 *c, u8 out[16]);

void sha256_init(struct Sha256 *c);
void sha256_update(struct Sha256 *c, const void *data, u32 len);
void sha256_final(struct Sha256 *c, u8 out[32]);

/* HMAC over up to two pieces (the second may be NULL/0) */
void hmac_md5(const u8 *key, u32 klen, const void *d1, u32 l1,
              const void *d2, u32 l2, u8 out[16]);
void hmac_sha256(const u8 *key, u32 klen, const void *d1, u32 l1,
                 const void *d2, u32 l2, u8 out[32]);

/* ---- SMB 3.x ---- */
struct Sha512 { unsigned long long s[8], len; u8 buf[128]; };
void sha512_init(struct Sha512 *c);
void sha512_update(struct Sha512 *c, const void *data, u32 len);
void sha512_final(struct Sha512 *c, u8 out[64]);

struct Aes { c32 rk[44], k1[4], k2[4]; };     /* round keys, CMAC subkeys */
void aes_setkey(struct Aes *a, const u8 key[16]);
void aes_encrypt(const struct Aes *a, const u8 in[16], u8 out[16]);
void aes_cmac(const struct Aes *a, const void *data, u32 len, u8 out[16]);

/* SP800-108 counter mode, HMAC-SHA256, L = 128; label and context exactly
 * as SMB passes them (their own trailing NULs counted in llen/clen) */
void smb3_kdf(const u8 key[16], const void *label, u32 llen, const void *ctx, u32 clen,
              u8 out[16]);

/* in place; nonce 11 bytes (CCM) or 12 (GCM), tag 16; decrypt: 0 or -1 */
void aes_ccm_encrypt(const struct Aes *a, const u8 *nonce, const u8 *aad, u32 alen,
                     u8 *data, u32 len, u8 tag[16]);
int  aes_ccm_decrypt(const struct Aes *a, const u8 *nonce, const u8 *aad, u32 alen,
                     u8 *data, u32 len, const u8 tag[16]);
struct Gcm { struct Aes aes; c32 m[16][4]; };  /* key, the multiples of H */
void gcm_setkey(struct Gcm *g, const u8 key[16]);
void aes_gcm_encrypt(const struct Gcm *g, const u8 *nonce, const u8 *aad, u32 alen,
                     u8 *data, u32 len, u8 tag[16]);
int  aes_gcm_decrypt(const struct Gcm *g, const u8 *nonce, const u8 *aad, u32 alen,
                     u8 *data, u32 len, const u8 tag[16]);

#endif
