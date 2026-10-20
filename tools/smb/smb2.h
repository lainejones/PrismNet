/*
 * PrismNet SMB client: SMB 2.0.2 to 3.1.1 over TCP 445, NTLMv2 (SPNEGO) or
 * guest/anonymous; HMAC-SHA256 (2.x) or AES-CMAC (3.x) signing, AES-128
 * CCM/GCM encryption when a share demands it. One request at a time,
 * credit charge 1 (transfers up to 64 KB). From [MS-SMB2], [MS-NLMP],
 * [MS-SPNG].
 * Paths are UTF-8, relative to the share, '\' separated ("" = the share).
 * Results: 0, a positive NFS3ERR_* (NTSTATUS mapped), or a negative NET_/FS_
 * error (fstypes.h, net.h). GPL-3.0-only, see LICENSE.
 */
#ifndef SMB_SMB2_H
#define SMB_SMB2_H

#include "fstypes.h"
#include "crypto.h"

struct Smb2Id { u8 b[16]; };            /* FileId: persistent + volatile */

struct Smb {
    long  sock;
    unsigned timeout;
    u8   *tx, *rx;
    u32   txsize, rxsize;
    u32   msgid_lo, msgid_hi;
    u8    sessid[8];
    u32   treeid;
    u32   dialect;
    int   sign;                         /* sign requests, check replies */
    int   guest;                        /* the session is guest/anonymous */
    u8    key[16];                      /* session key (= 2.x signing key) */
    u32   maxread, maxwrite;
    long  ntstatus;                     /* NTSTATUS of the last reply */
    /* for SESSION_SETUP; password only in memory */
    char  user[64], domain[32], password[64];
    int   want_guest, sign_required_by_us;
    /* SMB 3.x: options (kept by smb2_init's caller), then session state */
    u32   max_dialect;                  /* highest dialect offered, 0 = 0x311 */
    int   encrypt_mode;                 /* 0 AUTO (only when demanded), 1 YES, -1 NO */
    int   offer_enc;                    /* encryption offered in this NEGOTIATE */
    u32   cipher;                       /* 1 AES-128-CCM, 2 AES-128-GCM, 0 none */
    int   enc;                          /* every message goes encrypted */
    int   rx_enc;                       /* the last frame came encrypted */
    u8    preauth[64];                  /* 3.1.1 pre-auth integrity hash */
    u8    nsalt[8];                     /* encryption nonce: counter + salt */
    u32   nonce;
    struct Aes signkey;                 /* 3.x signing key (AES-CMAC) */
    struct Gcm enckey, deckey;          /* client->server, server->client */
};

/* buffers are the caller's; txsize/rxsize bound the largest message */
void  smb2_init(struct Smb *s, u8 *tx, u32 txsize, u8 *rx, u32 rxsize);
/* TCP connect, NEGOTIATE, SESSION_SETUP, TREE_CONNECT \\host\share */
long  smb2_connect(struct Smb *s, u32 ip, unsigned port, const char *host, const char *share);
void  smb2_close(struct Smb *s);

/* the attributes of path (CREATE + CLOSE, compounded) */
long  smb2_getattr(struct Smb *s, const char *path, struct Fattr *a);
/* open for reading (files or directories) / close */
long  smb2_open(struct Smb *s, const char *path, int dir, struct Smb2Id *id, struct Fattr *a);
long  smb2_closefile(struct Smb *s, const struct Smb2Id *id);
/* data points into the receive buffer until the next call */
long  smb2_read(struct Smb *s, const struct Smb2Id *id, u32 off_hi, u32 off_lo, u32 count,
                const u8 **data, u32 *got, int *eof);
/* one QUERY_DIRECTORY round (FileIdBothDirectoryInformation): the raw
 * buffer for smb2_dir_next(); restart: from the first entry. 0 with *eof
 * set at the end. */
long  smb2_querydir(struct Smb *s, const struct Smb2Id *id, int restart, u32 bufsize,
                    const u8 **buf, u32 *len, int *eof);
/* walk a QUERY_DIRECTORY buffer: next entry at *pos, name as UTF-8 */
int   smb2_dir_next(const u8 *buf, u32 len, u32 *pos, char *name, u32 namemax,
                    struct Fattr *a);
long  smb2_fsstat(struct Smb *s, struct Fsstat *f);

/* ---- writing (stage 2) ---- */
#define SMB2_OPEN        1              /* CreateDisposition */
#define SMB2_CREATE      2
#define SMB2_OPEN_IF     3
#define SMB2_OVERWRITE_IF 5
/* open path for reading and writing (files) with a disposition */
long  smb2_openw(struct Smb *s, const char *path, u32 disposition, struct Smb2Id *id,
                 struct Fattr *a);
long  smb2_write(struct Smb *s, const struct Smb2Id *id, u32 off_hi, u32 off_lo,
                 const u8 *data, u32 count, u32 *written);
long  smb2_flush(struct Smb *s, const struct Smb2Id *id);
long  smb2_mkdir(struct Smb *s, const char *path, struct Fattr *a);
long  smb2_delete(struct Smb *s, const char *path, int dir);
long  smb2_rename(struct Smb *s, const char *from, const char *to);
/* size: set_size; times: mtime (UTC seconds) when set_mtime; readonly bit
 * from the owner write bit when set_mode */
long  smb2_setdosattrs(struct Smb *s, const char *path, u32 attrs);
long  smb2_readsmall(struct Smb *s, const char *path, u32 count, const u8 **data, u32 *got, int *eof);
long  smb2_setattr(struct Smb *s, const char *path, int set_size, u32 size,
                   int set_mtime, u32 mtime, int set_mode, u32 mode);

/* ---- share list (stage 3) ---- */
/* the shares of a server via SRVSVC over IPC$ (connects and closes on its
 * own); fn gets each name, type (0 disk, 1 printer, 3 IPC; 0x80000000 =
 * hidden "$" share) and remark. buf (8 KB or more) holds the reply. */
long  smb2_shares(struct Smb *s, u32 ip, unsigned port, const char *host, u8 *buf, u32 bufsize,
                  void (*fn)(void *ctx, const char *name, u32 type, const char *remark),
                  void *ctx);

/* path helpers */
u32   smb2_fnv(const char *s);          /* stable 32-bit id of a path */
u32   utf8_to_utf16(const char *s, u8 *out, u32 max);   /* bytes written */
u32   utf16_to_utf8(const u8 *s, u32 bytes, char *out, u32 max);

#endif
