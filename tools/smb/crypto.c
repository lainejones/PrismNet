/*
 * PrismNet SMB client: MD4, MD5, SHA-256, HMAC, and for SMB 3.x SHA-512,
 * AES-128 (CMAC, CCM, GCM) and the SP800-108 KDF. Written from the RFCs,
 * FIPS 180-4/197 and NIST SP800-38B/C/D and 108; little/big-endian independent. GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include "crypto.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static c32 le32(const u8 *p) { return p[0] | (c32)p[1] << 8 | (c32)p[2] << 16 | (c32)p[3] << 24; }
static c32 be32(const u8 *p) { return (c32)p[0] << 24 | (c32)p[1] << 16 | (c32)p[2] << 8 | p[3]; }
static void putle(u8 *p, c32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void putbe(u8 *p, c32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* ---- MD4 (only ever used on short inputs: the password hash) --------------- */

static void md4_block(c32 s[4], const u8 *b)
{
    c32 x[16], a = s[0], bb = s[1], c = s[2], d = s[3];
    int i;
    static const int r3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

    for (i = 0; i < 16; i++) x[i] = le32(b + 4 * i);
#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
    for (i = 0; i < 16; i++) {
        static const int sh[4] = { 3, 7, 11, 19 };
        c32 t = a + F(bb, c, d) + x[i];
        a = d; d = c; c = bb; bb = ROL(t, sh[i & 3]);
    }
    for (i = 0; i < 16; i++) {
        static const int sh[4] = { 3, 5, 9, 13 };
        c32 t = a + G(bb, c, d) + x[(i & 3) * 4 + (i >> 2)] + 0x5a827999;
        a = d; d = c; c = bb; bb = ROL(t, sh[i & 3]);
    }
    for (i = 0; i < 16; i++) {
        static const int sh[4] = { 3, 9, 11, 15 };
        c32 t = a + H(bb, c, d) + x[r3[i]] + 0x6ed9eba1;
        a = d; d = c; c = bb; bb = ROL(t, sh[i & 3]);
    }
#undef F
#undef G
#undef H
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d;
}

void md4(const void *data, u32 len, u8 out[16])
{
    c32 s[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const u8 *p = data;
    u8 b[64];
    u32 n = len;
    int i;

    while (n >= 64) { md4_block(s, p); p += 64; n -= 64; }
    memset(b, 0, 64);
    memcpy(b, p, n);
    b[n] = 0x80;
    if (n >= 56) { md4_block(s, b); memset(b, 0, 64); }
    putle(b + 56, (c32)(len << 3));
    putle(b + 60, (c32)((unsigned long long)len >> 29));
    md4_block(s, b);
    for (i = 0; i < 4; i++) putle(out + 4 * i, s[i]);
}

/* ---- MD5 ------------------------------------------------------------------ */

static const c32 md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const u8 md5_r[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(c32 s[4], const u8 *b)
{
    c32 x[16], a = s[0], bb = s[1], c = s[2], d = s[3];
    int i;

    for (i = 0; i < 16; i++) x[i] = le32(b + 4 * i);
    for (i = 0; i < 64; i++) {
        c32 f, t;
        int g;
        if (i < 16)      { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (bb | ~d);       g = (7 * i) & 15; }
        t = d; d = c; c = bb;
        bb = bb + ROL(a + f + md5_k[i] + x[g], md5_r[i]);
        a = t;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d;
}

void md5_init(struct Md5 *c)
{
    c->s[0] = 0x67452301; c->s[1] = 0xefcdab89; c->s[2] = 0x98badcfe; c->s[3] = 0x10325476;
    c->lo = c->hi = 0;
}

void md5_update(struct Md5 *c, const void *data, u32 len)
{
    const u8 *p = data;
    u32 used = c->lo & 63;

    if ((c->lo += (c32)len) < (c32)len) c->hi++;
    while (len) {
        u32 n = 64 - used < len ? 64 - used : len;
        memcpy(c->buf + used, p, n);
        used += n; p += n; len -= n;
        if (used == 64) { md5_block(c->s, c->buf); used = 0; }
    }
}

void md5_final(struct Md5 *c, u8 out[16])
{
    u8 pad[72];
    c32 lo = c->lo << 3, hi = (c->hi << 3) | (c->lo >> 29);
    u32 used = c->lo & 63, n = used < 56 ? 56 - used : 120 - used;
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    md5_update(c, pad, n);
    putle(pad, lo);
    putle(pad + 4, hi);
    md5_update(c, pad, 8);
    for (i = 0; i < 4; i++) putle(out + 4 * i, c->s[i]);
}

/* ---- SHA-256 ---------------------------------------------------------------- */

static const c32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

/* -O2 here: 2.x signing hashes every byte (117 -> 131 KB/s on a 68030);
 * AES is no faster at -O2 than at -Os, so the rest stays small */
__attribute__((optimize("O2")))
static void sha256_block(c32 s[8], const u8 *b)
{
    c32 w[64], a, bb, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
    for (i = 16; i < 64; i++) {
        c32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        c32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s[0]; bb = s[1]; c = s[2]; d = s[3]; e = s[4]; f = s[5]; g = s[6]; h = s[7];
    for (i = 0; i < 64; i++) {
        c32 t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        c32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

void sha256_init(struct Sha256 *c)
{
    static const c32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->s, iv, sizeof(iv));
    c->lo = c->hi = 0;
}

void sha256_update(struct Sha256 *c, const void *data, u32 len)
{
    const u8 *p = data;
    u32 used = c->lo & 63;

    if ((c->lo += (c32)len) < (c32)len) c->hi++;
    while (len) {
        u32 n = 64 - used < len ? 64 - used : len;
        if (!used && len >= 64) {       /* whole blocks straight from the caller */
            sha256_block(c->s, p);
            p += 64; len -= 64;
            continue;
        }
        memcpy(c->buf + used, p, n);
        used += n; p += n; len -= n;
        if (used == 64) { sha256_block(c->s, c->buf); used = 0; }
    }
}

void sha256_final(struct Sha256 *c, u8 out[32])
{
    u8 pad[72];
    c32 lo = c->lo << 3, hi = (c->hi << 3) | (c->lo >> 29);
    u32 used = c->lo & 63, n = used < 56 ? 56 - used : 120 - used;
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    sha256_update(c, pad, n);
    putbe(pad, hi);
    putbe(pad + 4, lo);
    sha256_update(c, pad, 8);
    for (i = 0; i < 8; i++) putbe(out + 4 * i, c->s[i]);
}

/* ---- HMAC ------------------------------------------------------------------- */

void hmac_md5(const u8 *key, u32 klen, const void *d1, u32 l1,
              const void *d2, u32 l2, u8 out[16])
{
    struct Md5 c;
    u8 k[64], pad[64], inner[16];
    int i;

    memset(k, 0, 64);
    if (klen > 64) { md5_init(&c); md5_update(&c, key, klen); md5_final(&c, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    md5_init(&c); md5_update(&c, pad, 64);
    md5_update(&c, d1, l1);
    if (d2 && l2) md5_update(&c, d2, l2);
    md5_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    md5_init(&c); md5_update(&c, pad, 64); md5_update(&c, inner, 16);
    md5_final(&c, out);
}

void hmac_sha256(const u8 *key, u32 klen, const void *d1, u32 l1,
                 const void *d2, u32 l2, u8 out[32])
{
    struct Sha256 c;
    u8 k[64], pad[64], inner[32];
    int i;

    memset(k, 0, 64);
    if (klen > 64) { sha256_init(&c); sha256_update(&c, key, klen); sha256_final(&c, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c); sha256_update(&c, pad, 64);
    sha256_update(&c, d1, l1);
    if (d2 && l2) sha256_update(&c, d2, l2);
    sha256_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* ---- SHA-512 (FIPS 180-4): SMB 3.1.1 pre-auth integrity, handshake only ---- */

static const unsigned long long K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static const unsigned long long H512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};
#define R64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(unsigned long long s[8], const u8 *b)
{
    unsigned long long w[16], a = s[0], bb = s[1], c = s[2], d = s[3],
                       e = s[4], f = s[5], g = s[6], h = s[7], t1, t2;
    int i, j;

    for (i = 0; i < 16; i++)
        for (w[i] = 0, j = 0; j < 8; j++) w[i] = (w[i] << 8) | b[8 * i + j];
    for (i = 0; i < 80; i++) {
        if (i >= 16) {
            unsigned long long w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            w[i & 15] += (R64(w15, 1) ^ R64(w15, 8) ^ (w15 >> 7)) + w[(i + 9) & 15]
                       + (R64(w2, 19) ^ R64(w2, 61) ^ (w2 >> 6));
        }
        t1 = h + (R64(e, 14) ^ R64(e, 18) ^ R64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i & 15];
        t2 = (R64(a, 28) ^ R64(a, 34) ^ R64(a, 39)) + ((a & bb) ^ (a & c) ^ (bb & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

void sha512_init(struct Sha512 *c)
{
    memcpy(c->s, H512, sizeof(H512));
    c->len = 0;
}

void sha512_update(struct Sha512 *c, const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u32 have = (u32)(c->len & 127);

    c->len += len;
    while (len) {
        u32 n = 128 - have < len ? 128 - have : len;
        memcpy(c->buf + have, p, n);
        have += n; p += n; len -= n;
        if (have == 128) { sha512_block(c->s, c->buf); have = 0; }
    }
}

void sha512_final(struct Sha512 *c, u8 out[64])
{
    unsigned long long bits = c->len << 3;
    u32 have = (u32)(c->len & 127);
    int i;

    c->buf[have++] = 0x80;
    if (have > 112) { memset(c->buf + have, 0, 128 - have); sha512_block(c->s, c->buf); have = 0; }
    memset(c->buf + have, 0, 128 - have);
    for (i = 0; i < 8; i++) c->buf[127 - i] = (u8)(bits >> (8 * i));
    sha512_block(c->s, c->buf);
    for (i = 0; i < 64; i++) out[i] = (u8)(c->s[i >> 3] >> (8 * (7 - (i & 7))));
}

/* ---- AES-128, encryption direction only (FIPS 197) ---------------------------
 * CMAC, CCM and GCM only ever run the forward cipher. The S-box and the four
 * round tables are computed on first use (4.25 KB of BSS, nothing on disk).
 * The state stays in 32-bit words across the blocks of a message, so a
 * block costs four word loads instead of sixteen byte operations. */

static u8  sbox[256];
static c32 T0[256], T1[256], T2[256], T3[256];

/* message words: the 68000 loads words only from even addresses; the 68k
 * is big-endian, so an aligned load is already in the right order */
#if defined(__m68k__) || defined(__mc68000__) || defined(mc68000)
#define LD32(p)    (((u32)(p) & 1) ? be32(p) : *(const c32 *)(p))
#define ST32(p, v) do { if ((u32)(p) & 1) putbe(p, v); else *(c32 *)(p) = (v); } while (0)
#else
#define LD32(p)    be32(p)
#define ST32(p, v) putbe(p, v)
#endif

static void aes_tables(void)
{
    u32 p = 1, q = 1, i;

    do {
        u32 x;
        p = (p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0)) & 0xff;      /* p * 3 */
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xff;       /* q / 3 */
        if (q & 0x80) q ^= 0x09;
        x = q ^ (q << 1) ^ (q << 2) ^ (q << 3) ^ (q << 4);      /* affine part: */
        x = (x ^ (x >> 8)) & 0xff;                              /* rotations folded */
        sbox[p] = (u8)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (i = 0; i < 256; i++) {
        c32 s = sbox[i], s2 = ((s << 1) ^ (s & 0x80 ? 0x1b : 0)) & 0xff, t;
        t = s2 << 24 | s << 16 | s << 8 | (s2 ^ s);
        T0[i] = t;
        T1[i] = ROR(t, 8);
        T2[i] = ROR(t, 16);
        T3[i] = ROR(t, 24);
    }
}

#define TE(a, b, c, d, k) (T0[(a) >> 24] ^ T1[((b) >> 16) & 0xff] ^ \
                           T2[((c) >> 8) & 0xff] ^ T3[(d) & 0xff] ^ (k))
#define SB(a, b, c, d, k) (((c32)sbox[(a) >> 24] << 24 | (c32)sbox[((b) >> 16) & 0xff] << 16 | \
                            (c32)sbox[((c) >> 8) & 0xff] << 8 | sbox[(d) & 0xff]) ^ (k))

/* encrypt one block held as four big-endian words, in place */
static void aes_words(const struct Aes *a, c32 st[4])
{
    const c32 *rk = a->rk;
    c32 s0 = st[0] ^ rk[0], s1 = st[1] ^ rk[1], s2 = st[2] ^ rk[2], s3 = st[3] ^ rk[3];
    c32 t0, t1, t2, t3;
    int r;

    for (r = 0; r < 4; r++) {                   /* rounds 1-8, two at a time */
        t0 = TE(s0, s1, s2, s3, rk[4]);
        t1 = TE(s1, s2, s3, s0, rk[5]);
        t2 = TE(s2, s3, s0, s1, rk[6]);
        t3 = TE(s3, s0, s1, s2, rk[7]);
        s0 = TE(t0, t1, t2, t3, rk[8]);
        s1 = TE(t1, t2, t3, t0, rk[9]);
        s2 = TE(t2, t3, t0, t1, rk[10]);
        s3 = TE(t3, t0, t1, t2, rk[11]);
        rk += 8;
    }
    t0 = TE(s0, s1, s2, s3, rk[4]);             /* round 9 */
    t1 = TE(s1, s2, s3, s0, rk[5]);
    t2 = TE(s2, s3, s0, s1, rk[6]);
    t3 = TE(s3, s0, s1, s2, rk[7]);
    st[0] = SB(t0, t1, t2, t3, rk[8]);          /* round 10 */
    st[1] = SB(t1, t2, t3, t0, rk[9]);
    st[2] = SB(t2, t3, t0, t1, rk[10]);
    st[3] = SB(t3, t0, t1, t2, rk[11]);
}

/* x ^= the n bytes at p (n < 16: zero padded) */
static void xor_in(c32 x[4], const u8 *p, u32 n)
{
    u8 b[16];

    if (n == 16) {
        x[0] ^= LD32(p); x[1] ^= LD32(p + 4); x[2] ^= LD32(p + 8); x[3] ^= LD32(p + 12);
        return;
    }
    memset(b, 0, 16);
    memcpy(b, p, n);
    x[0] ^= be32(b); x[1] ^= be32(b + 4); x[2] ^= be32(b + 8); x[3] ^= be32(b + 12);
}

/* the n bytes at d ^= the key stream block ks */
static void xor_out(u8 *d, const c32 ks[4], u32 n)
{
    u8 b[16];
    u32 i;

    if (n == 16) {
        ST32(d, LD32(d) ^ ks[0]);
        ST32(d + 4, LD32(d + 4) ^ ks[1]);
        ST32(d + 8, LD32(d + 8) ^ ks[2]);
        ST32(d + 12, LD32(d + 12) ^ ks[3]);
        return;
    }
    putbe(b, ks[0]); putbe(b + 4, ks[1]); putbe(b + 8, ks[2]); putbe(b + 12, ks[3]);
    for (i = 0; i < n; i++) d[i] ^= b[i];
}

static void dbl(c32 k[4])
{
    c32 carry = k[0] >> 31;
    k[0] = k[0] << 1 | k[1] >> 31;
    k[1] = k[1] << 1 | k[2] >> 31;
    k[2] = k[2] << 1 | k[3] >> 31;
    k[3] = k[3] << 1 ^ (carry ? 0x87 : 0);
}

void aes_setkey(struct Aes *a, const u8 key[16])
{
    c32 *rk = a->rk, rcon = 0x01;
    int i;

    if (!T0[0]) aes_tables();
    for (i = 0; i < 4; i++) rk[i] = be32(key + 4 * i);
    for (i = 4; i < 44; i++) {
        c32 t = rk[i - 1];
        if (!(i & 3)) {
            t = (c32)sbox[(t >> 16) & 0xff] << 24 | (c32)sbox[(t >> 8) & 0xff] << 16 |
                (c32)sbox[t & 0xff] << 8 | sbox[t >> 24];
            t ^= rcon << 24;
            rcon = ((rcon << 1) ^ (rcon & 0x80 ? 0x1b : 0)) & 0xff;
        }
        rk[i] = rk[i - 4] ^ t;
    }
    /* the CMAC subkeys K1 and K2 (RFC 4493 2.3), once per key */
    memset(a->k1, 0, 16);
    aes_words(a, a->k1);
    dbl(a->k1);
    memcpy(a->k2, a->k1, 16);
    dbl(a->k2);
}

void aes_encrypt(const struct Aes *a, const u8 in[16], u8 out[16])
{
    c32 st[4];

    st[0] = be32(in); st[1] = be32(in + 4); st[2] = be32(in + 8); st[3] = be32(in + 12);
    aes_words(a, st);
    putbe(out, st[0]); putbe(out + 4, st[1]); putbe(out + 8, st[2]); putbe(out + 12, st[3]);
}

/* ---- AES-CMAC (RFC 4493): SMB 3.x signing --------------------------------- */

void aes_cmac(const struct Aes *a, const void *data, u32 len, u8 out[16])
{
    const u8 *p = (const u8 *)data;
    const c32 *k = a->k1;
    c32 x[4];
    u8 last[16];

    x[0] = x[1] = x[2] = x[3] = 0;
    while (len > 16) {
        xor_in(x, p, 16);
        aes_words(a, x);
        p += 16; len -= 16;
    }
    memset(last, 0, 16);
    memcpy(last, p, len);
    if (len < 16) {                             /* partial (or empty) last block */
        last[len] = 0x80;
        k = a->k2;
    }
    xor_in(x, last, 16);
    x[0] ^= k[0]; x[1] ^= k[1]; x[2] ^= k[2]; x[3] ^= k[3];
    aes_words(a, x);
    putbe(out, x[0]); putbe(out + 4, x[1]); putbe(out + 8, x[2]); putbe(out + 12, x[3]);
}

/* ---- SP800-108 counter-mode KDF, HMAC-SHA256, one 128-bit key ------------- */

void smb3_kdf(const u8 key[16], const void *label, u32 llen, const void *ctx, u32 clen,
              u8 out[16])
{
    u8 in[4 + 32 + 1 + 64 + 4], mac[32];
    u32 n;

    putbe(in, 1); n = 4;
    memcpy(in + n, label, llen); n += llen;
    in[n++] = 0;
    memcpy(in + n, ctx, clen); n += clen;
    putbe(in + n, 128); n += 4;
    hmac_sha256(key, 16, in, n, 0, 0, mac);
    memcpy(out, mac, 16);
}

/* ---- AES-128-CCM, 11-byte nonce, 16-byte tag (SP800-38C; SMB 3.0.x) ------- */

static void ccm_mac(const struct Aes *a, const u8 *nonce, const u8 *aad, u32 alen,
                    const u8 *m, u32 mlen, c32 x[4])
{
    u8 b[16];
    u32 i, n;

    b[0] = 0x40 | (7 << 3) | 3;                 /* Adata, t = 16, q = 4 */
    memcpy(b + 1, nonce, 11);
    putbe(b + 12, mlen);
    x[0] = x[1] = x[2] = x[3] = 0;
    xor_in(x, b, 16);
    aes_words(a, x);
    memset(b, 0, 16);                           /* aad after its 2-byte length, */
    b[0] = (u8)(alen >> 8); b[1] = (u8)alen;    /* zero padded to whole blocks */
    n = 2;
    for (i = 0; i < alen; i++) {
        b[n++] = aad[i];
        if (n == 16) { xor_in(x, b, 16); aes_words(a, x); n = 0; memset(b, 0, 16); }
    }
    if (n) { xor_in(x, b, 16); aes_words(a, x); }
    while (mlen) {
        n = mlen < 16 ? mlen : 16;
        xor_in(x, m, n);
        aes_words(a, x);
        m += n; mlen -= n;
    }
}

static void ccm_ctr_init(c32 ctr[4], const u8 *nonce)
{
    u8 b[16];
    b[0] = 3;                                   /* q - 1 */
    memcpy(b + 1, nonce, 11);
    ctr[0] = be32(b); ctr[1] = be32(b + 4); ctr[2] = be32(b + 8);
}

/* key stream blocks 1, 2, ... over d */
static void ccm_ctr(const struct Aes *a, const u8 *nonce, u8 *d, u32 len)
{
    c32 ctr[4], ks[4];
    u32 n, c = 1;

    ccm_ctr_init(ctr, nonce);
    while (len) {
        ks[0] = ctr[0]; ks[1] = ctr[1]; ks[2] = ctr[2]; ks[3] = c++;
        aes_words(a, ks);
        n = len < 16 ? len : 16;
        xor_out(d, ks, n);
        d += n; len -= n;
    }
}

/* tag = x ^ the key stream block 0 */
static void ccm_tag(const struct Aes *a, const u8 *nonce, const c32 x[4], u8 tag[16])
{
    c32 s[4];
    ccm_ctr_init(s, nonce);
    s[3] = 0;
    aes_words(a, s);
    putbe(tag, x[0] ^ s[0]); putbe(tag + 4, x[1] ^ s[1]);
    putbe(tag + 8, x[2] ^ s[2]); putbe(tag + 12, x[3] ^ s[3]);
}

void aes_ccm_encrypt(const struct Aes *a, const u8 *nonce, const u8 *aad, u32 alen,
                     u8 *data, u32 len, u8 tag[16])
{
    c32 x[4];
    ccm_mac(a, nonce, aad, alen, data, len, x);
    ccm_tag(a, nonce, x, tag);
    ccm_ctr(a, nonce, data, len);
}

int aes_ccm_decrypt(const struct Aes *a, const u8 *nonce, const u8 *aad, u32 alen,
                    u8 *data, u32 len, const u8 tag[16])
{
    c32 x[4];
    u8 t[16];
    ccm_ctr(a, nonce, data, len);
    ccm_mac(a, nonce, aad, alen, data, len, x);
    ccm_tag(a, nonce, x, t);
    return memcmp(t, tag, 16) ? -1 : 0;
}

/* ---- AES-128-GCM, 12-byte nonce (SP800-38D; SMB 3.1.1) ----------------------
 * GHASH with Shoup's 4-bit tables (16 multiples of H, 256 bytes per key),
 * all in 32-bit words: 64-bit shifts are slow on a 68000. */

void gcm_setkey(struct Gcm *g, const u8 key[16])
{
    c32 (*m)[4] = g->m;
    int i, j;

    aes_setkey(&g->aes, key);
    memset(m, 0, sizeof(g->m));
    aes_words(&g->aes, m[8]);                   /* H = E(0) */
    for (i = 4; i > 0; i >>= 1) {               /* M[i] = M[2i] * x (a right shift) */
        const c32 *s = m[2 * i];
        c32 r = (s[3] & 1) ? 0xe1000000UL : 0;
        m[i][3] = s[3] >> 1 | s[2] << 31;
        m[i][2] = s[2] >> 1 | s[1] << 31;
        m[i][1] = s[1] >> 1 | s[0] << 31;
        m[i][0] = s[0] >> 1 ^ r;
    }
    for (i = 2; i <= 8; i *= 2)
        for (j = 1; j < i; j++) {
            m[i + j][0] = m[i][0] ^ m[j][0];
            m[i + j][1] = m[i][1] ^ m[j][1];
            m[i + j][2] = m[i][2] ^ m[j][2];
            m[i + j][3] = m[i][3] ^ m[j][3];
        }
}

static const unsigned short last4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
};

/* z = z * x^4 (reduced), then z ^= M[n] */
#define G4(n) do { \
        c32 rem = z3 & 15; \
        z3 = z3 >> 4 | z2 << 28; z2 = z2 >> 4 | z1 << 28; z1 = z1 >> 4 | z0 << 28; \
        z0 = z0 >> 4 ^ (c32)last4[rem] << 16; \
        z0 ^= m[n][0]; z1 ^= m[n][1]; z2 ^= m[n][2]; z3 ^= m[n][3]; \
    } while (0)

/* x = x * H, x as four big-endian words */
static void gmul(const struct Gcm *g, c32 x[4])
{
    const c32 (*m)[4] = (const c32 (*)[4])g->m;
    c32 z0, z1, z2, z3, v = x[3];
    u32 b = v & 0xff;
    int w, k;

    z0 = m[b & 15][0]; z1 = m[b & 15][1]; z2 = m[b & 15][2]; z3 = m[b & 15][3];
    G4(b >> 4);
    v >>= 8;
    for (w = 3, k = 1; w >= 0; w--, k = 0) {    /* bytes 14 down to 0 */
        if (k == 0) v = x[w];
        for (; k < 4; k++, v >>= 8) {
            b = v & 0xff;
            G4(b & 15);
            G4(b >> 4);
        }
    }
    x[0] = z0; x[1] = z1; x[2] = z2; x[3] = z3;
}

static void gcm_run(const struct Gcm *g, const u8 *nonce, const u8 *aad, u32 alen,
                    u8 *data, u32 len, int enc, u8 tag[16])
{
    c32 j[4], ks[4], y[4];
    u32 n, c = 2, left = len, abits = alen << 3;
    u8 *p = data;

    j[0] = be32(nonce); j[1] = be32(nonce + 4); j[2] = be32(nonce + 8); j[3] = 1;
    y[0] = y[1] = y[2] = y[3] = 0;
    while (alen) {
        n = alen < 16 ? alen : 16;
        xor_in(y, aad, n);
        gmul(g, y);
        aad += n; alen -= n;
    }
    while (left) {
        n = left < 16 ? left : 16;
        if (!enc) { xor_in(y, p, n); gmul(g, y); }
        ks[0] = j[0]; ks[1] = j[1]; ks[2] = j[2]; ks[3] = c++;
        aes_words(&g->aes, ks);
        xor_out(p, ks, n);
        if (enc) { xor_in(y, p, n); gmul(g, y); }
        p += n; left -= n;
    }
    y[1] ^= abits;                              /* bit lengths, 64 bits each */
    y[3] ^= len << 3;
    gmul(g, y);
    aes_words(&g->aes, j);                      /* E(J0) */
    putbe(tag, y[0] ^ j[0]); putbe(tag + 4, y[1] ^ j[1]);
    putbe(tag + 8, y[2] ^ j[2]); putbe(tag + 12, y[3] ^ j[3]);
}

void aes_gcm_encrypt(const struct Gcm *g, const u8 *nonce, const u8 *aad, u32 alen,
                     u8 *data, u32 len, u8 tag[16])
{
    gcm_run(g, nonce, aad, alen, data, len, 1, tag);
}

int aes_gcm_decrypt(const struct Gcm *g, const u8 *nonce, const u8 *aad, u32 alen,
                    u8 *data, u32 len, const u8 tag[16])
{
    u8 t[16];
    gcm_run(g, nonce, aad, alen, data, len, 0, t);
    return memcmp(t, tag, 16) ? -1 : 0;
}
