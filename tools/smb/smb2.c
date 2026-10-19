/*
 * PrismNet SMB client: SMB 2.0.2 to 3.1.1, NTLMv2 in SPNEGO, signing
 * (HMAC-SHA256 or AES-CMAC) and AES-128-CCM/GCM encryption. Written from
 * [MS-SMB2], [MS-NLMP] and [MS-SPNG] (RFC 4178). GPL-3.0-only, see LICENSE.
 *
 * tx layout: [NBSS 4][room for a transform header 52][SMB2 header 64][body]
 * - unencrypted frames go out from tx+52 with the NBSS header moved there.
 * A compound request is several header+body units, 8-byte aligned, linked
 * by NextCommand; each is signed on its own, or the whole frame encrypted.
 * rx holds one reply frame, decrypted in place to the start of rx.
 */
#include <string.h>
#include "smb2.h"
#include "crypto.h"
#include "net.h"

/* commands */
#define C_NEGOTIATE      0
#define C_SESSION_SETUP  1
#define C_TREE_CONNECT   3
#define C_CREATE         5
#define C_CLOSE          6
#define C_FLUSH          7
#define C_READ           8
#define C_WRITE          9
#define C_SET_INFO       0x11
#define C_QUERY_DIR      0x0e
#define C_QUERY_INFO     0x10

#define F_REPLY    0x01
#define F_ASYNC    0x02
#define F_RELATED  0x04
#define F_SIGNED   0x08

#define ST_OK                  0x00000000UL
#define ST_PENDING             0x00000103UL
#define ST_MORE_PROCESSING     0xC0000016UL
#define ST_NO_MORE_FILES       0x80000006UL
#define ST_END_OF_FILE         0xC0000011UL

#define HDR 64
#ifdef SMB_TRACE                                /* host builds: dump every frame */
#include <stdio.h>
static void trace(const char *dir, const u8 *p, u32 n)
{
    u32 i;
    fprintf(stderr, "%s %lu bytes", dir, (unsigned long)n);
    for (i = 0; i < n && i < 256; i++) fprintf(stderr, "%s%02x", i % 32 ? " " : "\n  ", p[i]);
    fprintf(stderr, "\n");
}
#define TRACE(d, p, n) trace(d, p, n)
#else
#define TRACE(d, p, n)
#endif
#define TXH 52                                  /* SMB2 TRANSFORM_HEADER */

static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32  g16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32  g32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* ---- names ---------------------------------------------------------------- */

u32 smb2_fnv(const char *s)
{
    u32 h = 2166136261UL;
    while (*s) {
        u32 c = (u8)*s++;
        if (c >= 'A' && c <= 'Z') c += 32;      /* case-insensitive server */
        h = ((h ^ c) * 16777619UL) & 0xffffffffUL;
    }
    return h ? h : 1;
}

u32 utf8_to_utf16(const char *s, u8 *out, u32 max)
{
    const u8 *p = (const u8 *)s;
    u32 n = 0;

    while (*p && n + 2 <= max) {
        u32 c = *p++;
        if (c >= 0x80) {
            u32 more = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 0;
            c &= more == 1 ? 0x1f : 0x0f;
            while (more-- && (*p & 0xc0) == 0x80) c = (c << 6) | (*p++ & 0x3f);
        }
        if (c == '/') c = '\\';
        out[n++] = (u8)c;
        out[n++] = (u8)(c >> 8);
    }
    return n;
}

u32 utf16_to_utf8(const u8 *s, u32 bytes, char *out, u32 max)
{
    u32 i, n = 0;

    for (i = 0; i + 1 < bytes; i += 2) {
        u32 c = s[i] | (u32)s[i + 1] << 8;
        if (c >= 0xd800 && c < 0xe000) c = '?'; /* outside the BMP */
        if (c < 0x80) {
            if (n + 1 >= max) break;
            out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 >= max) break;
            out[n++] = (char)(0xc0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (n + 3 >= max) break;
            out[n++] = (char)(0xe0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[n++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[n] = 0;
    return n;
}

/* ---- status -> filesystem errors ----------------------------------------- */

static long map_status(u32 st)
{
    switch (st) {
    case 0xC0000034UL: case 0xC000003AUL: case 0xC000000FUL: return NFS3ERR_NOENT;
    case 0xC0000022UL: case 0xC0000043UL:                    return NFS3ERR_ACCES;
    case 0xC0000103UL:                                       return NFS3ERR_NOTDIR;
    case 0xC00000BAUL:                                       return NFS3ERR_ISDIR;
    case 0xC0000035UL:                                       return NFS3ERR_EXIST;
    case 0xC0000101UL:                                       return NFS3ERR_NOTEMPTY;
    case 0xC000007FUL:                                       return NFS3ERR_NOSPC;
    case 0xC00000A2UL:                                       return NFS3ERR_ROFS;
    case 0xC0000033UL:                                       return NFS3ERR_NAMETOOLONG;
    case 0xC000006DUL: case 0xC000006EUL: case 0xC0000072UL: return FS_EDENIED;
    case 0xC00000CCUL:                                       return FS_EUNSUPP;   /* no such share */
    case 0xC0000203UL: case 0xC00000C9UL: case 0xC000035CUL: return FS_EPROTO;    /* session gone */
    default:                                                 return NFS3ERR_IO;
    }
}

/* ---- framing -------------------------------------------------------------- */

void smb2_init(struct Smb *s, u8 *tx, u32 txsize, u8 *rx, u32 rxsize)
{
    memset(s, 0, sizeof(*s));
    s->sock = -1;
    s->tx = tx; s->txsize = txsize;
    s->rx = rx; s->rxsize = rxsize;
    s->timeout = 15;
}

void smb2_close(struct Smb *s)
{
    if (s->sock >= 0)
        net_close(s->sock);
    s->sock = -1;
}

/* start a request unit at tx+off; returns the body pointer */
static u8 *unit(struct Smb *s, u32 off, u32 cmd, int related)
{
    u8 *h = s->tx + 4 + TXH + off;

    memset(h, 0, HDR + 64);                     /* and the fixed part of any body */
    h[0] = 0xfe; h[1] = 'S'; h[2] = 'M'; h[3] = 'B';
    p16(h + 4, HDR);
    p16(h + 6, s->dialect >= 0x210 ? 1 : 0);   /* credit charge */
    p16(h + 12, cmd);
    p16(h + 14, 64);                            /* credits asked for */
    p32(h + 16, related ? F_RELATED : 0);
    p32(h + 24, s->msgid_lo);
    p32(h + 28, s->msgid_hi);
    if (++s->msgid_lo == 0) s->msgid_hi++;
    p32(h + 36, s->treeid);
    memcpy(h + 40, s->sessid, 8);
    return h + HDR;
}

/* the 16-byte signature of a message (its signature field already zero) */
static void mac16(struct Smb *s, const u8 *h, u32 len, u8 out[16])
{
    if (s->dialect >= 0x300)
        aes_cmac(&s->signkey, h, len, out);
    else {
        u8 mac[32];
        hmac_sha256(s->key, 16, h, len, 0, 0, mac);
        memcpy(out, mac, 16);
    }
}

static void sign_unit(struct Smb *s, u8 *h, u32 len)
{
    p32(h + 16, g32(h + 16) | F_SIGNED);
    memset(h + 48, 0, 16);
    mac16(s, h, len, h + 48);
}

/* check a received message's signature in place; 0 if it is good */
static int check_sig(struct Smb *s, u8 *h, u32 len)
{
    u8 sig[16], mac[16];

    memcpy(sig, h + 48, 16);
    memset(h + 48, 0, 16);
    mac16(s, h, len, mac);
    memcpy(h + 48, sig, 16);
    return memcmp(sig, mac, 16) != 0;
}

/* wrap the units (mlen bytes at tx+4+TXH) in a TRANSFORM_HEADER, in place */
static void encrypt_frame(struct Smb *s, u32 mlen)
{
    u8 *t = s->tx + 4, *m = t + TXH;

    memset(t, 0, TXH);
    t[0] = 0xfd; t[1] = 'S'; t[2] = 'M'; t[3] = 'B';
    p32(t + 20, ++s->nonce);                    /* nonce: never repeats for a key */
    memcpy(t + 24, s->nsalt, s->cipher == 2 ? 8 : 7);
    p32(t + 36, mlen);
    p16(t + 42, 1);                             /* encrypted */
    memcpy(t + 44, s->sessid, 8);
    if (s->cipher == 2) aes_gcm_encrypt(&s->enckey, t + 20, t + 20, 32, m, mlen, t + 4);
    else aes_ccm_encrypt(&s->enckey.aes, t + 20, t + 20, 32, m, mlen, t + 4);
}

/* finish the units (lengths given), sign, send; 0 or a negative error */
static long send_units(struct Smb *s, const u32 *len, int n)
{
    u32 off = 0, total;
    u8 *f;
    int i;
    long e;

    for (i = 0; i < n; i++) {
        u8 *h = s->tx + 4 + TXH + off;
        u32 l = len[i];
        if (i + 1 < n) {
            u32 padded = (l + 7) & ~7UL;
            memset(h + l, 0, padded - l);
            p32(h + 20, padded);                /* NextCommand */
            l = padded;
        }
        if (s->sign && !s->enc)
            sign_unit(s, h, l);
        off += l;
    }
    if (s->enc) {
        encrypt_frame(s, off);
        f = s->tx;
        total = TXH + off;
    } else {
        f = s->tx + TXH;
        total = off;
    }
    f[0] = 0;
    f[1] = (u8)(total >> 16);
    f[2] = (u8)(total >> 8);
    f[3] = (u8)total;
    TRACE(">>", f + 4, total);
    if ((e = net_send(s->sock, f, total + 4)) < 0) {
        smb2_close(s);
        return e;
    }
    return 0;
}

/* receive one whole reply frame into rx (skipping interim PENDING replies);
 * returns its length (after the 4-byte NBSS header) or a negative error */
static long recv_frame(struct Smb *s)
{
    for (;;) {
        u8 nb[4];
        u32 len;
        long e;

        if ((e = net_recv(s->sock, nb, 4, s->timeout)) < 0) goto lost;
        len = (u32)nb[1] << 16 | (u32)nb[2] << 8 | nb[3];
        if (nb[0] != 0x00) {                    /* not a session message: e.g. the */
            u8 junk[64];                        /* 0x85 keepalive Samba sends every */
            while (len) {                       /* few minutes - skip it */
                u32 n = len < sizeof(junk) ? len : sizeof(junk);
                if ((e = net_recv(s->sock, junk, n, s->timeout)) < 0) goto lost;
                len -= n;
            }
            continue;
        }
        if (len < HDR || len > s->rxsize) { e = FS_EPROTO; goto lost; }
        if ((e = net_recv(s->sock, s->rx, len, s->timeout)) < 0) goto lost;
        s->rx_enc = 0;
        if (s->rx[0] == 0xfd && s->rx[1] == 'S') {
            u8 *t = s->rx;
            u32 olen = g32(t + 36);
            int bad;
            if (!s->cipher || len < TXH + HDR || olen > len - TXH || memcmp(t + 44, s->sessid, 8))
                { e = FS_EPROTO; goto lost; }
            if (s->cipher == 2) bad = aes_gcm_decrypt(&s->deckey, t + 20, t + 20, 32, t + TXH, olen, t + 4);
            else bad = aes_ccm_decrypt(&s->deckey.aes, t + 20, t + 20, 32, t + TXH, olen, t + 4);
            if (bad) { e = FS_EPROTO; goto lost; }
            memmove(s->rx, s->rx + TXH, olen);
            len = olen;
            s->rx_enc = 1;
        } else if (s->enc) {
            e = FS_EPROTO;                      /* plain text on an encrypted session */
            goto lost;
        }
        if (s->rx[0] != 0xfe || s->rx[1] != 'S') { e = FS_EPROTO; goto lost; }
        TRACE("<<", s->rx, len);
        if ((g32(s->rx + 16) & F_ASYNC) && g32(s->rx + 8) == ST_PENDING)
            continue;                           /* interim: the real reply follows */
        return (long)len;
lost:
        smb2_close(s);
        return e;
    }
}

/* check unit i of a reply frame; returns its body (and status), or NULL */
static const u8 *reply_unit(struct Smb *s, long flen, int idx, u32 *status, u32 *blen)
{
    u32 off = 0;
    int i;

    for (i = 0; ; i++) {
        const u8 *h = s->rx + off;
        u32 next = g32(h + 20), ulen = next ? next : (u32)flen - off;
        if (off + HDR > (u32)flen || ulen < HDR || off + ulen > (u32)flen)
            return NULL;
        if (i == idx) {
            *status = g32(h + 8);
            *blen = ulen - HDR;
            if (s->sign && !s->rx_enc && *status != ST_PENDING) {
                if (!(g32(h + 16) & F_SIGNED))
                    return *status == ST_OK ? NULL : h + HDR;   /* errors may come unsigned */
                if (check_sig(s, (u8 *)h, ulen))
                    return NULL;                /* forged or broken: drop */
            }
            return h + HDR;
        }
        if (!next)
            return NULL;
        off += next;
    }
}

/* one request unit, one reply: body pointer or NULL (*err set) */
static const u8 *transact(struct Smb *s, u32 len, u32 *status, u32 *blen, long *err)
{
    long fl;
    const u8 *b;

    *err = 0;
    if (s->sock < 0) { *err = NET_ECLOSED; return NULL; }
    if ((*err = send_units(s, &len, 1)) < 0) return NULL;
    if ((fl = recv_frame(s)) < 0) { *err = fl; return NULL; }
    if (!(b = reply_unit(s, fl, 0, status, blen))) { smb2_close(s); *err = FS_EPROTO; return NULL; }
    s->ntstatus = (long)*status;
    return b;
}

/* ---- NTLMv2 inside SPNEGO --------------------------------------------------- */

#define NTLM_UNICODE   0x00000001UL
#define NTLM_REQ_TGT   0x00000004UL
#define NTLM_SIGN      0x00000010UL
#define NTLM_NTLM      0x00000200UL
#define NTLM_ANON      0x00000800UL
#define NTLM_ALWAYS    0x00008000UL
#define NTLM_ESS       0x00080000UL
#define NTLM_128       0x20000000UL
#define NTLM_56        0x80000000UL
#define NTLM_TINFO_MAX  700
#define NTLM_TOKEN_MAX  1600
#define NTLM_OURS (NTLM_UNICODE | NTLM_REQ_TGT | NTLM_SIGN | NTLM_NTLM | NTLM_ALWAYS | \
                   NTLM_ESS | NTLM_128 | NTLM_56)

/* DER: tag + length + content (content already at buf+hdrroom); returns size */
static u32 der_wrap(u8 *out, u32 tag, const u8 *content, u32 clen)
{
    u32 n = 0;

    out[n++] = (u8)tag;
    if (clen < 128) out[n++] = (u8)clen;
    else if (clen < 256) { out[n++] = 0x81; out[n++] = (u8)clen; }
    else { out[n++] = 0x82; out[n++] = (u8)(clen >> 8); out[n++] = (u8)clen; }
    memmove(out + n, content, clen);
    return n + clen;
}

static const u8 OID_SPNEGO[] = { 0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02 };
static const u8 OID_NTLM[] = { 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a };

/* NegTokenInit [NTLMSSP] with the NTLM NEGOTIATE message */
static u32 spnego_init(u8 *out)
{
    u8 a[128], b[128], t[64];
    u32 n, m, k;

    memset(t, 0, 32);
    memcpy(t, "NTLMSSP", 8);
    p32(t + 8, 1);
    p32(t + 12, NTLM_OURS);
    p32(t + 20, 32); p32(t + 28, 32);          /* empty domain/workstation */
    n = der_wrap(a, 0x04, t, 32);              /* OCTET STRING mechToken */
    n = der_wrap(b, 0xa2, a, n);               /* [2] */
    m = der_wrap(a, 0x30, OID_NTLM, sizeof(OID_NTLM));
    m = der_wrap(t, 0xa0, a, m);               /* [0] mechTypes */
    memcpy(a, t, m);
    memcpy(a + m, b, n);
    k = der_wrap(b, 0x30, a, m + n);           /* NegTokenInit */
    k = der_wrap(a, 0xa0, b, k);
    memcpy(b, OID_SPNEGO, sizeof(OID_SPNEGO));
    memcpy(b + sizeof(OID_SPNEGO), a, k);
    return der_wrap(out, 0x60, b, sizeof(OID_SPNEGO) + k);
}

/* NegTokenResp with an NTLM AUTHENTICATE message */
static u32 spnego_resp(u8 *out, const u8 *tok, u32 len, u8 *tmp)
{
    u32 n = der_wrap(tmp, 0x04, tok, len);
    n = der_wrap(out, 0xa2, tmp, n);
    n = der_wrap(tmp, 0x30, out, n);
    return der_wrap(out, 0xa1, tmp, n);
}

static u32 to_utf16_upper(const char *s, u8 *out, int upper)
{
    u32 n = 0;
    while (*s) {
        u32 c = (u8)*s++;
        if (upper && c >= 'a' && c <= 'z') c -= 32;
        out[n++] = (u8)c; out[n++] = 0;
    }
    return n;
}

/* build the AUTHENTICATE message from the CHALLENGE (ch, chlen) into out */
static u32 ntlm_auth(struct Smb *s, const u8 *ch, u32 chlen, u8 *out)
{
    u8 nthash[16], ntowf[16], proof[16], cc[8], buf[160], *p;
    u32 flags = g32(ch + 20) & (NTLM_OURS | 0x00800000UL), n, tinfo_len = g16(ch + 40);
    u32 tinfo_off = g32(ch + 44), ntlen = 0;
    const u8 *tinfo = ch + tinfo_off, *ts = NULL;
    u8 *nt = out + 512;                         /* NT response built out of the way */
    int anon = !s->user[0] || s->want_guest;
    u32 off = 64, i;

    if (tinfo_off > chlen || tinfo_len > chlen - tinfo_off) tinfo_len = 0;
    if (tinfo_len > NTLM_TINFO_MAX) tinfo_len = NTLM_TINFO_MAX;   /* see session_setup */
    /* the server's own time (MsvAvTimestamp) if it gave one */
    for (i = 0; i + 4 <= tinfo_len; ) {
        u32 id = g16(tinfo + i), l = g16(tinfo + i + 2);
        if (id == 7 && l == 8) ts = tinfo + i + 4;
        if (id == 0) break;
        i += 4 + l;
    }
    memset(out, 0, 64);
    memcpy(out, "NTLMSSP", 8);
    p32(out + 8, 3);
    if (anon) {
        /* anonymous: empty user, 1-byte LM, no NT response */
        flags |= NTLM_ANON;
        out[off] = 0;
        p16(out + 12, 1); p16(out + 14, 1); p32(out + 16, off); off += 1;
        p16(out + 20, 0); p16(out + 22, 0); p32(out + 24, off);
        p32(out + 32, off); p32(out + 40, off); p32(out + 48, off); p32(out + 56, off);
        p32(out + 60, flags);
        memset(s->key, 0, 16);
        s->guest = 1;
        return off;
    }
    /* NTOWFv2 = HMAC-MD5(MD4(UTF-16 password), UTF-16(UPPER(user) + domain)) */
    n = to_utf16_upper(s->password, buf, 0);
    md4(buf, n, nthash);
    memset(buf, 0, sizeof(buf));
    n = to_utf16_upper(s->user, buf, 1);
    n += to_utf16_upper(s->domain, buf + n, 0);
    hmac_md5(nthash, 16, buf, n, 0, 0, ntowf);
    memset(nthash, 0, 16);
    /* the blob: 0101 0000 0000 0000, time, client challenge, 0, target info, 0 */
    net_random(cc, 8);
    p = nt + 16;
    memset(p, 0, 28);
    p[0] = 1; p[1] = 1;
    if (ts) memcpy(p + 8, ts, 8);
    else {
        unsigned long long ft = ((unsigned long long)net_time() + 11644473600ULL) * 10000000ULL;
        for (i = 0; i < 8; i++) p[8 + i] = (u8)(ft >> (8 * i));
    }
    memcpy(p + 16, cc, 8);
    memcpy(p + 28, tinfo, tinfo_len);
    memset(p + 28 + tinfo_len, 0, 4);
    ntlen = 16 + 28 + tinfo_len + 4;
    hmac_md5(ntowf, 16, ch + 24, 8, p, ntlen - 16, proof);
    memcpy(nt, proof, 16);
    hmac_md5(ntowf, 16, proof, 16, 0, 0, s->key);    /* session base key */
    memset(ntowf, 0, 16);
    /* payload: domain, user, workstation, LM (24 zero bytes), NT */
    n = to_utf16_upper(s->domain, out + off, 0);
    p16(out + 28, n); p16(out + 30, n); p32(out + 32, off); off += n;
    n = to_utf16_upper(s->user, out + off, 0);
    p16(out + 36, n); p16(out + 38, n); p32(out + 40, off); off += n;
    n = to_utf16_upper("AMIGA", out + off, 0);
    p16(out + 44, n); p16(out + 46, n); p32(out + 48, off); off += n;
    memset(out + off, 0, 24);
    p16(out + 12, 24); p16(out + 14, 24); p32(out + 16, off); off += 24;
    memmove(out + off, nt, ntlen);
    p16(out + 20, ntlen); p16(out + 22, ntlen); p32(out + 24, off); off += ntlen;
    p32(out + 56, off);                         /* no encrypted session key */
    p32(out + 60, flags);
    s->guest = 0;
    return off;
}

/* ---- connect ---------------------------------------------------------------- */

/* 3.1.1 pre-auth integrity: H = SHA-512(H || message) */
static void preauth_add(struct Smb *s, const u8 *msg, u32 len)
{
    struct Sha512 c;

    if (s->dialect != 0x311) return;
    sha512_init(&c);
    sha512_update(&c, s->preauth, 64);
    sha512_update(&c, msg, len);
    sha512_final(&c, s->preauth);
}

static const unsigned short dialects[] = { 0x0202, 0x0210, 0x0300, 0x0302, 0x0311 };

static long negotiate(struct Smb *s, int *server_requires_signing)
{
    u8 *b = unit(s, 0, C_NEGOTIATE, 0);
    u32 st, bl, i, nd = 0, len, max = s->max_dialect ? s->max_dialect : 0x311;
    long err;
    const u8 *r;

    p16(b, 36);
    p16(b + 4, 1);                              /* signing enabled */
    if (s->offer_enc) p32(b + 8, 0x40);         /* CAP_ENCRYPTION (3.0.x) */
    net_random(b + 12, 16);                     /* client GUID */
    for (i = 0; i < sizeof(dialects) / sizeof(dialects[0]) && dialects[i] <= max; i++)
        p16(b + 36 + 2 * nd++, dialects[i]);
    p16(b + 2, nd);
    len = 36 + 2 * nd;
    if (max >= 0x311) {
        /* negotiate contexts, 8-byte aligned from the header */
        u8 *c;
        while ((HDR + len) & 7) b[len++] = 0;
        p32(b + 28, HDR + len);
        c = b + len;
        memset(c, 0, 8 + 38);
        p16(c, 1);                              /* PREAUTH_INTEGRITY_CAPABILITIES */
        p16(c + 2, 38);
        p16(c + 8, 1);                          /* one hash, */
        p16(c + 10, 32);                        /* a 32-byte salt */
        p16(c + 12, 1);                         /* SHA-512 */
        net_random(c + 14, 32);
        len += 46;
        if (s->offer_enc) {
            while ((HDR + len) & 7) b[len++] = 0;
            c = b + len;
            memset(c, 0, 8);
            p16(c, 2);                          /* ENCRYPTION_CAPABILITIES */
            p16(c + 2, 6);
            p16(c + 8, 2);
            p16(c + 10, 2);                     /* AES-128-GCM first, */
            p16(c + 12, 1);                     /* then AES-128-CCM */
            len += 14;
            p16(b + 32, 2);
        } else
            p16(b + 32, 1);
    }
    memset(s->preauth, 0, 64);
    s->cipher = 0;
    if (!(r = transact(s, HDR + len, &st, &bl, &err))) return err;
    if (st != ST_OK || bl < 64) return st ? map_status(st) : FS_EPROTO;
    s->dialect = g16(r + 4);
    *server_requires_signing = (g16(r + 2) & 2) != 0;
    s->maxread = g32(r + 32);
    s->maxwrite = g32(r + 36);
    for (i = 0; i < nd && dialects[i] != s->dialect; i++)
        ;
    if (i == nd) return FS_EUNSUPP;             /* not one we offered */
    if (s->dialect == 0x311) {
        u32 off = g32(r + 60), cnt = g16(r + 6), k;
        int have_preauth = 0;
        preauth_add(s, s->tx + 4 + TXH, HDR + len);
        preauth_add(s, s->rx, HDR + bl);
        for (k = 0; k < cnt; k++) {
            const u8 *c = s->rx + off;
            u32 t, l;
            if (off < HDR || off > HDR + bl - 8) return FS_EPROTO;
            t = g16(c); l = g16(c + 2);
            if (l > HDR + bl - off - 8) return FS_EPROTO;
            if (t == 1 && l >= 6 && g16(c + 8) == 1 && g16(c + 12) == 1) have_preauth = 1;
            if (t == 2 && l >= 4 && g16(c + 8) == 1 && s->offer_enc) {
                u32 ci = g16(c + 10);
                if (ci == 1 || ci == 2) s->cipher = ci;
            }
            off = (off + 8 + l + 7) & ~7UL;
        }
        if (!have_preauth) return FS_EPROTO;
    } else if (s->dialect >= 0x300 && s->offer_enc && (g32(r + 24) & 0x40))
        s->cipher = 1;                          /* 3.0.x: CCM only */
    return 0;
}

/* SMB 3.x keys from the session key (and for 3.1.1 the pre-auth hash) */
static void derive_keys(struct Smb *s)
{
    u8 k[16];

    if (s->dialect == 0x311) {
        smb3_kdf(s->key, "SMBSigningKey", 14, s->preauth, 64, k);
        aes_setkey(&s->signkey, k);
        smb3_kdf(s->key, "SMBC2SCipherKey", 16, s->preauth, 64, k);
        gcm_setkey(&s->enckey, k);
        smb3_kdf(s->key, "SMBS2CCipherKey", 16, s->preauth, 64, k);
        gcm_setkey(&s->deckey, k);
    } else {
        smb3_kdf(s->key, "SMB2AESCMAC", 12, "SmbSign", 8, k);
        aes_setkey(&s->signkey, k);
        smb3_kdf(s->key, "SMB2AESCCM", 11, "ServerIn ", 10, k);
        gcm_setkey(&s->enckey, k);
        smb3_kdf(s->key, "SMB2AESCCM", 11, "ServerOut", 10, k);
        gcm_setkey(&s->deckey, k);
    }
    memset(k, 0, 16);
    net_random(s->nsalt, 8);
    s->nonce = 0;
}

static long session_setup(struct Smb *s, int server_requires_signing)
{
    /* handler stack is small, so static. Sized for an AUTHENTICATE with
     * NTLM_TINFO_MAX bytes of TargetInfo (an AD forest with long DNS names
     * sends a few hundred), 64-char user, 32-char domain: about 1300 bytes.
     * The read-only send buffer (2 KB) still takes that plus the headers. */
    static u8 tok[NTLM_TOKEN_MAX], tmp[NTLM_TOKEN_MAX], scratch[NTLM_TOKEN_MAX];
    u32 tlen = spnego_init(tok), st, bl, round;
    int want_sign = 0;
    long err;
    const u8 *r;

    s->sign = 0;
    for (round = 0; round < 2; round++) {
        u8 *b = unit(s, 0, C_SESSION_SETUP, 0);
        p16(b, 25);
        b[3] = 1;                               /* SecurityMode: signing enabled */
        p16(b + 12, HDR + 24);
        p16(b + 14, tlen);
        memcpy(b + 24, tok, tlen);
        if (!(r = transact(s, HDR + 24 + tlen, &st, &bl, &err)))
            return err;
        preauth_add(s, s->tx + 4 + TXH, HDR + 24 + tlen);
        if (round == 0) {
            preauth_add(s, s->rx, HDR + bl);
            const u8 *sec, *ch = NULL;
            u32 off = g16(r + 4), len = g16(r + 6), i;
            if (st != ST_MORE_PROCESSING)
                return st ? map_status(st) : FS_EPROTO;
            memcpy(s->sessid, s->rx + 40, 8);   /* the session id from the reply header */
            if (off < HDR || off - HDR + len > bl) return FS_EPROTO;
            sec = s->rx + off;
            for (i = 0; i + 12 <= len; i++)     /* find the NTLM CHALLENGE in the token */
                if (!memcmp(sec + i, "NTLMSSP", 8) && g32(sec + i + 8) == 2) { ch = sec + i; break; }
            if (!ch) return FS_EPROTO;
            len -= (u32)(ch - sec);
            tlen = ntlm_auth(s, ch, len, tmp);
            tlen = spnego_resp(tok, tmp, tlen, scratch);
            /* SESSION_SETUP itself goes unsigned; signing starts after it */
            want_sign = !s->guest && (server_requires_signing || s->sign_required_by_us);
        } else {
            u32 flags;
            if (st != ST_OK)
                return map_status(st);
            flags = g16(r + 2);
            if (flags & 3)                      /* IS_GUEST / IS_NULL */
                s->guest = 1;
            if (s->dialect >= 0x300 && !s->guest) {
                /* the final reply (not in the hash) is signed with the new key */
                derive_keys(s);
                if (g32(s->rx + 16) & F_SIGNED) {
                    if (check_sig(s, s->rx, HDR + bl)) return FS_EDENIED;
                } else if (s->dialect == 0x311)
                    return FS_EPROTO;
                if ((flags & 4) || s->encrypt_mode > 0) {   /* ENCRYPT_DATA, or ours */
                    if (!s->cipher) return FS_EUNSUPP;
                    s->enc = 1;
                }
            }
            s->sign = want_sign && !s->guest;
        }
    }
    return 0;
}

static long tree_connect(struct Smb *s, const char *host, const char *share)
{
    u8 *b = unit(s, 0, C_TREE_CONNECT, 0), path[600];
    char unc[300];
    u32 n, st, bl;
    long err;
    const u8 *r;

    strcpy(unc, "\\\\");
    strncat(unc, host, 100);
    strcat(unc, "\\");
    strncat(unc, share, 150);
    n = utf8_to_utf16(unc, path, sizeof(path));
    int sign = s->sign;
    p16(b, 9);
    p16(b + 4, HDR + 8);
    p16(b + 6, n);
    memcpy(b + 8, path, n);
    if (s->dialect == 0x311 && !s->guest)
        s->sign = 1;                            /* 3.1.1 always signs TREE_CONNECT */
    r = transact(s, HDR + 8 + n, &st, &bl, &err);
    s->sign = sign;
    if (!r) return err;
    if (st != ST_OK) return map_status(st);
    s->treeid = g32(s->rx + 36);
    if (bl >= 8 && (g32(r + 4) & 0x08) && !s->enc) {   /* SHAREFLAG_ENCRYPT_DATA */
        if (!s->cipher) return FS_EUNSUPP;
        s->enc = 1;
    }
    return 0;
}

/* ENCRYPT=AUTO offers encryption only after a share refused us without it
 * (ACCESS_DENIED or a demand we could not meet): AES on a 68000 is slow,
 * and a Samba server set to "desired" would otherwise encrypt everything */
long smb2_connect(struct Smb *s, u32 ip, unsigned port, const char *host, const char *share)
{
    long e = 0;
    int req = 0, attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        int stage = 0;
        smb2_close(s);
        s->msgid_lo = s->msgid_hi = 0;
        s->treeid = 0;
        s->dialect = 0;
        s->sign = s->enc = s->rx_enc = 0;
        s->offer_enc = s->encrypt_mode > 0 || (s->encrypt_mode == 0 && attempt);
        memset(s->sessid, 0, 8);
        if ((e = net_connect(ip, port ? port : 445, 0, s->timeout)) < 0)
            return e;
        s->sock = e;
        if (!(e = negotiate(s, &req)) && !(e = session_setup(s, req)) &&
            (stage = 1, !(e = tree_connect(s, host, share)))) {
            if (s->encrypt_mode > 0 && !s->enc) {
                /* ENCRYPT=YES on a 2.x dialect or a guest session: nothing
                 * would be encrypted, so refuse rather than go on in clear */
                smb2_close(s);
                return FS_EUNSUPP;
            }
            return 0;
        }
        smb2_close(s);
        if (!(stage && s->encrypt_mode == 0 && !s->offer_enc && s->dialect >= 0x300 &&
              !s->guest && (e == NFS3ERR_ACCES || e == FS_EUNSUPP)))
            break;
    }
    return e;
}

/* ---- files ------------------------------------------------------------------ */

static void ft_to_unix(const u8 *p, u32 *out)
{
    unsigned long long ft = 0;
    int i;
    for (i = 7; i >= 0; i--) ft = (ft << 8) | p[i];
    ft /= 10000000ULL;
    *out = ft > 11644473600ULL ? (u32)(ft - 11644473600ULL) : 0;
}

/* attributes from times/sizes/attrs laid out as in a CREATE reply body
 * (creation @0, access @8, write @16, change @24, eof @40, attrs @48) */
static void to_fattr(const u8 *t, struct Fattr *a)
{
    u32 attrs = g32(t + 48);

    memset(a, 0, sizeof(*a));
    a->type = (attrs & 0x10) ? NF3DIR : NF3REG;
    a->mode = (attrs & 0x01) && !(attrs & 0x10) ? 0555 : 0777;
    a->nlink = 1;
    a->size_lo = g32(t + 40);
    a->size_hi = g32(t + 44);
    ft_to_unix(t + 8, &a->atime);
    ft_to_unix(t + 16, &a->mtime);
    ft_to_unix(t + 24, &a->ctime);
}

/* a CREATE body at b (in a unit); returns its length */
static u32 create_body2(u8 *b, const char *path, u32 access, u32 options,
                        u32 disposition, u32 attrs)
{
    u32 n;

    memset(b, 0, 56);
    p16(b, 57);
    p32(b + 4, 2);                              /* impersonation */
    p32(b + 24, access);
    p32(b + 28, attrs);
    p32(b + 32, 7);                             /* share read/write/delete */
    p32(b + 36, disposition);
    p32(b + 40, options);
    n = utf8_to_utf16(path, b + 56, 600);
    p16(b + 44, HDR + 56);
    p16(b + 46, n);
    if (!n) b[56] = 0;
    return 56 + (n ? n : 1);
}

static u32 create_body(u8 *b, const char *path, u32 access, u32 options)
{
    return create_body2(b, path, access, options, 1 /* FILE_OPEN */, 0);
}

long smb2_getattr(struct Smb *s, const char *path, struct Fattr *a)
{
    u32 len[2], st, bl;
    u8 *b;
    const u8 *r;
    long fl, e;

    if (s->sock < 0) return NET_ECLOSED;
    b = unit(s, 0, C_CREATE, 0);
    len[0] = HDR + create_body(b, path, 0x80, 0);       /* FILE_READ_ATTRIBUTES */
    {
        u32 off = (len[0] + 7) & ~7UL;
        b = unit(s, off, C_CLOSE, 1);
        p16(b, 24);
        memset(b + 8, 0xff, 16);                         /* the FileId just opened */
        len[1] = HDR + 24;
    }
    if ((e = send_units(s, len, 2)) < 0) return e;
    if ((fl = recv_frame(s)) < 0) return fl;
    if (!(r = reply_unit(s, fl, 0, &st, &bl))) { smb2_close(s); return FS_EPROTO; }
    s->ntstatus = (long)st;
    if (st != ST_OK) return map_status(st);
    if (bl < 88) return FS_EPROTO;
    to_fattr(r + 8, a);
    return 0;
}

long smb2_open(struct Smb *s, const char *path, int dir, struct Smb2Id *id, struct Fattr *a)
{
    u8 *b = unit(s, 0, C_CREATE, 0);
    u32 st, bl, n = create_body(b, path, dir ? 0x00100081UL : 0x00120089UL, dir ? 1 : 0);
    long err;
    const u8 *r;

    if (!(r = transact(s, HDR + n, &st, &bl, &err))) return err;
    if (st != ST_OK) return map_status(st);
    if (bl < 88) return FS_EPROTO;
    memcpy(id->b, r + 64, 16);
    if (a) to_fattr(r + 8, a);
    return 0;
}

long smb2_closefile(struct Smb *s, const struct Smb2Id *id)
{
    u8 *b = unit(s, 0, C_CLOSE, 0);
    u32 st, bl;
    long err;

    p16(b, 24);
    memcpy(b + 8, id->b, 16);
    if (!transact(s, HDR + 24, &st, &bl, &err)) return err;
    return st == ST_OK ? 0 : map_status(st);
}

long smb2_read(struct Smb *s, const struct Smb2Id *id, u32 off_hi, u32 off_lo, u32 count,
               const u8 **data, u32 *got, int *eof)
{
    u8 *b = unit(s, 0, C_READ, 0);
    u32 st, bl, doff;
    long err;
    const u8 *r;

    memset(b, 0, 49);
    p16(b, 49);
    b[2] = 0x50;
    p32(b + 4, count);
    p32(b + 8, off_lo);
    p32(b + 12, off_hi);
    memcpy(b + 16, id->b, 16);
    if (!(r = transact(s, HDR + 49, &st, &bl, &err))) return err;
    *eof = 0;
    if (st == ST_END_OF_FILE) { *got = 0; *eof = 1; *data = r; return 0; }
    if (st != ST_OK && st != 0x80000005UL) return map_status(st);   /* BUFFER_OVERFLOW */
    doff = r[2];
    *got = g32(r + 4);
    if (doff < HDR || doff - HDR > bl || *got > bl - (doff - HDR) || *got > count) return FS_EPROTO;
    *data = s->rx + doff;
    *eof = *got < count;
    return 0;
}

long smb2_querydir(struct Smb *s, const struct Smb2Id *id, int restart, u32 bufsize,
                   const u8 **buf, u32 *len, int *eof)
{
    u8 *b = unit(s, 0, C_QUERY_DIR, 0);
    u32 st, bl, off;
    long err;
    const u8 *r;

    memset(b, 0, 32);
    p16(b, 33);
    b[2] = 0x25;                                /* FileIdBothDirectoryInformation */
    b[3] = restart ? 0x01 : 0;
    memcpy(b + 8, id->b, 16);
    p16(b + 24, HDR + 32);
    p16(b + 26, 2);
    b[32] = '*'; b[33] = 0;
    p32(b + 28, bufsize);
    if (!(r = transact(s, HDR + 34, &st, &bl, &err))) return err;
    *eof = 0;
    if (st == ST_NO_MORE_FILES) { *len = 0; *eof = 1; return 0; }
    if (st != ST_OK) return map_status(st);
    off = g16(r + 2);
    *len = g32(r + 4);
    if (off < HDR || off - HDR > bl || *len > bl - (off - HDR)) return FS_EPROTO;
    *buf = s->rx + off;
    return 0;
}

int smb2_dir_next(const u8 *buf, u32 len, u32 *pos, char *name, u32 namemax, struct Fattr *a)
{
    const u8 *e;
    u32 next, nlen;

    if (*pos + 104 > len)
        return 0;
    e = buf + *pos;
    next = g32(e);
    nlen = g32(e + 60);
    if (nlen > len - (*pos + 104))
        return 0;
    if (next && (next < 104 || next > len - *pos))
        return 0;                               /* malformed chain: stop here */
    utf16_to_utf8(e + 104, nlen, name, namemax);
    to_fattr(e + 8, a);                         /* times and attributes as in CREATE */
    a->size_lo = g32(e + 40);                   /* but EndOfFile sits at +40 here */
    a->size_hi = g32(e + 44);
    *pos = next ? *pos + next : len;
    return 1;
}

long smb2_fsstat(struct Smb *s, struct Fsstat *f)
{
    u32 len[3], st, bl, off;
    u8 *b;
    const u8 *r;
    long fl, e;
    unsigned long long unit_bytes, total, avail;

    if (s->sock < 0) return NET_ECLOSED;
    b = unit(s, 0, C_CREATE, 0);
    len[0] = HDR + create_body(b, "", 0x80, 0);
    off = (len[0] + 7) & ~7UL;
    b = unit(s, off, C_QUERY_INFO, 1);
    memset(b, 0, 41);
    p16(b, 41);
    b[2] = 2;                                   /* SMB2_0_INFO_FILESYSTEM */
    b[3] = 7;                                   /* FileFsFullSizeInformation */
    p32(b + 4, 32);
    memset(b + 24, 0xff, 16);
    len[1] = HDR + 41;
    off += (len[1] + 7) & ~7UL;
    b = unit(s, off, C_CLOSE, 1);
    p16(b, 24);
    memset(b + 8, 0xff, 16);
    len[2] = HDR + 24;
    if ((e = send_units(s, len, 3)) < 0) return e;
    if ((fl = recv_frame(s)) < 0) return fl;
    if (!(r = reply_unit(s, fl, 1, &st, &bl))) { smb2_close(s); return FS_EPROTO; }
    if (st != ST_OK) return map_status(st);
    {
        u32 o = g16(r + 2), l = g32(r + 4);
        const u8 *d = (r - HDR) + o;            /* offsets count from the unit's header */
        if (o < HDR || o - HDR > bl || l > bl - (o - HDR) || l < 32) return FS_EPROTO;
        unit_bytes = (unsigned long long)g32(d + 24) * g32(d + 28);
        total = ((unsigned long long)g32(d + 4) << 32 | g32(d)) * unit_bytes;
        avail = ((unsigned long long)g32(d + 12) << 32 | g32(d + 8)) * unit_bytes;
    }
    f->tbytes_hi = (u32)(total >> 32); f->tbytes_lo = (u32)total;
    f->fbytes_hi = f->abytes_hi = (u32)(avail >> 32);
    f->fbytes_lo = f->abytes_lo = (u32)avail;
    return 0;
}

/* ---- writing (stage 2) ---------------------------------------------------- */

#define A_READ_DATA     0x00000001UL
#define A_WRITE_DATA    0x00000002UL
#define A_APPEND_DATA   0x00000004UL
#define A_READ_ATTR     0x00000080UL
#define A_WRITE_ATTR    0x00000100UL
#define A_DELETE        0x00010000UL
#define A_SYNCHRONIZE   0x00100000UL
#define O_DIRECTORY     0x00000001UL
#define O_NON_DIRECTORY 0x00000040UL
#define O_DELETE_ON_CLOSE 0x00001000UL

long smb2_openw(struct Smb *s, const char *path, u32 disposition, struct Smb2Id *id,
                struct Fattr *a)
{
    u8 *b = unit(s, 0, C_CREATE, 0);
    u32 st, bl, n = create_body2(b, path,
                                 A_READ_DATA | A_WRITE_DATA | A_APPEND_DATA | A_READ_ATTR |
                                 A_WRITE_ATTR | A_SYNCHRONIZE,
                                 O_NON_DIRECTORY, disposition, 0x80 /* NORMAL */);
    long err;
    const u8 *r;

    if (!(r = transact(s, HDR + n, &st, &bl, &err))) return err;
    if (st != ST_OK) return map_status(st);
    if (bl < 88) return FS_EPROTO;
    memcpy(id->b, r + 64, 16);
    if (a) to_fattr(r + 8, a);
    return 0;
}

long smb2_write(struct Smb *s, const struct Smb2Id *id, u32 off_hi, u32 off_lo,
                const u8 *data, u32 count, u32 *written)
{
    u8 *b = unit(s, 0, C_WRITE, 0);
    u32 st, bl;
    long err;
    const u8 *r;

    memset(b, 0, 48);
    p16(b, 49);
    p16(b + 2, HDR + 48);                       /* DataOffset */
    p32(b + 4, count);
    p32(b + 8, off_lo);
    p32(b + 12, off_hi);
    memcpy(b + 16, id->b, 16);
    memcpy(b + 48, data, count);
    if (!(r = transact(s, HDR + 48 + count, &st, &bl, &err))) return err;
    if (st != ST_OK) return map_status(st);
    *written = g32(r + 4);
    return 0;
}

long smb2_flush(struct Smb *s, const struct Smb2Id *id)
{
    u8 *b = unit(s, 0, C_FLUSH, 0);
    u32 st, bl;
    long err;

    memset(b, 0, 24);
    p16(b, 24);
    memcpy(b + 8, id->b, 16);
    if (!transact(s, HDR + 24, &st, &bl, &err)) return err;
    return st == ST_OK ? 0 : map_status(st);
}

/* CREATE path (with access/options/disposition) + optional SET_INFO + CLOSE,
 * compounded; returns the first failing status. a (if given) gets the
 * attributes from the CREATE reply. */
static long open_set_close(struct Smb *s, const char *path, u32 access, u32 options,
                           u32 disposition, u32 info_class, const u8 *info, u32 ilen,
                           struct Fattr *a)
{
    u32 len[3], st, bl, off, n = 0, i;
    u8 *b;
    const u8 *r;
    long fl, e;

    if (s->sock < 0) return NET_ECLOSED;
    b = unit(s, 0, C_CREATE, 0);
    len[n++] = HDR + create_body2(b, path, access, options, disposition, 0x80);
    off = (len[0] + 7) & ~7UL;
    if (info) {
        b = unit(s, off, C_SET_INFO, 1);
        memset(b, 0, 32);
        p16(b, 33);
        b[2] = 1;                               /* SMB2_0_INFO_FILE */
        b[3] = (u8)info_class;
        p32(b + 4, ilen);
        p16(b + 8, HDR + 32);
        memset(b + 16, 0xff, 16);               /* the FileId just opened */
        memcpy(b + 32, info, ilen);
        len[n++] = HDR + 32 + ilen;
        off += (len[1] + 7) & ~7UL;
    }
    b = unit(s, off, C_CLOSE, 1);
    p16(b, 24);
    memset(b + 8, 0xff, 16);
    len[n++] = HDR + 24;
    if ((e = send_units(s, len, (int)n)) < 0) return e;
    if ((fl = recv_frame(s)) < 0) return fl;
    for (i = 0; i < n; i++) {
        if (!(r = reply_unit(s, fl, (int)i, &st, &bl))) { smb2_close(s); return FS_EPROTO; }
        s->ntstatus = (long)st;
        if (st != ST_OK) return map_status(st);
        if (i == 0 && a && bl >= 88) to_fattr(r + 8, a);
    }
    return 0;
}

long smb2_mkdir(struct Smb *s, const char *path, struct Fattr *a)
{
    return open_set_close(s, path, A_READ_ATTR | A_SYNCHRONIZE, O_DIRECTORY,
                          SMB2_CREATE, 0, NULL, 0, a);
}

long smb2_delete(struct Smb *s, const char *path, int dir)
{
    /* FileDispositionInformation rather than DELETE_ON_CLOSE: the server
     * refuses a non-empty directory here (DIRECTORY_NOT_EMPTY), where a
     * delete-on-close CLOSE may just succeed and leave it in place */
    static const u8 pending[1] = { 1 };
    return open_set_close(s, path, A_DELETE | A_READ_ATTR | A_SYNCHRONIZE,
                          dir ? O_DIRECTORY : O_NON_DIRECTORY,
                          SMB2_OPEN, 13, pending, 1, NULL);
}

long smb2_rename(struct Smb *s, const char *from, const char *to)
{
    u8 info[24 + 600];
    u32 n;

    memset(info, 0, 20);                        /* ReplaceIfExists 0, RootDirectory 0 */
    n = utf8_to_utf16(to, info + 20, 600);
    p32(info + 16, n);                          /* FileNameLength */
    return open_set_close(s, from, A_DELETE | A_READ_ATTR | A_SYNCHRONIZE, 0, SMB2_OPEN,
                          10 /* FileRenameInformation */, info, 20 + n, NULL);
}

long smb2_setattr(struct Smb *s, const char *path, int set_size, u32 size,
                  int set_mtime, u32 mtime, int set_mode, u32 mode)
{
    long e;

    if (set_size) {
        u8 eof[8];
        memset(eof, 0, 8);
        p32(eof, size);
        if ((e = open_set_close(s, path, A_WRITE_DATA | A_READ_ATTR | A_SYNCHRONIZE, 0,
                                SMB2_OPEN, 20 /* FileEndOfFileInformation */, eof, 8, NULL)))
            return e;
    }
    if (set_mtime || set_mode) {
        u8 bi[40];
        memset(bi, 0, 40);                      /* 0 = leave unchanged */
        if (set_mtime) {
            unsigned long long ft = ((unsigned long long)mtime + 11644473600ULL) * 10000000ULL;
            int i;
            for (i = 0; i < 8; i++) bi[16 + i] = (u8)(ft >> (8 * i));   /* LastWriteTime */
        }
        if (set_mode)
            p32(bi + 32, (mode & 0200) ? 0x80 /* NORMAL */ : 0x01 /* READONLY */);
        if ((e = open_set_close(s, path, A_WRITE_ATTR | A_READ_ATTR | A_SYNCHRONIZE, 0,
                                SMB2_OPEN, 4 /* FileBasicInformation */, bi, 40, NULL)))
            return e;
    }
    return 0;
}

/* ---- share list (stage 3): SRVSVC NetrShareEnum over \\host\IPC$ ----------
 * [MS-SRVS] 3.1.4.8 on DCE/RPC ([C706], [MS-RPCE]) over the srvsvc named
 * pipe: CREATE, WRITE a bind, READ the ack, WRITE the request, READ the
 * reply fragments, CLOSE. Only for listing - never needed to mount. */

static const u8 rpc_bind[72] = {
    5, 0, 11, 3, 0x10, 0, 0, 0, 72, 0, 0, 0, 1, 0, 0, 0,   /* bind, call 1 */
    0xb8, 0x10, 0xb8, 0x10, 0, 0, 0, 0, 1, 0, 0, 0,         /* 4280/4280, 1 context */
    0, 0, 1, 0,                                             /* id 0, 1 transfer syntax */
    0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01,         /* srvsvc 4b324fc8-1670- */
    0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88, 3, 0, 0, 0,   /* 01d3-1278-5a47bf6ee188 v3 */
    0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11,         /* NDR 8a885d04-1ceb-11c9- */
    0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 2, 0, 0, 0    /* 9fe8-08002b104860 v2 */
};

/* write one PDU, then read whole PDUs until the last fragment; the stub
 * data of response fragments is gathered at buf. Returns its length or a
 * negative error. */
static long rpc_call(struct Smb *s, const struct Smb2Id *pipe, const u8 *pdu, u32 plen,
                     u8 *buf, u32 bufsize, int bind)
{
    u32 w, have = 0, stub = 0, got;
    const u8 *d;
    u8 *raw = buf + bufsize / 2;                /* raw PDUs in the upper half */
    u32 rawmax = bufsize - bufsize / 2;
    int eof;
    long e;

    if ((e = smb2_write(s, pipe, 0, 0, pdu, plen, &w))) return e < 0 ? e : FS_EPROTO;
    for (;;) {
        u32 flen;
        while (have < 16 || have < (flen = g16(raw + 8))) {
            if ((e = smb2_read(s, pipe, 0, 0, 4280, &d, &got, &eof))) return e < 0 ? e : FS_EPROTO;
            if (!got || got > rawmax - have) return FS_EPROTO;
            memcpy(raw + have, d, got);
            have += got;
        }
        if (raw[0] != 5 || flen < 24) return FS_EPROTO;
        if (bind) {
            /* bind_ack: the one result must be "acceptance" */
            u32 sec = g16(raw + 24), o = (26 + sec + 3) & ~3UL;
            if (raw[2] != 12 || o + 8 > flen || raw[o] < 1 || g16(raw + o + 4)) return FS_EUNSUPP;
            return 0;
        }
        if (raw[2] != 2) return FS_EPROTO;      /* a fault or something else */
        if (stub + flen - 24 > bufsize / 2) return FS_EPROTO;
        memcpy(buf + stub, raw + 24, flen - 24);
        stub += flen - 24;
        if (raw[3] & 2) return (long)stub;      /* last fragment */
        memmove(raw, raw + flen, have - flen);
        have -= flen;
    }
}

/* NDR conformant varying string at *pos -> UTF-8 */
static int ndr_str(const u8 *b, u32 len, u32 *pos, char *out, u32 max)
{
    u32 n;
    if (*pos + 12 > len) return 0;
    n = g32(b + *pos + 8);
    if (n > (len - *pos - 12) / 2) return 0;
    n *= 2;
    utf16_to_utf8(b + *pos + 12, n >= 2 ? n - 2 : 0, out, max);   /* without the NUL */
    *pos = (*pos + 12 + n + 3) & ~3UL;
    return 1;
}

long smb2_shares(struct Smb *s, u32 ip, unsigned port, const char *host, u8 *buf, u32 bufsize,
                 void (*fn)(void *ctx, const char *name, u32 type, const char *remark),
                 void *ctx)
{
    struct Smb2Id pipe;
    u8 *b, *q, req[24 + 4 + 12 + 600 + 36];
    u32 st, bl, n, i, pos, cnt, arr;
    long e, len;
    const u8 *r;
    char unc[120], name[260], remark[260];

    if ((e = smb2_connect(s, ip, port, host, "IPC$"))) return e;
    b = unit(s, 0, C_CREATE, 0);
    n = create_body2(b, "srvsvc", 0x0012019fUL, 0, SMB2_OPEN, 0);
    if (!(r = transact(s, HDR + n, &st, &bl, &e))) return e;
    if (st != ST_OK) { e = map_status(st); goto out; }
    if (bl < 88) { e = FS_EPROTO; goto out; }
    memcpy(pipe.b, r + 64, 16);
    if ((e = rpc_call(s, &pipe, rpc_bind, sizeof(rpc_bind), buf, bufsize, 1))) goto done;

    /* NetrShareEnum(\\host, level 1, empty container, max, resume 0) */
    memset(req, 0, sizeof(req));
    q = req + 24;
    p32(q, 0x00020000UL); q += 4;               /* ServerName: unique pointer */
    strcpy(unc, "\\\\");
    strncat(unc, host, 100);
    n = utf8_to_utf16(unc, q + 12, 600);
    q[12 + n] = q[13 + n] = 0;                  /* with its NUL */
    p32(q, n / 2 + 1); p32(q + 8, n / 2 + 1);   /* max count, offset 0, actual count */
    q += (12 + n + 2 + 3) & ~3UL;
    p32(q, 1); p32(q + 4, 1);                   /* level 1, union arm 1 */
    p32(q + 8, 0x00020004UL);                   /* container pointer */
    q += 20;                                    /* EntriesRead 0, Buffer NULL */
    p32(q, 0xffffffffUL);                       /* PreferedMaximumLength */
    p32(q + 4, 0x00020008UL);                   /* ResumeHandle pointer, value 0 */
    q += 12;
    n = (u32)(q - req);
    req[0] = 5; req[2] = 0; req[3] = 3;         /* request, first + last */
    req[4] = 0x10;
    p16(req + 8, n);
    p32(req + 12, 2);                           /* call 2 */
    p32(req + 16, n - 24);                      /* alloc hint */
    p16(req + 22, 15);                          /* opnum 15: NetrShareEnum */
    if ((len = rpc_call(s, &pipe, req, n, buf, bufsize, 0)) < 0) { e = len; goto done; }

    /* SHARE_ENUM_STRUCT: level, arm, pointer -> { count, pointer -> [max]
     * { name ptr, type, remark ptr } } and the strings after the array */
    pos = 12;
    if ((u32)len < 24 || !g32(buf + 8)) { e = FS_EPROTO; goto done; }
    cnt = g32(buf + pos);
    if (!g32(buf + pos + 4)) cnt = 0;
    pos += 8;
    if (cnt) pos += 4;                          /* conformant max count */
    arr = pos;
    if (cnt > ((u32)len - arr) / 12) { e = FS_EPROTO; goto done; }
    pos += 12 * cnt;
    for (i = 0; i < cnt; i++) {
        const u8 *ent = buf + arr + 12 * i;
        name[0] = remark[0] = 0;
        if (g32(ent) && !ndr_str(buf, (u32)len, &pos, name, sizeof(name))) break;
        if (g32(ent + 8) && !ndr_str(buf, (u32)len, &pos, remark, sizeof(remark))) break;
        fn(ctx, name, g32(ent + 4), remark);
    }
    e = 0;
done:
    smb2_closefile(s, &pipe);
out:
    smb2_close(s);
    return e;
}
