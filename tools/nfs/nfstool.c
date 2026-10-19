/*
 * nfstool - host test for the PrismNet NFS protocol layer.
 *   nfstool host:/export ls [path]
 *   nfstool host:/export cat path > file
 *   nfstool host:/export info
 *   nfstool host:/export put localfile name   (writes UNSTABLE + COMMIT)
 *   nfstool host:/export rm|mkdir|rmdir name
 *   nfstool host:/export mv from to
 * Builds on Linux/WSL with net_posix.c. GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nfs3.h"
#include "net.h"

static u8 txbuf[70000], rxbuf[70000], filebuf[32768];

static int walk(struct Rpc *r, const struct Fh *root, const char *path, struct Fh *out)
{
    char part[256];

    *out = *root;
    while (*path) {
        const char *e = strchr(path, '/');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        struct Fh next;
        long st;

        if (n >= sizeof(part)) return -1;
        memcpy(part, path, n);
        part[n] = 0;
        path += n + (e ? 1 : 0);
        if (!n) continue;
        if ((st = nfs3_lookup(r, out, part, &next, 0, 0)) != 0) {
            fprintf(stderr, "lookup %s: %ld\n", part, st);
            return -1;
        }
        *out = next;
    }
    return 0;
}

static int show(void *ctx, const char *name, u32 nlen, const struct Fattr *a,
                const struct Fh *fh, u32 chi, u32 clo)
{
    (void)ctx; (void)fh; (void)chi; (void)clo;
    if (a)
        printf("%c %06lo %10lu %10lu  %.*s\n", a->type == NF3DIR ? 'd' : a->type == NF3LNK ? 'l' : '-',
               a->mode & 07777, a->size_lo, a->mtime, (int)nlen, name);
    else
        printf("? %.*s\n", (int)nlen, name);
    return 0;
}

int main(int argc, char **argv)
{
    struct Rpc r;
    struct Fh root, fh;
    char host[128], *colon;
    const char *export;
    unsigned mport, nport;
    u32 ip;
    long st;

    if (argc < 3 || !(colon = strchr(argv[1], ':'))) {
        fprintf(stderr, "usage: nfstool host:/export ls|cat|info [path]\n");
        return 20;
    }
    memcpy(host, argv[1], (size_t)(colon - argv[1]));
    host[colon - argv[1]] = 0;
    export = colon + 1;
    if (!(ip = net_resolve(host))) { fprintf(stderr, "cannot resolve %s\n", host); return 20; }

    rpc_init(&r, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    r.resvport = getenv("NORESV") ? 0 : 1;
    if ((st = rpc_connect(&r, ip, 111)) || (st = pmap_getport(&r, MOUNT_PROG, 3, &mport)) ||
        (st = pmap_getport(&r, NFS_PROG, 3, &nport))) {
        fprintf(stderr, "portmap: %ld\n", st); return 20;
    }
    fprintf(stderr, "mountd port %u, nfs port %u\n", mport, nport);
    if ((st = rpc_connect(&r, ip, mport)) || (st = mnt3_mnt(&r, export, &root))) {
        fprintf(stderr, "mount %s: %ld\n", export, st); return 20;
    }
    rpc_close(&r);
    if ((st = rpc_connect(&r, ip, nport))) { fprintf(stderr, "nfs connect: %ld\n", st); return 20; }

    if (!strcmp(argv[2], "info")) {
        struct Fsstat s; struct Fsinfo i; struct Fattr a;
        if ((st = nfs3_fsinfo(&r, &root, &i)) || (st = nfs3_fsstat(&r, &root, &s)) ||
            (st = nfs3_getattr(&r, &root, &a))) { fprintf(stderr, "info: %ld\n", st); return 20; }
        printf("rtmax %lu rtpref %lu wtmax %lu wtpref %lu dtpref %lu\n",
               i.rtmax, i.rtpref, i.wtmax, i.wtpref, i.dtpref);
        printf("total %lu:%lu free %lu:%lu avail %lu:%lu\n", s.tbytes_hi, s.tbytes_lo,
               s.fbytes_hi, s.fbytes_lo, s.abytes_hi, s.abytes_lo);
        printf("root mode %lo uid %lu gid %lu mtime %lu\n", a.mode, a.uid, a.gid, a.mtime);
    } else if (!strcmp(argv[2], "ls")) {
        u32 cookie[2] = { 0, 0 };
        u8 verf[8] = { 0 };
        int eof = 0;
        if (walk(&r, &root, argc > 3 ? argv[3] : "", &fh)) return 20;
        while (!eof)
            if ((st = nfs3_readdirplus(&r, &fh, cookie, verf, 8192, show, 0, &eof))) {
                fprintf(stderr, "readdirplus: %ld\n", st); return 20;
            }
    } else if (!strcmp(argv[2], "cat") && argc > 3) {
        u32 off = 0, got;
        const u8 *d;
        int eof = 0;
        if (walk(&r, &root, argv[3], &fh)) return 20;
        while (!eof) {
            if ((st = nfs3_read(&r, &fh, 0, off, 32768, &d, &got, &eof))) {
                fprintf(stderr, "read: %ld\n", st); return 20;
            }
            fwrite(d, 1, got, stdout);
            off += got;
            if (!got) break;
        }
        fprintf(stderr, "%lu bytes\n", off);
    } else if (!strcmp(argv[2], "put") && argc > 4) {
        FILE *in = fopen(argv[3], "rb");
        struct Sattr sa;
        struct Fattr a;
        u8 verf[8], cverf[8];
        u32 off = 0, w;
        size_t n;
        int ha, first = 1;
        if (!in) { perror(argv[3]); return 20; }
        memset(&sa, 0, sizeof(sa));
        sa.set_mode = 1; sa.mode = 0644;
        sa.set_size = 1; sa.size = 0;
        if ((st = nfs3_create(&r, &root, argv[4], CREATE_UNCHECKED, &sa, &fh, &a, &ha))) {
            fprintf(stderr, "create: %ld\n", st); return 20;
        }
        while ((n = fread(filebuf, 1, sizeof(filebuf), in)) > 0) {
            u32 done_ = 0;
            while (done_ < n) {
                u8 v[8];
                if ((st = nfs3_write(&r, &fh, 0, off, filebuf + done_, (u32)n - done_, UNSTABLE, &w, v))) {
                    fprintf(stderr, "write: %ld\n", st); return 20;
                }
                if (first) { memcpy(verf, v, 8); first = 0; }
                else if (memcmp(verf, v, 8)) fprintf(stderr, "write verifier changed\n");
                done_ += w; off += w;
            }
        }
        fclose(in);
        if ((st = nfs3_commit(&r, &fh, cverf))) { fprintf(stderr, "commit: %ld\n", st); return 20; }
        fprintf(stderr, "%lu bytes written, commit verifier %s\n", off,
                first || !memcmp(verf, cverf, 8) ? "matches" : "CHANGED");
    } else if (!strcmp(argv[2], "rm") && argc > 3) {
        st = nfs3_remove(&r, &root, argv[3]);
        fprintf(stderr, "remove: %ld\n", st);
    } else if (!strcmp(argv[2], "rmdir") && argc > 3) {
        st = nfs3_rmdir(&r, &root, argv[3]);
        fprintf(stderr, "rmdir: %ld\n", st);
    } else if (!strcmp(argv[2], "mkdir") && argc > 3) {
        struct Sattr sa; struct Fattr a; int ha;
        memset(&sa, 0, sizeof(sa)); sa.set_mode = 1; sa.mode = 0755;
        st = nfs3_mkdir(&r, &root, argv[3], &sa, &fh, &a, &ha);
        fprintf(stderr, "mkdir: %ld\n", st);
    } else if (!strcmp(argv[2], "mv") && argc > 4) {
        st = nfs3_rename(&r, &root, argv[3], &root, argv[4]);
        fprintf(stderr, "rename: %ld\n", st);
    }
    rpc_close(&r);
    return 0;
}
