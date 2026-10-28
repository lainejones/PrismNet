/*
 * PrismPrint - settings and a test for prismprint.device, the network
 * printer port. Part of PrismNet. GPL-3.0-only, see LICENSE.
 *
 *   PrismPrint                                 the settings of unit 0
 *   PrismPrint HOST=brother PROTOCOL=RAW SAVE  change and keep them
 *   PrismPrint TEST                            a text test page to the printer
 *   PrismPrint FILE=T:job.ps                   a file to the printer as it is
 *   PrismPrint PREFS [SAVE]                    printer.device's port: prismprint
 *   ... UNIT=n                                 printer 0-3
 *
 * Without SAVE a change lasts until the next reboot (ENV: only). FILE and
 * TEST go through prismprint.device itself, the way printer.device does.
 * PREFS sets the port in Sys/printer.prefs (OS 3.2's printer.device: the
 * name without ".device", which it adds itself; 16 characters at most).
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <devices/parallel.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char ver[] __attribute__((used)) = "$VER: PrismPrint 1.0 (28.10.2026) PrismNet";

#define TEMPLATE "HOST/K,PORT/K/N,PROTOCOL/K,QUEUE/K,IDLE/K/N,TIMEOUT/K/N,UNIT/K/N,SAVE/S,FILE/K,TEST/S,PREFS/S"
enum { A_HOST, A_PORT, A_PROTOCOL, A_QUEUE, A_IDLE, A_TIMEOUT, A_UNIT, A_SAVE, A_FILE, A_TEST, A_PREFS, A_N };

struct Cfg { char host[64], protocol[8], queue[32]; LONG port, idle, timeout; };

static void cfg_name(char *buf, const char *vol, LONG unit)
{
    if (unit) sprintf(buf, "%sPrismNet/printer%ld", vol, (long)unit);
    else sprintf(buf, "%sPrismNet/printer", vol);
}

static void cfg_line(struct Cfg *c, char *l)
{
    char *v = strchr(l, '=');
    if (!v) return;
    *v++ = 0;
    if (!stricmp(l, "HOST")) { strncpy(c->host, v, sizeof(c->host) - 1); }
    else if (!stricmp(l, "PROTOCOL")) { strncpy(c->protocol, v, sizeof(c->protocol) - 1); }
    else if (!stricmp(l, "QUEUE")) { strncpy(c->queue, v, sizeof(c->queue) - 1); }
    else if (!stricmp(l, "PORT")) c->port = atol(v);
    else if (!stricmp(l, "IDLE")) c->idle = atol(v);
    else if (!stricmp(l, "TIMEOUT")) c->timeout = atol(v);
}

static BOOL cfg_read(struct Cfg *c, LONG unit)
{
    char name[48], line[128];
    BPTR fh;
    cfg_name(name, "ENV:", unit);
    if (!(fh = Open((STRPTR)name, MODE_OLDFILE))) {
        cfg_name(name, "ENVARC:", unit);
        if (!(fh = Open((STRPTR)name, MODE_OLDFILE))) return FALSE;
    }
    while (FGets(fh, (STRPTR)line, sizeof(line))) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        cfg_line(c, line);
    }
    Close(fh);
    return TRUE;
}

static BOOL cfg_write(const struct Cfg *c, const char *vol, LONG unit)
{
    char name[48], dir[40], text[256];
    BPTR fh, lock;
    sprintf(dir, "%sPrismNet", vol);
    if (!(lock = Lock((STRPTR)dir, ACCESS_READ))) lock = CreateDir((STRPTR)dir);
    if (lock) UnLock(lock);
    cfg_name(name, vol, unit);
    if (!(fh = Open((STRPTR)name, MODE_NEWFILE))) return FALSE;
    sprintf(text, "HOST=%s\nPORT=%ld\nPROTOCOL=%s\nQUEUE=%s\nIDLE=%ld\nTIMEOUT=%ld\n",
            c->host, (long)c->port, c->protocol, c->queue, (long)c->idle, (long)c->timeout);
    Write(fh, text, strlen(text));
    Close(fh);
    return TRUE;
}

/* data to the printer through prismprint.device (as printer.device would) */
static LONG send_data(LONG unit, BPTR fh, const char *text)
{
    struct MsgPort *mp = CreateMsgPort();
    struct IOExtPar *io = mp ? (struct IOExtPar *)CreateIORequest(mp, sizeof(struct IOExtPar)) : NULL;
    UBYTE *buf = AllocVec(4096, MEMF_ANY);
    LONG total = 0, n, err = 0;

    if (!io || !buf || OpenDevice((STRPTR)"prismprint.device", unit, (struct IORequest *)io, 0)) {
        printf("PrismPrint: cannot open prismprint.device unit %ld\n", (long)unit);
        total = -1;
        goto out;
    }
    for (;;) {
        if (text) { n = strlen(text); memcpy(buf, text, n); text = NULL; }
        else if (fh) n = Read(fh, buf, 4096);
        else n = 0;
        if (n <= 0) break;
        io->IOPar.io_Command = CMD_WRITE;
        io->IOPar.io_Data = buf;
        io->IOPar.io_Length = n;
        if ((err = DoIO((struct IORequest *)io))) break;
        total += n;
        if (!fh) break;
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) { err = -1; break; }
    }
    CloseDevice((struct IORequest *)io);     /* ends the job: the printer gets it now */
    if (err > 0) {
        printf("PrismPrint: the printer did not take the data (error %ld) - is HOST right and the printer on?\n", (long)err);
        total = -1;
    } else if (err < 0) total = -1;
out:
    if (buf) FreeVec(buf);
    if (io) DeleteIORequest((struct IORequest *)io);
    if (mp) DeleteMsgPort(mp);
    return total;
}

/* Sys/printer.prefs: the port (PUNT chunk) becomes prismprint, unit n; the
 * rest of the file (printer driver, paper) stays as Printer prefs saved it.
 * No file yet: a header and the port only. */
static BOOL prefs_port(const char *vol, LONG unit)
{
    static UBYTE buf[2048];
    static const UBYTE head[] = { 'F','O','R','M', 0,0,0,4, 'P','R','E','F',
                                  'P','R','H','D', 0,0,0,6, 0,0,0,0,0,0 };
    char name[40];
    LONG len = 0, i;
    BPTR fh;
    UBYTE *punt = NULL;

    sprintf(name, "%sSys/printer.prefs", vol);
    if ((fh = Open((STRPTR)name, MODE_OLDFILE))) {
        len = Read(fh, buf, sizeof(buf) - 64);
        Close(fh);
    }
    if (len < 12 || memcmp(buf, "FORM", 4) || memcmp(buf + 8, "PREF", 4)) {
        memcpy(buf, head, sizeof(head));
        len = sizeof(head);
    }
    for (i = 12; i + 8 <= len; ) {
        LONG n = (buf[i + 4] << 24) | (buf[i + 5] << 16) | (buf[i + 6] << 8) | buf[i + 7];
        if (!memcmp(buf + i, "PUNT", 4) && n >= 56) { punt = buf + i + 8; break; }
        i += 8 + n + (n & 1);
    }
    if (!punt) {                       /* append one */
        memcpy(buf + len, "PUNT", 4);
        buf[len + 4] = 0; buf[len + 5] = 0; buf[len + 6] = 0; buf[len + 7] = 56;
        punt = buf + len + 8;
        memset(punt, 0, 56);
        len += 64;
    }
    punt[16] = unit >> 24; punt[17] = unit >> 16; punt[18] = unit >> 8; punt[19] = unit;
    memset(punt + 20, 0, 36);          /* open flags 0, the name */
    strcpy((char *)punt + 24, "prismprint");
    i = len - 8;
    buf[4] = i >> 24; buf[5] = i >> 16; buf[6] = i >> 8; buf[7] = i;
    if (!(fh = Open((STRPTR)name, MODE_NEWFILE))) return FALSE;
    i = Write(fh, buf, len);
    Close(fh);
    return i == len;
}

static const char testpage[] =
    "PrismNet network printer test\r\n"
    "=============================\r\n\r\n"
    "This page came from the Amiga through prismprint.device.\r\n"
    "If you can read it, the network path to the printer works.\r\n\r\n"
    "Next: Prefs/Printer - set the device to prismprint.device and pick\r\n"
    "a printer driver your printer understands (PostScript, or an HP\r\n"
    "LaserJet / PCL driver).\r\n\f";

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    struct Cfg c;
    LONG unit = 0, n;
    BOOL change = FALSE;

    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"PrismPrint"); return RETURN_FAIL; }
    if (args[A_UNIT]) unit = *(LONG *)args[A_UNIT];
    if (unit < 0 || unit > 3) { printf("PrismPrint: UNIT is 0-3\n"); FreeArgs(rda); return RETURN_ERROR; }
    memset(&c, 0, sizeof(c));
    strcpy(c.protocol, "RAW");
    strcpy(c.queue, "lp");
    c.port = 9100; c.idle = 10; c.timeout = 60;
    cfg_read(&c, unit);
    if (args[A_HOST]) { strncpy(c.host, (char *)args[A_HOST], sizeof(c.host) - 1); change = TRUE; }
    if (args[A_PROTOCOL]) {
        BOOL lpd = toupper(*(char *)args[A_PROTOCOL]) == 'L';
        if (lpd && c.port == 9100) c.port = 515;
        if (!lpd && c.port == 515) c.port = 9100;
        strcpy(c.protocol, lpd ? "LPD" : "RAW");
        change = TRUE;
    }
    if (args[A_PORT]) { c.port = *(LONG *)args[A_PORT]; change = TRUE; }
    if (args[A_QUEUE]) { strncpy(c.queue, (char *)args[A_QUEUE], sizeof(c.queue) - 1); change = TRUE; }
    if (args[A_IDLE]) { c.idle = *(LONG *)args[A_IDLE]; change = TRUE; }
    if (args[A_TIMEOUT]) { c.timeout = *(LONG *)args[A_TIMEOUT]; change = TRUE; }

    if (change || args[A_SAVE]) {
        if (!cfg_write(&c, "ENV:", unit) || (args[A_SAVE] && !cfg_write(&c, "ENVARC:", unit))) {
            printf("PrismPrint: cannot write the settings\n");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }
    printf("Printer %ld: %s, %s port %ld%s%s%s\n", (long)unit, c.host[0] ? c.host : "(no HOST set)",
           c.protocol, (long)c.port, stricmp(c.protocol, "LPD") ? "" : ", queue ",
           stricmp(c.protocol, "LPD") ? "" : c.queue, args[A_SAVE] ? " (saved)" : change ? " (until reboot)" : "");

    if (args[A_PREFS]) {
        if (!prefs_port("ENV:", unit) || (args[A_SAVE] && !prefs_port("ENVARC:", unit))) {
            printf("PrismPrint: cannot write Sys/printer.prefs\n");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        /* printer.device takes its port from the prefs IPrefs hands it: the
         * Printer editor's USE does that now (else at the next boot) */
        {
            BPTR l = Lock((STRPTR)"SYS:Prefs/Printer", ACCESS_READ);
            if (l) {
                UnLock(l);
                SystemTags((STRPTR)"SYS:Prefs/Printer FROM ENV:Sys/printer.prefs USE", TAG_DONE);
            }
        }
        printf("printer.device now prints through prismprint.device unit %ld%s.\n"
               "Pick the printer driver in Prefs/Printer: PostScript, or HP LaserJet for PCL.\n",
               (long)unit, args[A_SAVE] ? " (saved)" : "");
    }
    if (args[A_TEST]) {
        n = send_data(unit, 0, testpage);
        if (n >= 0) printf("Test page sent (%ld bytes).\n", (long)n);
        FreeArgs(rda);
        return n >= 0 ? RETURN_OK : RETURN_ERROR;
    }
    if (args[A_FILE]) {
        BPTR fh = Open((STRPTR)args[A_FILE], MODE_OLDFILE);
        if (!fh) { PrintFault(IoErr(), (CONST_STRPTR)args[A_FILE]); FreeArgs(rda); return RETURN_ERROR; }
        n = send_data(unit, fh, NULL);
        Close(fh);
        if (n >= 0) printf("%ld bytes sent.\n", (long)n);
        FreeArgs(rda);
        return n >= 0 ? RETURN_OK : RETURN_ERROR;
    }
    FreeArgs(rda);
    return RETURN_OK;
}
