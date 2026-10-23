/*
 * httpd - PrismNet's web server: the files of one folder, read-only.
 *
 * Started by inetd for each connection: "httpd SOCKET=<id> [options]".
 * GET and HEAD over HTTP/1.0 and 1.1, persistent connections (one process
 * serves every request a browser sends on its connection), single byte
 * ranges (resumed downloads, seeking in audio and video), a folder's
 * index file or a generated listing, and a content type by file name.
 * Nothing on the Amiga can be changed through it.
 *
 *   httpd SOCKET/N   the connection, from inetd
 *         ROOT=dir       the folder served as "/" (default SYS:WWW)
 *         INDEX=name     a folder's own page (default index.html)
 *         NOLIST         no listings: a folder without an index is refused
 *         IDLE=s         close a connection idle for s seconds (default 15)
 *         LOG=file       append one line per request to file
 *
 * Paths can't leave ROOT: the decoded URL is split at "/", empty, "." and
 * ".." parts are refused, and so is any ":" (a volume) - in AmigaDOS an
 * empty part ("a//b") means the parent folder, so it can't pass either.
 *
 * GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <bsdsocket/socketbasetags.h>
#include <proto/bsdsocket.h>

static const char version[] __attribute__((used)) = "$VER: httpd 1.0 (23.10.2026) PrismNet";

struct Library *SocketBase;

#define BUFSIZE  16384
#define PATHMAX  512
#define HDRMAX   8192
#define MAXREQ   200                /* requests on one connection */

static LONG sock = -1;
static LONG idle_secs = 15;
static char root[PATHMAX] = "SYS:WWW";
static char index_name[108] = "index.html";
static BOOL nolist;
static const char *logfile;
static char peer[16];

static char req[HDRMAX + 1];        /* the request line and headers */
static LONG reqlen;
static const char *hdrs;            /* the header lines: the "\n" before them */
static char rbuf[2048];             /* received, not yet used */
static LONG rpos, rlen;
static UBYTE *xbuf;                 /* file data / listing */

/* ---- the connection --------------------------------------------------------- */

static BOOL send_all(const void *p, LONG n)
{
    const char *s = p;
    while (n > 0) {
        LONG k = send(sock, (APTR)s, n, 0);
        if (k <= 0) return FALSE;
        s += k; n -= k;
    }
    return TRUE;
}

/* Read up to the blank line after the headers. 1 = a request, 0 = the
 * connection ended or went idle, -1 = headers too long. */
static int get_request(void)
{
    LONG blank = 0;
    reqlen = 0;
    for (;;) {
        while (rpos < rlen) {
            char c = rbuf[rpos++];
            if (c == '\r') continue;
            if (c == '\n') {
                if (!reqlen) continue;      /* stray line ends between requests */
                if (++blank == 2) { req[reqlen] = 0; return 1; }
            } else
                blank = 0;
            if (reqlen >= HDRMAX) return -1;
            req[reqlen++] = c;
        }
        {
            fd_set rd;
            struct timeval tv;
            ULONG sigs = SIGBREAKF_CTRL_C;
            FD_ZERO(&rd); FD_SET(sock, &rd);
            tv.tv_secs = idle_secs; tv.tv_micro = 0;
            if (WaitSelect(sock + 1, &rd, NULL, NULL, &tv, &sigs) <= 0)
                return 0;
        }
        rpos = 0;
        if ((rlen = recv(sock, rbuf, sizeof(rbuf), 0)) <= 0)
            return 0;
    }
}

/* a header's value (name without the colon), or NULL */
static const char *header(const char *name)
{
    size_t n = strlen(name);
    const char *l = hdrs;
    while (l) {
        l++;
        if (!strncasecmp(l, name, n) && l[n] == ':') {
            l += n + 1;
            while (*l == ' ' || *l == '\t') l++;
            return l;
        }
        l = strchr(l, '\n');
    }
    return NULL;
}

/* does the header's value contain the word (case ignored)? */
static BOOL header_has(const char *name, const char *word)
{
    const char *v = header(name);
    size_t n = strlen(word);
    while (v && *v && *v != '\n') {
        if (!strncasecmp(v, word, n)) return TRUE;
        v++;
    }
    return FALSE;
}

static void log_line(const char *method, const char *target, int status, ULONG bytes)
{
    BPTR fh;
    char line[PATHMAX + 96];
    LONG n;
    if (!logfile) return;
    n = snprintf(line, sizeof(line), "%s \"%s %.400s\" %d %lu\n", peer, method, target, status,
                 (unsigned long)bytes);
    if ((fh = Open((CONST_STRPTR)logfile, MODE_READWRITE)) != 0) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, line, n < (LONG)sizeof(line) ? n : (LONG)sizeof(line) - 1);
        Close(fh);
    }
}

/* ---- answers ---------------------------------------------------------------- */

static const char *reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 505: return "HTTP Version Not Supported";
    }
    return "Error";
}

static BOOL keep;                   /* keep the connection after this answer */

static BOOL head_out(int status, const char *type, ULONG len, const char *extra)
{
    char h[768];
    LONG n = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nServer: PrismNet httpd\r\n%s%s%sContent-Length: %lu\r\n%s%s\r\n",
        status, reason(status), type ? "Content-Type: " : "", type ? type : "", type ? "\r\n" : "",
        (unsigned long)len, keep ? "" : "Connection: close\r\n", extra ? extra : "");
    return n > 0 && n < (LONG)sizeof(h) && send_all(h, n);
}

/* a short HTML page for an error */
static void error_out(int status, BOOL body)
{
    char b[256];
    LONG n = snprintf(b, sizeof(b), "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>"
                      "<body><h1>%d %s</h1></body></html>\n", status, reason(status), status, reason(status));
    if (status == 400 || status == 431 || status == 505) keep = FALSE;
    head_out(status, "text/html", n, status == 405 ? "Allow: GET, HEAD\r\n" : NULL);
    if (body) send_all(b, n);
}

/* ---- paths ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The URL path -> an Amiga path under ROOT. `slash` = the URL ends in "/".
 * FALSE = refused (it would leave ROOT, or can't be decoded). */
static BOOL map_path(const char *url, char *out, BOOL *slash)
{
    char dec[PATHMAX], *p, *part;
    LONG n = 0;

    for (; *url && *url != '?' && *url != '#'; url++) {
        char c = *url;
        if (c == '%') {
            int a = hexval(url[1]), b = a < 0 ? -1 : hexval(url[2]);
            if (b < 0) return FALSE;
            c = (char)(a * 16 + b);
            url += 2;
        }
        if (!c || c == ':' || c == '\\' || n >= PATHMAX - 1) return FALSE;
        dec[n++] = c;
    }
    dec[n] = 0;
    if (dec[0] != '/') return FALSE;
    *slash = dec[n - 1] == '/';
    strcpy(out, root);
    for (p = dec + 1; *p; ) {
        part = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        else if (!*part) break;             /* the URL's last "/" */
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) return FALSE;
        if (!AddPart((STRPTR)out, (CONST_STRPTR)part, PATHMAX)) return FALSE;
    }
    return TRUE;
}

static const struct { const char *ext, *type; } types[] = {
    { "html", "text/html; charset=iso-8859-1" }, { "htm", "text/html; charset=iso-8859-1" },
    { "txt", "text/plain; charset=iso-8859-1" }, { "readme", "text/plain; charset=iso-8859-1" },
    { "guide", "text/plain; charset=iso-8859-1" }, { "doc", "text/plain; charset=iso-8859-1" },
    { "c", "text/plain; charset=iso-8859-1" }, { "h", "text/plain; charset=iso-8859-1" },
    { "rexx", "text/plain; charset=iso-8859-1" }, { "md", "text/plain; charset=iso-8859-1" },
    { "css", "text/css" }, { "js", "text/javascript" }, { "json", "application/json" },
    { "xml", "application/xml" }, { "png", "image/png" }, { "gif", "image/gif" },
    { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "svg", "image/svg+xml" },
    { "ico", "image/x-icon" }, { "webp", "image/webp" }, { "bmp", "image/bmp" },
    { "iff", "image/x-iff" }, { "ilbm", "image/x-ilbm" }, { "pdf", "application/pdf" },
    { "zip", "application/zip" }, { "lha", "application/x-lha" }, { "lzh", "application/x-lha" },
    { "lzx", "application/x-lzx" }, { "gz", "application/gzip" }, { "tar", "application/x-tar" },
    { "mp3", "audio/mpeg" }, { "wav", "audio/wav" }, { "ogg", "audio/ogg" },
    { "mod", "audio/x-mod" }, { "mp4", "video/mp4" }, { "webm", "video/webm" },
    { NULL, NULL }
};

static const char *content_type(const char *name)
{
    const char *dot = strrchr(name, '.'), *slash = strrchr(name, '/'), *colon = strrchr(name, ':');
    int i;
    if (!dot || dot < slash || dot < colon) return "application/octet-stream";
    for (i = 0; types[i].ext; i++)
        if (!strcasecmp(dot + 1, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

/* ---- a file ----------------------------------------------------------------- */

/* "bytes=a-b", "bytes=a-", "bytes=-n" against size; FALSE = no usable single range */
static BOOL parse_range(const char *r, ULONG size, ULONG *from, ULONG *to)
{
    char *e;
    unsigned long a, b;
    if (strncasecmp(r, "bytes=", 6)) return FALSE;
    r += 6;
    if (*r == '-') {
        b = strtoul(r + 1, &e, 10);
        if (e == r + 1 || !b) return FALSE;
        *from = b >= size ? 0 : size - b;
        *to = size - 1;
        return size > 0;
    }
    a = strtoul(r, &e, 10);
    if (e == r || *e != '-') return FALSE;
    r = e + 1;
    b = (*r >= '0' && *r <= '9') ? strtoul(r, &e, 10) : size - 1;
    if (*r >= '0' && *r <= '9' && *e && *e != '\n' && *e != ' ') return FALSE;   /* a list */
    if (b >= size) b = size - 1;
    *from = a; *to = b;
    return TRUE;
}

static int send_file(const char *path, ULONG size, BOOL body, ULONG *sent)
{
    const char *r = header("Range");
    ULONG from = 0, to = size ? size - 1 : 0, left;
    int status = 200;
    char extra[96] = "Accept-Ranges: bytes\r\n";
    BPTR fh;

    if (r && parse_range(r, size, &from, &to)) {
        if (from > to || from >= size) {
            snprintf(extra, sizeof(extra), "Content-Range: bytes */%lu\r\n", (unsigned long)size);
            head_out(416, NULL, 0, extra);
            return 416;
        }
        status = 206;
        snprintf(extra, sizeof(extra), "Content-Range: bytes %lu-%lu/%lu\r\n",
                 (unsigned long)from, (unsigned long)to, (unsigned long)size);
    }
    if (!(fh = Open((CONST_STRPTR)path, MODE_OLDFILE))) {
        error_out(403, body);
        return 403;
    }
    left = size ? to - from + 1 : 0;
    if (from && Seek(fh, from, OFFSET_BEGINNING) < 0) {
        Close(fh);
        error_out(500, body);
        return 500;
    }
    if (!head_out(status, content_type(path), left, extra)) left = 0, keep = FALSE;
    if (body) {
        while (left) {
            LONG n = Read(fh, xbuf, left < BUFSIZE ? left : BUFSIZE);
            if (n <= 0 || !send_all(xbuf, n)) { keep = FALSE; break; }   /* the length was promised */
            left -= n; *sent += n;
        }
    }
    Close(fh);
    return status;
}

/* ---- a folder listing ------------------------------------------------------- */

struct Entry { char name[108]; LONG size; BOOL dir; struct DateStamp date; };

static int cmp_entry(const void *a, const void *b)
{
    const struct Entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static UBYTE *lbuf;                 /* the page being built */
static ULONG llen, lmax;

static void put(const char *s, ULONG n)
{
    if (llen + n > lmax) {
        ULONG m = (llen + n) * 2 + 4096;
        UBYTE *nb = AllocVec(m, MEMF_ANY);
        if (!nb) return;
        if (lbuf) { CopyMem(lbuf, nb, llen); FreeVec(lbuf); }
        lbuf = nb; lmax = m;
    }
    CopyMem((APTR)s, lbuf + llen, n);
    llen += n;
}
static void puts_(const char *s) { put(s, strlen(s)); }

static void put_html(const char *s)    /* text in a page */
{
    for (; *s; s++) {
        switch (*s) {
        case '<': puts_("&lt;"); break;
        case '>': puts_("&gt;"); break;
        case '&': puts_("&amp;"); break;
        case '"': puts_("&quot;"); break;
        default: put(s, 1);
        }
    }
}

static void put_url(const char *s)     /* a name in a link */
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        UBYTE c = (UBYTE)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            put(s, 1);
        else {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };
            put(e, 3);
        }
    }
}

static int send_listing(BPTR lk, const char *url, BOOL body, ULONG *sent)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    struct Entry *ent = NULL;
    LONG n = 0, max = 0, i;
    char shown[PATHMAX], line[160];
    int status = 200;

    llen = 0;
    if (!fib || !Examine(lk, fib)) { if (fib) FreeDosObject(DOS_FIB, fib); error_out(500, body); return 500; }
    while (ExNext(lk, fib)) {
        if (n == max) {
            LONG m = max ? max * 2 : 64;
            struct Entry *ne = AllocVec(m * sizeof(*ne), MEMF_ANY);
            if (!ne) break;
            if (ent) { CopyMem(ent, ne, n * sizeof(*ne)); FreeVec(ent); }
            ent = ne; max = m;
        }
        strncpy(ent[n].name, (char *)fib->fib_FileName, sizeof(ent[n].name) - 1);
        ent[n].name[sizeof(ent[n].name) - 1] = 0;
        ent[n].dir = fib->fib_DirEntryType > 0;
        ent[n].size = fib->fib_Size;
        ent[n].date = fib->fib_Date;
        n++;
    }
    FreeDosObject(DOS_FIB, fib);
    if (n) qsort(ent, n, sizeof(*ent), cmp_entry);

    /* the URL path as decoded text, for the title */
    for (i = 0; *url && *url != '?' && i < PATHMAX - 1; url++) {
        if (*url == '%' && hexval(url[1]) >= 0 && hexval(url[2]) >= 0) {
            shown[i++] = (char)(hexval(url[1]) * 16 + hexval(url[2])); url += 2;
        } else
            shown[i++] = *url;
    }
    shown[i] = 0;
    puts_("<!DOCTYPE html>\n<html><head><meta charset=\"iso-8859-1\"><title>");
    put_html(shown);
    puts_("</title><style>body{font-family:sans-serif}td{padding:0 1em 0 0}"
          "td.s{text-align:right}</style></head><body><h1>");
    put_html(shown);
    puts_("</h1><table>\n");
    if (strcmp(shown, "/")) puts_("<tr><td><a href=\"../\">../</a></td><td></td><td></td></tr>\n");
    for (i = 0; i < n; i++) {
        struct DateTime dt;
        char day[LEN_DATSTRING], tim[LEN_DATSTRING];
        puts_("<tr><td><a href=\"");
        put_url(ent[i].name);
        if (ent[i].dir) puts_("/");
        puts_("\">");
        put_html(ent[i].name);
        if (ent[i].dir) puts_("/");
        puts_("</a></td><td class=\"s\">");
        if (!ent[i].dir) { snprintf(line, sizeof(line), "%ld", (long)ent[i].size); puts_(line); }
        memset(&dt, 0, sizeof(dt));
        dt.dat_Stamp = ent[i].date;
        dt.dat_Format = FORMAT_INT;
        dt.dat_StrDate = (STRPTR)day;
        dt.dat_StrTime = (STRPTR)tim;
        day[0] = tim[0] = 0;
        DateToStr(&dt);
        snprintf(line, sizeof(line), "</td><td>%s %s</td></tr>\n", day, tim);
        puts_(line);
    }
    puts_("</table></body></html>\n");
    if (ent) FreeVec(ent);

    if (llen && lbuf) {
        head_out(200, "text/html; charset=iso-8859-1", llen, NULL);
        if (body) { if (!send_all(lbuf, llen)) keep = FALSE; else *sent = llen; }
    } else {
        error_out(500, body);
        status = 500;
    }
    return status;
}

/* ---- one request ------------------------------------------------------------ */

static void serve(void)
{
    char *method = req, *target, *ver, path[PATHMAX], ipath[PATHMAX];
    BOOL body, slash = FALSE, http11;
    int status;
    ULONG sent = 0;
    BPTR lk;
    struct FileInfoBlock *fib;

    hdrs = strchr(req, '\n');               /* found before the request line is cut up */
    target = strchr(method, ' ');
    ver = target ? strchr(target + 1, ' ') : NULL;
    if (!target || !ver || (hdrs && ver > hdrs)) {
        keep = FALSE; error_out(400, TRUE); log_line("-", "-", 400, 0); return;
    }
    *target++ = 0; *ver++ = 0;
    if (strncmp(ver, "HTTP/1.", 7)) { error_out(505, TRUE); log_line(method, target, 505, 0); return; }
    if (ver[7] != '0') ver[7] = '1';
    http11 = ver[7] == '1';
    keep = http11 ? !header_has("Connection", "close") : header_has("Connection", "keep-alive");
    body = strcmp(method, "HEAD") != 0;
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        keep = FALSE;                       /* it may have a body we won't read */
        error_out(405, TRUE); log_line(method, target, 405, 0); return;
    }
    if (!strncasecmp(target, "http://", 7)) {   /* absolute form: drop the host */
        char *s = strchr(target + 7, '/');
        target = s ? s : "/";
    }
    if (!map_path(target, path, &slash)) { error_out(403, body); log_line(method, target, 403, 0); return; }
    if (!(lk = Lock((CONST_STRPTR)path, ACCESS_READ))) {
        error_out(404, body); log_line(method, target, 404, 0); return;
    }
    if (!(fib = AllocDosObject(DOS_FIB, NULL)) || !Examine(lk, fib)) {
        if (fib) FreeDosObject(DOS_FIB, fib);
        UnLock(lk);
        error_out(500, body); log_line(method, target, 500, 0); return;
    }
    if (fib->fib_DirEntryType > 0) {
        if (!slash) {                       /* "dir" -> "dir/", so its links resolve */
            char loc[PATHMAX + 16];
            char *q = strchr(target, '?');
            if (q) *q = 0;
            snprintf(loc, sizeof(loc), "Location: %s/\r\n", target);
            head_out(301, NULL, 0, loc);
            status = 301;
        } else {
            BPTR il;
            strcpy(ipath, path);
            if (AddPart((STRPTR)ipath, (CONST_STRPTR)index_name, PATHMAX) &&
                (il = Lock((CONST_STRPTR)ipath, ACCESS_READ)) != 0) {
                struct FileInfoBlock *ifib = AllocDosObject(DOS_FIB, NULL);
                if (ifib && Examine(il, ifib) && ifib->fib_DirEntryType < 0) {
                    UnLock(il); il = 0;
                    status = send_file(ipath, ifib->fib_Size, body, &sent);
                } else
                    status = -1;
                if (ifib) FreeDosObject(DOS_FIB, ifib);
                if (il) UnLock(il);
            } else
                status = -1;
            if (status < 0) {
                if (nolist) { error_out(403, body); status = 403; }
                else status = send_listing(lk, target, body, &sent);
            }
        }
    } else
        status = send_file(path, fib->fib_Size, body, &sent);
    FreeDosObject(DOS_FIB, fib);
    UnLock(lk);
    log_line(method, target, status, sent);
}

int main(void)
{
    LONG args[6] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"SOCKET/K/N,ROOT/K,INDEX/K,NOLIST/S,IDLE/K/N,LOG/K", args, NULL);
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int k, served = 0;

    if (!rda || !args[0])
        return RETURN_FAIL;                 /* inetd starts us; no console to talk to */
    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if ((sock = ObtainSocket(*(LONG *)args[0], AF_INET, SOCK_STREAM, 0)) < 0) {
        CloseLibrary(SocketBase);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (args[1]) strncpy(root, (const char *)args[1], sizeof(root) - 1);
    if (args[2]) strncpy(index_name, (const char *)args[2], sizeof(index_name) - 1);
    nolist = args[3] != 0;
    if (args[4] && *(LONG *)args[4] > 0) idle_secs = *(LONG *)args[4];
    logfile = (const char *)args[5];
    if (getpeername(sock, (struct sockaddr *)&sa, &len) == 0) {
        ULONG a = sa.sin_addr.s_addr;
        snprintf(peer, sizeof(peer), "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24), (unsigned long)((a >> 16) & 255),
                 (unsigned long)((a >> 8) & 255), (unsigned long)(a & 255));
    }
    xbuf = AllocVec(BUFSIZE, MEMF_ANY);

    if (xbuf) {
        do {
            if ((k = get_request()) < 0) { keep = FALSE; error_out(431, TRUE); break; }
            if (!k) break;
            serve();
        } while (keep && ++served < MAXREQ);
    } else if (get_request() > 0) {
        keep = FALSE;
        error_out(500, TRUE);
    }

    if (lbuf) FreeVec(lbuf);
    if (xbuf) FreeVec(xbuf);
    CloseSocket(sock);
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return RETURN_OK;
}
