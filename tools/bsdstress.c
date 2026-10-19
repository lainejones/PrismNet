/*
 * bsdstress - PrismNet concurrency test: N processes fetching at once.
 * GPL-3.0-only, see LICENSE.
 *
 * Mimics a browser: each child process opens its own bsdsocket.library
 * base, resolves the host, connects and reads an HTTP response to the end,
 * ROUNDS times. The parent waits for all children and prints a summary.
 *
 *   bsdstress [HOST=aminet.net] [PATH=/] [TASKS=8] [ROUNDS=2]
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#define MAXTASKS 16

struct Library *SocketBase;       /* unused in children: they shadow it */

static struct Task *parent;
static const char *host = "aminet.net", *path = "/";
static int rounds = 2;
static volatile int next_id, done_count;

static struct result {
    LONG  stage;        /* how far it got: 1 open 2 resolve 3 socket 4 connect 5 send 6 done */
    LONG  err;          /* errno at failure */
    ULONG bytes;
    LONG  ok_rounds;
} results[MAXTASKS];

static void child(void)
{
    struct Library *SocketBase;
    struct result *res;
    struct hostent *h;
    struct sockaddr_in sa;
    char req[200], buf[2048];
    int id, r;

    Forbid();
    id = next_id++;
    Permit();
    res = &results[id];

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase) {
        res->stage = 1;
        for (r = 0; r < rounds; r++) {
            LONG s, n;

            h = gethostbyname((STRPTR)host);
            if (!h) { res->err = -1; break; }
            res->stage = 2;
            memset(&sa, 0, sizeof(sa));
            sa.sin_len = sizeof(sa);
            sa.sin_family = AF_INET;
            sa.sin_port = 80;
            memcpy(&sa.sin_addr, h->h_addr_list[0], 4);
            s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) { res->err = Errno(); break; }
            res->stage = 3;
            if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
                res->err = Errno(); CloseSocket(s); break;
            }
            res->stage = 4;
            strcpy(req, "GET ");
            strcat(req, path);
            strcat(req, " HTTP/1.0\r\nHost: ");
            strcat(req, host);
            strcat(req, "\r\nUser-Agent: PrismNet-bsdstress\r\nConnection: close\r\n\r\n");
            if (send(s, (UBYTE *)req, strlen(req), 0) != (LONG)strlen(req)) {
                res->err = Errno(); CloseSocket(s); break;
            }
            res->stage = 5;
            while ((n = recv(s, (UBYTE *)buf, sizeof(buf), 0)) > 0)
                res->bytes += n;
            if (n < 0) { res->err = Errno(); CloseSocket(s); break; }
            CloseSocket(s);
            res->ok_rounds++;
            res->stage = 6;
        }
        CloseLibrary(SocketBase);
    }

    Forbid();
    done_count++;
    Signal(parent, SIGBREAKF_CTRL_F);
    Permit();
}

int main(void)
{
    LONG args[5] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"HOST/K,PATH/K,TASKS/K/N,ROUNDS/K/N,PREOPEN/S", args, NULL);
    int tasks = 8, i, total_ok = 0;
    ULONG total = 0;

    if (args[0]) host = (const char *)args[0];
    if (args[1]) path = (const char *)args[1];
    if (args[2]) tasks = (int)*(LONG *)args[2];
    if (args[3]) rounds = (int)*(LONG *)args[3];
    if (tasks < 1) tasks = 1;
    if (tasks > MAXTASKS) tasks = MAXTASKS;

    parent = FindTask(NULL);
    if (args[4]) {
        SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
        printf("  pre-opened bsdsocket.library: %s\n", SocketBase ? "ok" : "FAILED");
    }
    printf("bsdstress: %d tasks x %d rounds, GET http://%s%s\n", tasks, rounds, host, path);
    for (i = 0; i < tasks; i++) {
        char name[24];
        sprintf(name, "bsdstress.%d", i);
        if (!CreateNewProcTags(NP_Entry, (ULONG)child, NP_StackSize, 16384,
                               NP_Name, (ULONG)name, NP_Output, 0, NP_Input, 0,
                               NP_CloseOutput, FALSE, NP_CloseInput, FALSE, TAG_DONE)) {
            printf("  cannot start task %d\n", i);
            tasks = i;
            break;
        }
    }
    for (i = 0; done_count < tasks && i < 60 * 5; i++) {    /* give up after 60 s */
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
        Delay(10);
    }
    if (done_count < tasks) {
        printf("  TIMEOUT: only %d/%d tasks finished (stage shows where the rest are)\n",
               done_count, tasks);
        /* The children run code in this program's segment: returning now
         * would unload it under them. Stop their sockets and wait. */
        printf("  waiting for the rest (they are aborted through their sockets)...\n");
        Flush(Output());
        while (done_count < tasks)
            Delay(10);
    }
    for (i = 0; i < tasks; i++) {
        printf("  task %d: %ld/%d rounds, %lu bytes, stage %ld, errno %ld\n", i,
               (long)results[i].ok_rounds, rounds, (unsigned long)results[i].bytes,
               (long)results[i].stage, (long)results[i].err);
        total_ok += results[i].ok_rounds;
        total += results[i].bytes;
    }
    printf("bsdstress: %d/%d rounds ok, %lu bytes -> %s\n", total_ok, tasks * rounds,
           (unsigned long)total, total_ok == tasks * rounds ? "PASS" : "FAIL");
    if (SocketBase) CloseLibrary(SocketBase);
    if (rda) FreeArgs(rda);
    return total_ok == tasks * rounds ? RETURN_OK : RETURN_WARN;
}
