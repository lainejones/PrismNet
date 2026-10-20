/*
 * smbtool - host test for the PrismNet SMB protocol layer.
 *   smbtool host/share ls [path]
 *   smbtool host/share cat path > file
 *   smbtool host/share info
 *   smbtool host/- shares          (the server's share list)
 * Guest/anonymous unless SMBUSER (and SMBPASS, SMBDOMAIN) are set in the
 * environment by whoever runs it. SMBSIGN=1 forces signing, SMBENC=YES|NO
 * sets encryption (default: only when a share demands it), SMBPROTO=210,
 * 300, 302 or 311 caps the dialect.
 * Builds on Linux/WSL with net_posix.c. GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "smb2.h"
#include "net.h"

static u8 txbuf[70000], rxbuf[140000];

static void show_share(void *ctx, const char *name, u32 type, const char *remark)
{
    static const char *kind[] = { "disk", "printer", "device", "IPC" };
    (void)ctx;
    printf("%-20s %-7s%s  %s\n", name, kind[type & 3], type & 0x80000000UL ? " hidden" : "",
           remark);
}

static void show(const char *name, const struct Fattr *a)
{
    printf("%c %04lo %12llu %10lu  %s\n", a->type == NF3DIR ? 'd' : '-', a->mode,
           (unsigned long long)(a->size_hi & 0xffffffffUL) << 32 | (a->size_lo & 0xffffffffUL),
           a->mtime, name);
}

int main(int argc, char **argv)
{
    struct Smb s;
    char host[128], share[128], *slash;
    const char *env;
    u32 ip;
    long e;

    if (argc < 3 || !(slash = strchr(argv[1], '/'))) {
        fprintf(stderr, "usage: smbtool host/share ls|cat|info [path]\n");
        return 20;
    }
    memcpy(host, argv[1], (size_t)(slash - argv[1]));
    host[slash - argv[1]] = 0;
    strcpy(share, slash + 1);
    if (!(ip = net_resolve(host))) { fprintf(stderr, "cannot resolve %s\n", host); return 20; }

    smb2_init(&s, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    if ((env = getenv("SMBUSER"))) strncpy(s.user, env, sizeof(s.user) - 1);
    if ((env = getenv("SMBPASS"))) strncpy(s.password, env, sizeof(s.password) - 1);
    if ((env = getenv("SMBDOMAIN"))) strncpy(s.domain, env, sizeof(s.domain) - 1);
    s.sign_required_by_us = getenv("SMBSIGN") != NULL;
    if ((env = getenv("SMBENC"))) s.encrypt_mode = !strcmp(env, "YES") ? 1 : !strcmp(env, "NO") ? -1 : 0;
    if ((env = getenv("SMBPROTO"))) s.max_dialect = (u32)strtoul(env, NULL, 16);
    if (!strcmp(argv[2], "shares")) {
        static u8 buf[65536];
        if ((e = smb2_shares(&s, ip, 445, host, buf, sizeof(buf), show_share, NULL))) {
            fprintf(stderr, "shares: %ld (NTSTATUS %08lx, dialect %04lx)\n", e,
                    (unsigned long)s.ntstatus, s.dialect);
            return 20;
        }
        return 0;
    }
    if ((e = smb2_connect(&s, ip, 445, host, share))) {
        fprintf(stderr, "connect: %ld (NTSTATUS %08lx, dialect %04lx, session %s)\n", e,
                (unsigned long)s.ntstatus, s.dialect, s.sessid[0] || s.sessid[1] ? "yes" : "no");
        return 20;
    }
    fprintf(stderr, "dialect %04lx, %s, signing %s, encryption %s, maxread %lu\n", s.dialect,
            s.guest ? "guest" : "user", s.sign ? "on" : "off",
            s.enc ? (s.cipher == 2 ? "AES-128-GCM" : "AES-128-CCM") : "off", s.maxread);

    if (!strcmp(argv[2], "info")) {
        struct Fsstat f;
        struct Fattr a;
        if ((e = smb2_fsstat(&s, &f)) || (e = smb2_getattr(&s, "", &a))) {
            fprintf(stderr, "info: %ld (NTSTATUS %08lx)\n", e, (unsigned long)s.ntstatus); return 20;
        }
        printf("total %lu:%lu free %lu:%lu\n", f.tbytes_hi, f.tbytes_lo, f.fbytes_hi, f.fbytes_lo);
        show("(root)", &a);
    } else if (!strcmp(argv[2], "ls")) {
        struct Smb2Id id;
        const u8 *buf;
        u32 len, pos;
        int eof = 0, restart = 1;
        char name[300];
        struct Fattr a;
        if ((e = smb2_open(&s, argc > 3 ? argv[3] : "", 1, &id, &a))) {
            fprintf(stderr, "open: %ld (NTSTATUS %08lx)\n", e, (unsigned long)s.ntstatus); return 20;
        }
        while (!eof) {
            if ((e = smb2_querydir(&s, &id, restart, 16384, &buf, &len, &eof))) {
                fprintf(stderr, "querydir: %ld\n", e); return 20;
            }
            restart = 0;
            for (pos = 0; smb2_dir_next(buf, len, &pos, name, sizeof(name), &a); )
                show(name, &a);
        }
        smb2_closefile(&s, &id);
    } else if (!strcmp(argv[2], "cat") && argc > 3) {
        struct Smb2Id id;
        struct Fattr a;
        u32 off = 0, got;
        const u8 *d;
        int eof = 0;
        if ((e = smb2_open(&s, argv[3], 0, &id, &a))) {
            fprintf(stderr, "open: %ld (NTSTATUS %08lx)\n", e, (unsigned long)s.ntstatus); return 20;
        }
        /* SMBCHUNK=bytes per READ (default 65536), SMBLIMIT=MB to stop at;
         * any READ slower than a second is reported (stall hunting) */
        u32 chunk = getenv("SMBCHUNK") ? (u32)atol(getenv("SMBCHUNK")) : 65536;
        unsigned long long limit = getenv("SMBLIMIT") ? (unsigned long long)atol(getenv("SMBLIMIT")) << 20 : ~0ULL;
        unsigned long long pos = 0;
        struct timespec t0, t1, start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!eof && pos < limit) {
            double dt;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if ((e = smb2_read(&s, &id, (u32)(pos >> 32), (u32)(pos & 0xffffffffUL), chunk, &d, &got, &eof))) {
                fprintf(stderr, "read at %llu: %ld\n", pos, e); return 20;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (dt > 1.0)
                fprintf(stderr, "slow READ at %llu: %.1f s\n", pos, dt);
            fwrite(d, 1, got, stdout);
            pos += got;
            if (!got) break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        off = (u32)pos;
        fprintf(stderr, "%llu bytes in %.0f s\n", pos,
                (t1.tv_sec - start.tv_sec) + (t1.tv_nsec - start.tv_nsec) / 1e9);
        smb2_closefile(&s, &id);
        fprintf(stderr, "%lu bytes\n", off);
    } else if (!strcmp(argv[2], "put") && argc > 4) {
        /* put localfile path: create/overwrite, write in 32 KB pieces, flush */
        FILE *in = fopen(argv[3], "rb");
        static u8 buf[32768];
        struct Smb2Id id;
        struct Fattr a;
        size_t n;
        unsigned long long pos = 0;
        if (!in) { perror(argv[3]); return 20; }
        if ((e = smb2_openw(&s, argv[4], SMB2_OVERWRITE_IF, &id, &a))) {
            fprintf(stderr, "openw: %ld (NTSTATUS %08lx)\n", e, (unsigned long)s.ntstatus); return 20;
        }
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            u32 done_ = 0, w;
            while (done_ < n) {
                if ((e = smb2_write(&s, &id, (u32)(pos >> 32), (u32)(pos & 0xffffffffUL),
                                    buf + done_, (u32)n - done_, &w)) || !w) {
                    fprintf(stderr, "write at %llu: %ld\n", pos, e); return 20;
                }
                done_ += w; pos += w;
            }
        }
        fclose(in);
        e = smb2_flush(&s, &id);
        smb2_closefile(&s, &id);
        fprintf(stderr, "%llu bytes written, flush %ld\n", pos, e);
    } else if (!strcmp(argv[2], "mkdir") && argc > 3) {
        struct Fattr a;
        fprintf(stderr, "mkdir: %ld\n", smb2_mkdir(&s, argv[3], &a));
    } else if (!strcmp(argv[2], "rm") && argc > 3) {
        fprintf(stderr, "rm: %ld\n", smb2_delete(&s, argv[3], 0));
    } else if (!strcmp(argv[2], "rmdir") && argc > 3) {
        fprintf(stderr, "rmdir: %ld\n", smb2_delete(&s, argv[3], 1));
    } else if (!strcmp(argv[2], "mv") && argc > 4) {
        fprintf(stderr, "mv: %ld\n", smb2_rename(&s, argv[3], argv[4]));
    } else if (!strcmp(argv[2], "truncate") && argc > 4) {
        fprintf(stderr, "truncate: %ld\n",
                smb2_setattr(&s, argv[3], 1, (u32)atol(argv[4]), 0, 0, 0, 0));
    } else if (!strcmp(argv[2], "touch") && argc > 4) {   /* touch path unixtime */
        fprintf(stderr, "touch: %ld\n",
                smb2_setattr(&s, argv[3], 0, 0, 1, (u32)atol(argv[4]), 0, 0));
    } else if (!strcmp(argv[2], "attrs") && argc > 3) {     /* the raw FILE_ATTRIBUTE_* bits */
        struct Fattr a;
        long e = smb2_getattr(&s, argv[3], &a);
        if (e) fprintf(stderr, "attrs: %ld\n", e);
        else printf("attrs %s: %08lx\n", argv[3], (unsigned long)a.dosattrs);
    } else if (!strcmp(argv[2], "setattrs") && argc > 4) {  /* set them exactly (hex) */
        fprintf(stderr, "setattrs: %ld\n", smb2_setdosattrs(&s, argv[3], (u32)strtoul(argv[4], NULL, 16)));
    } else if (!strcmp(argv[2], "readonly") && argc > 3) {
        fprintf(stderr, "readonly: %ld\n", smb2_setattr(&s, argv[3], 0, 0, 0, 0, 1, 0444));
    } else if (!strcmp(argv[2], "writable") && argc > 3) {
        fprintf(stderr, "writable: %ld\n", smb2_setattr(&s, argv[3], 0, 0, 0, 0, 1, 0644));
    }
    smb2_close(&s);
    return 0;
}
