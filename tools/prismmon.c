/*
 * PrismMon - PrismNet network monitor (GadTools, OS 3.1+).
 * GPL-3.0-only, see LICENSE.
 *
 *  +- PrismMon -------------------------------------------------+
 *  | Running - host amiga.home.lan, 2 clients, 5 sockets          |
 *  | [Start] [Stop] [Refresh] [Prefs...]                          |
 *  |  Status \/ Interfaces \/ Connections \/ Memory \/ Log \       |
 *  | +----------------------------------------------------------+ |
 *  | | the selected tab's list                                  | |
 *  | +----------------------------------------------------------+ |
 *  +--------------------------------------------------------------+
 *
 * One list gadget shows the selected tab's lines; a right click over it
 * opens a small menu (copy the selected line or its address, refresh).
 * Talks to the core directly (PNOP_STATUS / SOCKLIST / GETLOG), refreshes
 * once a second from timer.device so it stays live while inactive.
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <devices/clipboard.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "prismnet/pnmsg.h"
#include "prismnet/pnclient.h"
#include "tabs.h"

static const char verstag[] __attribute__((used)) = "$VER: PrismMon 0.6 (18.10.2026) PrismNet";

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;

enum { GAD_STATUS = 1, GAD_START, GAD_STOP, GAD_REFRESH, GAD_PREFS, GAD_LIST };
enum { PG_STATUS, PG_IF, PG_CONN, PG_MEM, PG_LOG, PG_COUNT };
static const char *page_labels[PG_COUNT] = { "Status", "Interfaces", "Connections", "Memory", "Log" };
#define GAD_TAB0 100

#define MENU_ABOUT 1
#define MENU_QUIT  2
#define MAX_SOCKS  64
#define LOG_KEEP   200

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };

#define POLL_TICKS 25            /* 1/2 s: the GUI must stay responsive */

struct Mon {
    int backoff;                /* refreshes to skip after the core timed out */
    struct Screen *scr;
    APTR           vi;
    struct Window *win;
    struct Menu   *menu;
    struct Gadget *glist, *gStatus, *gStart, *gStop, *gList;
    struct Tabs    tabs;
    int            page;
    int            sel[PG_COUNT];   /* selected line per tab, -1 = none */
    struct List    lists[PG_COUNT];
    int            nlog;
    ULONG          logseq;
    char           status[120];
    BOOL           running;
    WORD           lx, ly, lw, lh;  /* the list gadget's box, for right clicks */
    struct PnStatus   *st;
    struct PnSockInfo *si;
    char              *logbuf;
};

/* ---------------------------------------------------------------------- */
/* list helpers                                                             */
/* ---------------------------------------------------------------------- */

static void list_init(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void list_free(struct List *l)
{
    struct Node *n;
    while ((n = RemHead(l)) != NULL)
        FreeVec(n);
}

static void list_add(struct List *l, const char *text)
{
    int len = strlen(text) + 1;
    struct Node *n = AllocVec(sizeof(*n) + len, MEMF_CLEAR);
    if (!n) return;
    n->ln_Name = (char *)(n + 1);
    memcpy(n->ln_Name, text, len);
    AddTail(l, n);
}

static struct Node *list_nth(struct List *l, int i)
{
    struct Node *n;
    for (n = l->lh_Head; n->ln_Succ && i > 0; n = n->ln_Succ, i--)
        ;
    return n->ln_Succ && i == 0 ? n : NULL;
}

static void ipstr(ULONG a, char *out)
{
    sprintf(out, "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24) & 255, (unsigned long)(a >> 16) & 255,
            (unsigned long)(a >> 8) & 255, (unsigned long)a & 255);
}

static void bytes(ULONG v, char *out)
{
    if (v >= 10 * 1024 * 1024) sprintf(out, "%luM", (unsigned long)(v / (1024 * 1024)));
    else if (v >= 10 * 1024)   sprintf(out, "%luK", (unsigned long)(v / 1024));
    else                       sprintf(out, "%lu", (unsigned long)v);
}

static unsigned long kb(ULONG v) { return (unsigned long)((v + 512) / 1024); }

/* ---------------------------------------------------------------------- */
/* refresh from the core                                                    */
/* ---------------------------------------------------------------------- */

static const char *const states[] = { "-", "LISTEN", "SYN_SENT", "ESTABLISHED", "CLOSING", "BOUND" };

/* (Re)attach the current tab's list to the gadget. */
static void show_list(struct Mon *m)
{
    LONG sel = m->sel[m->page];
    if (!m->gList || !m->win) return;
    if (m->page == PG_LOG)
        GT_SetGadgetAttrs(m->gList, m->win, NULL, GTLV_Labels, (ULONG)&m->lists[PG_LOG],
                          GTLV_Selected, ~0UL, GTLV_MakeVisible, m->nlog ? m->nlog - 1 : 0, TAG_END);
    else
        GT_SetGadgetAttrs(m->gList, m->win, NULL, GTLV_Labels, (ULONG)&m->lists[m->page],
                          GTLV_Selected, sel >= 0 ? (ULONG)sel : ~0UL, TAG_END);
}

static void detach(struct Mon *m, int page)
{
    if (m->gList && m->win && m->page == page)
        GT_SetGadgetAttrs(m->gList, m->win, NULL, GTLV_Labels, ~0UL, TAG_END);
}

static void fill_status(struct Mon *m)
{
    struct List *l = &m->lists[PG_STATUS];
    struct PnStatus *st = m->st;
    char line[160], a[20];
    LONG i;

    if (!m->running) {
        list_add(l, m->status);
        list_add(l, "");
        list_add(l, "Start it with the Start button, or PrismNet START in a shell.");
        return;
    }
    strcpy(line, "DNS servers:");
    for (i = 0; i < 3; i++)
        if (st->dns[i]) { ipstr(st->dns[i], a); strcat(line, " "); strcat(line, a); }
    if (!st->dns[0]) strcat(line, " (none)");
    list_add(l, line);
    for (i = 0; i < (LONG)st->nifs; i++) {
        struct PnIfStatus *is = &st->ifs[i];
        ipstr(is->ip, a);
        sprintf(line, "%s: %s, %s, link %s%s", is->name, a, is->dhcp ? "DHCP" : "static",
                is->link_up ? "up" : "down", is->is_default ? ", default route" : "");
        list_add(l, line);
    }
    sprintf(line, "Memory: %lu KB in use, %lu KB free in the machine",
            kb(st->mem.image + st->mem.stack + st->mem.mem_used + st->mem.mem_cached + st->mem.dyn_used),
            kb(st->mem.sys_free));
    list_add(l, line);
}

static void fill_ifaces(struct Mon *m)
{
    struct List *l = &m->lists[PG_IF];
    char line[160], a[20], b[20], c[20], d[12], e[12];
    LONG i;

    if (!m->running) return;
    for (i = 0; i < (LONG)m->st->nifs; i++) {
        struct PnIfStatus *is = &m->st->ifs[i];
        struct PnIfStatusEx *x = &m->st->ifx[i];
        ipstr(is->ip, a); ipstr(is->netmask, b); ipstr(is->gateway, c);
        if (i) list_add(l, "");
        sprintf(line, "%-6s %s unit %lu, %s, link %s%s", is->name, is->device, (unsigned long)is->unit,
                x->p2p ? "point-to-point" : "Ethernet", is->link_up ? "up" : "down",
                is->is_default ? ", default route" : "");
        list_add(l, line);
        sprintf(line, "       %s / %s  gateway %s  %s", a, b, c, is->dhcp ? "DHCP" : "static");
        list_add(l, line);
        if (!x->p2p) {
            sprintf(line, "       hardware %02x:%02x:%02x:%02x:%02x:%02x  mtu %u  %lu bit/s",
                    is->mac[0], is->mac[1], is->mac[2], is->mac[3], is->mac[4], is->mac[5],
                    (unsigned)is->mtu, (unsigned long)x->bps);
        } else {
            sprintf(line, "       mtu %u  %lu bit/s", (unsigned)is->mtu, (unsigned long)x->bps);
        }
        list_add(l, line);
        bytes(is->rx_bytes, d); bytes(is->tx_bytes, e);
        sprintf(line, "       in %lu packets %s bytes, out %lu packets %s bytes", (unsigned long)is->rx_packets, d,
                (unsigned long)is->tx_packets, e);
        list_add(l, line);
        sprintf(line, "       dropped in %lu out %lu, errors in %lu out %lu", (unsigned long)is->rx_dropped,
                (unsigned long)is->tx_dropped, (unsigned long)x->rx_errors, (unsigned long)x->tx_errors);
        list_add(l, line);
    }
}

static void fill_conns(struct Mon *m)
{
    struct List *l = &m->lists[PG_CONN];
    char line[160], a[32], b[32], t[20];
    LONG n, i;

    if (!m->running) return;
    n = pn_core_request(PNOP_SOCKLIST, (ULONG)m->si, MAX_SOCKS, 0, POLL_TICKS);
    if (n == PNC_TIMEOUT) {
        m->si = AllocVec(MAX_SOCKS * sizeof(*m->si), MEMF_PUBLIC | MEMF_CLEAR);
        m->backoff = 10;
        return;
    }
    for (i = 0; i < n; i++) {
        struct PnSockInfo *s = &m->si[i];
        ipstr(s->lip, t);
        sprintf(a, "%s:%u", t, (unsigned)s->lport);
        if (s->rip || s->rport) { ipstr(s->rip, t); sprintf(b, "%s:%u", t, (unsigned)s->rport); }
        else strcpy(b, "*");
        sprintf(line, "%s %-21s %-21s %-11s %s", s->type == 1 ? "tcp" : s->type == 2 ? "udp" : "raw",
                a, b, states[s->state < 6 ? s->state : 0], s->owner);
        list_add(l, line);
    }
    if (!n) list_add(l, "(no open sockets)");
}

static void fill_memory(struct Mon *m)
{
    struct List *l = &m->lists[PG_MEM];
    const struct PnMemStatus *mm = &m->st->mem;
    char line[160];

    if (!m->running) return;
    sprintf(line, "In use        %lu KB", kb(mm->image + mm->stack + mm->mem_used + mm->mem_cached + mm->dyn_used));
    list_add(l, line);
    sprintf(line, "  program     %lu KB (code and data)", kb(mm->image));
    list_add(l, line);
    sprintf(line, "  stack       %lu KB, at most %lu bytes used", kb(mm->stack), (unsigned long)mm->stack_peak);
    list_add(l, line);
    sprintf(line, "  lwIP        %lu KB (peak %lu KB, %lu KB kept for reuse) of %lu KB budget",
            kb(mm->mem_used + mm->mem_cached), kb(mm->mem_peak), kb(mm->mem_cached), kb(mm->budget));
    list_add(l, line);
    sprintf(line, "  sockets     %lu bytes (peak %lu)", (unsigned long)mm->dyn_used, (unsigned long)mm->dyn_peak);
    list_add(l, line);
    list_add(l, "");
    sprintf(line, "Packet buffers %lu of %lu (peak %lu)", (unsigned long)mm->pool_used, (unsigned long)mm->pool_max,
            (unsigned long)mm->pool_peak);
    list_add(l, line);
    sprintf(line, "TCP            %u of %u connections (peak %u), %u of %u segments (peak %u)",
            mm->tcp_used, mm->tcp_max, mm->tcp_peak, mm->seg_used, mm->seg_max, mm->seg_peak);
    list_add(l, line);
    sprintf(line, "UDP            %u of %u sockets", mm->udp_used, mm->udp_max);
    list_add(l, line);
    sprintf(line, "Refused        %lu allocations", (unsigned long)(mm->mem_fails + mm->pool_fails));
    list_add(l, line);
    list_add(l, "");
    sprintf(line, "Machine free   %lu KB (chip %lu KB), largest block %lu KB", kb(mm->sys_free),
            kb(mm->sys_chip), kb(mm->sys_largest));
    list_add(l, line);
}

static void fill_log(struct Mon *m)
{
    BOOL added = FALSE;
    LONG res;

    if (!m->running) return;
    for (;;) {
        char *p;
        ULONG last;
        if (!m->logbuf) break;
        res = pn_core_request(PNOP_GETLOG, m->logseq, (ULONG)m->logbuf, 4096, POLL_TICKS);
        if (res < 0) {
            if (res == PNC_TIMEOUT) {
                m->logbuf = AllocVec(4096, MEMF_PUBLIC | MEMF_CLEAR);
                m->backoff = 10;
            }
            break;
        }
        last = (ULONG)res;
        if (last < m->logseq) m->logseq = 0;    /* core restarted: start over */
        if (!m->logbuf[0] || last == m->logseq) break;
        if (!added) { detach(m, PG_LOG); added = TRUE; }
        for (p = m->logbuf; *p; ) {
            char *tab = strchr(p, '\t'), *eol = strchr(p, '\n');
            if (!eol) break;
            *eol = 0;
            list_add(&m->lists[PG_LOG], tab ? tab + 1 : p);
            if (++m->nlog > LOG_KEEP) { FreeVec(RemHead(&m->lists[PG_LOG])); m->nlog--; }
            p = eol + 1;
        }
        m->logseq = last;
    }
    if (added && m->page == PG_LOG)
        show_list(m);
}

static void refresh(struct Mon *m)
{
    LONG res;
    int pg;

    if (m->backoff > 0) {                   /* the core did not answer: ask again later */
        m->backoff--;
        return;
    }
    if (!m->st || !m->si || !m->logbuf)
        return;                             /* replacement buffer could not be allocated */
    res = pn_core_request(PNOP_STATUS, (ULONG)m->st, sizeof(*m->st), 0, POLL_TICKS);
    if (res == PNC_TIMEOUT) {
        /* the core may still write into the old buffer: leak it, use a new one,
         * and stop polling a wedged core every second (each try leaks a request) */
        m->st = AllocVec(sizeof(*m->st), MEMF_PUBLIC | MEMF_CLEAR);
        strcpy(m->status, "PrismNet is not responding");
        m->running = FALSE;
        m->backoff = 10;
    } else if (res == PNC_NOT_RUNNING) {
        strcpy(m->status, "PrismNet is not running");
        m->running = FALSE;
    } else {
        sprintf(m->status, "Running - host %s%s%s, %lu client%s, %lu socket%s",
                m->st->hostname, m->st->domain[0] ? "." : "", m->st->domain,
                (unsigned long)m->st->clients, m->st->clients == 1 ? "" : "s",
                (unsigned long)m->st->sockets, m->st->sockets == 1 ? "" : "s");
        m->running = TRUE;
    }
    if (!m->st) return;

    /* rebuild every tab but the log (which only grows) */
    detach(m, m->page);
    for (pg = 0; pg < PG_COUNT; pg++)
        if (pg != PG_LOG)
            list_free(&m->lists[pg]);
    fill_status(m);
    fill_ifaces(m);
    fill_conns(m);
    fill_memory(m);
    if (m->page != PG_LOG)
        show_list(m);
    fill_log(m);

    if (m->win) {
        GT_SetGadgetAttrs(m->gStatus, m->win, NULL, GTTX_Text, (ULONG)m->status, TAG_END);
        GT_SetGadgetAttrs(m->gStart, m->win, NULL, GA_Disabled, m->running, TAG_END);
        GT_SetGadgetAttrs(m->gStop, m->win, NULL, GA_Disabled, !m->running, TAG_END);
    }
}

/* ---------------------------------------------------------------------- */
/* actions                                                                  */
/* ---------------------------------------------------------------------- */

static void run_async(const char *cmd)
{
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE), out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        DisplayBeep(NULL);
    }
}

static void stop_core(void)
{
    struct MsgPort *p;
    Forbid();
    if ((p = FindPort((CONST_STRPTR)PN_PORTNAME)) != NULL)
        Signal(p->mp_SigTask, SIGBREAKF_CTRL_C);
    Permit();
}

static void start_prefs(void)
{
    BPTR l = Lock((CONST_STRPTR)"SYS:Prefs/PrismNet", ACCESS_READ);
    if (l) { UnLock(l); run_async("SYS:Prefs/PrismNet"); }
    else run_async("PROGDIR:PrismNetPrefs");
}

/* Put text on the clipboard (unit 0) as IFF FTXT. */
static BOOL clip_text(const char *text)
{
    struct MsgPort *port = CreateMsgPort();
    struct IOClipReq *io = port ? (struct IOClipReq *)CreateIORequest(port, sizeof(*io)) : NULL;
    ULONG len = strlen(text), pad = len & 1, hdr[5];
    BOOL ok = FALSE;

    if (io && OpenDevice((CONST_STRPTR)"clipboard.device", 0, (struct IORequest *)io, 0) == 0) {
        hdr[0] = 0x464F524DUL;                  /* FORM */
        hdr[1] = 12 + len + pad;
        hdr[2] = 0x46545854UL;                  /* FTXT */
        hdr[3] = 0x43485253UL;                  /* CHRS */
        hdr[4] = len;
        io->io_Offset = 0;
        io->io_ClipID = 0;
        io->io_Command = CMD_WRITE;
        io->io_Data = (STRPTR)hdr;
        io->io_Length = sizeof(hdr);
        ok = DoIO((struct IORequest *)io) == 0;
        io->io_Data = (STRPTR)text;
        io->io_Length = len;
        ok &= DoIO((struct IORequest *)io) == 0;
        if (pad) {
            io->io_Data = (STRPTR)"";
            io->io_Length = 1;
            DoIO((struct IORequest *)io);
        }
        io->io_Command = CMD_UPDATE;
        DoIO((struct IORequest *)io);
        CloseDevice((struct IORequest *)io);
    }
    if (io) DeleteIORequest((struct IORequest *)io);
    if (port) DeleteMsgPort(port);
    return ok;
}

/* The last a.b.c.d (without :port) in a line: the remote end of a
 * connection, or an interface's address. */
static BOOL last_address(const char *s, char *out)
{
    const char *p, *best = NULL;
    int dots;
    for (p = s; *p; p++) {
        const char *q = p;
        if (p != s && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9'))) continue;
        for (dots = 0; *q && ((*q >= '0' && *q <= '9') || *q == '.'); q++)
            if (*q == '.') dots++;
        if (dots == 3 && q > p + 6) best = p;
    }
    if (!best) return FALSE;
    for (p = best; *p && ((*p >= '0' && *p <= '9') || *p == '.'); p++)
        *out++ = *p;
    *out = 0;
    return TRUE;
}

/* ---- the right-click menu ------------------------------------------------- */

enum { POP_COPYLINE, POP_COPYADDR, POP_REFRESH, POP_COUNT };
static const char *pop_items[POP_COUNT] = { "Copy line", "Copy address", "Refresh" };

static void pop_draw(struct Window *pw, int hi, BOOL usable[], WORD ih)
{
    struct RastPort *rp = pw->RPort;
    int i;
    SetDrMd(rp, JAM1);
    for (i = 0; i < POP_COUNT; i++) {
        WORD y = 2 + i * ih;
        SetAPen(rp, i == hi && usable[i] ? 3 : 0);
        RectFill(rp, 2, y, pw->Width - 3, y + ih - 1);
        SetAPen(rp, usable[i] ? 1 : 2);
        Move(rp, 8, y + 2 + rp->TxBaseline);
        Text(rp, (STRPTR)pop_items[i], strlen(pop_items[i]));
    }
    SetAPen(rp, 1);                             /* frame */
    Move(rp, 0, 0); Draw(rp, pw->Width - 1, 0); Draw(rp, pw->Width - 1, pw->Height - 1);
    Draw(rp, 0, pw->Height - 1); Draw(rp, 0, 0);
}

/* Open at the pointer; pick with the right button released (or a click)
 * over an item. Returns the item, or -1. */
static int popup(struct Mon *m, BOOL usable[])
{
    struct Window *pw;
    struct TextFont *f;
    WORD ih, w = 0, h, x, y;
    int i, hi = -1, pick = -1;
    BOOL done = FALSE;

    f = OpenFont(&topaz8);
    ih = 8 + 4;
    for (i = 0; i < POP_COUNT; i++)
        if ((WORD)(strlen(pop_items[i]) * 8 + 16) > w) w = strlen(pop_items[i]) * 8 + 16;
    h = POP_COUNT * ih + 4;
    x = m->scr->MouseX - 4;
    y = m->scr->MouseY - 4;
    if (x + w > m->scr->Width) x = m->scr->Width - w;
    if (y + h > m->scr->Height) y = m->scr->Height - h;
    pw = OpenWindowTags(NULL, WA_Left, x, WA_Top, y, WA_Width, w, WA_Height, h,
                        WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                        WA_ReportMouse, TRUE, WA_SmartRefresh, TRUE,
                        WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_INACTIVEWINDOW,
                        WA_PubScreen, (ULONG)m->scr, TAG_END);
    if (!pw) {
        if (f) CloseFont(f);
        return -1;
    }
    if (f) SetFont(pw->RPort, f);
    pop_draw(pw, hi, usable, ih);
    while (!done) {
        struct IntuiMessage *im;
        WaitPort(pw->UserPort);
        while ((im = (struct IntuiMessage *)GetMsg(pw->UserPort)) != NULL) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            WORD mx = im->MouseX, my = im->MouseY;
            ReplyMsg((struct Message *)im);
            i = (mx >= 0 && mx < pw->Width && my >= 2 && my < h - 2) ? (my - 2) / ih : -1;
            if (cls == IDCMP_MOUSEMOVE && i != hi) {
                hi = i;
                pop_draw(pw, hi, usable, ih);
            } else if (cls == IDCMP_MOUSEBUTTONS && (code == MENUUP || code == SELECTUP)) {
                pick = (i >= 0 && usable[i]) ? i : -1;
                if (code == MENUUP && i < 0)
                    ;                           /* released outside: wait for a click */
                else
                    done = TRUE;
            } else if (cls == IDCMP_INACTIVEWINDOW) {
                done = TRUE;
            }
        }
    }
    CloseWindow(pw);
    if (f) CloseFont(f);
    return pick;
}

static void context_menu(struct Mon *m)
{
    BOOL usable[POP_COUNT];
    struct Node *n = m->sel[m->page] >= 0 ? list_nth(&m->lists[m->page], m->sel[m->page]) : NULL;
    char addr[20];

    usable[POP_COPYLINE] = n != NULL && n->ln_Name[0];
    usable[POP_COPYADDR] = n != NULL && last_address(n->ln_Name, addr);
    usable[POP_REFRESH]  = TRUE;
    switch (popup(m, usable)) {
    case POP_COPYLINE: if (!clip_text(n->ln_Name)) DisplayBeep(m->scr); break;
    case POP_COPYADDR: if (!clip_text(addr)) DisplayBeep(m->scr); break;
    case POP_REFRESH:  refresh(m); break;
    }
}

/* Right clicks open our menu only over the list; elsewhere they show the
 * menu strip as usual. Checked on every tick (10/s). */
static void track_rmb(struct Mon *m)
{
    struct Window *w = m->win;
    BOOL over = w->MouseX >= m->lx && w->MouseX < m->lx + m->lw &&
                w->MouseY >= m->ly && w->MouseY < m->ly + m->lh;
    if (over != ((w->Flags & WFLG_RMBTRAP) != 0)) {
        Forbid();
        if (over) w->Flags |= WFLG_RMBTRAP; else w->Flags &= ~WFLG_RMBTRAP;
        Permit();
    }
}

/* ---------------------------------------------------------------------- */
/* window                                                                   */
/* ---------------------------------------------------------------------- */

static BOOL make_gadgets(struct Mon *m)
{
    struct NewGadget ng;
    struct Gadget *g;
    struct Window *w = m->win;
    WORD fh = m->scr->Font ? m->scr->Font->ta_YSize : 8;
    WORD cl = w->BorderLeft + 4, cr = w->Width - w->BorderRight - 4;
    WORD cb = w->Height - w->BorderBottom - 4;
    WORD gh = fh + 4, row = fh + 6, y = w->BorderTop + 3, bw;

    g = CreateContext(&m->glist);
    if (!g) return FALSE;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = m->vi;
    ng.ng_TextAttr   = m->scr->Font;

    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y; ng.ng_Width = cr - cl; ng.ng_Height = gh;
    ng.ng_GadgetID = GAD_STATUS;
    g = m->gStatus = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)m->status, GTTX_Border, TRUE, TAG_END);

    y += row;
    bw = (cr - cl - 3 * 6) / 4;
    ng.ng_TopEdge = y; ng.ng_Width = bw; ng.ng_Height = gh; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_LeftEdge = cl;                 ng.ng_GadgetText = (UBYTE *)"_Start";    ng.ng_GadgetID = GAD_START;
    g = m->gStart = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);
    ng.ng_LeftEdge = cl + (bw + 6);      ng.ng_GadgetText = (UBYTE *)"S_top";     ng.ng_GadgetID = GAD_STOP;
    g = m->gStop = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);
    ng.ng_LeftEdge = cl + 2 * (bw + 6);  ng.ng_GadgetText = (UBYTE *)"_Refresh";  ng.ng_GadgetID = GAD_REFRESH;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);
    ng.ng_LeftEdge = cl + 3 * (bw + 6);  ng.ng_GadgetText = (UBYTE *)"_Prefs..."; ng.ng_GadgetID = GAD_PREFS;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);

    /* the tab row, then the list filling the rest */
    y = m->tabs.y + m->tabs.h + 2;              /* tabs_init() placed the tab row */
    m->lx = cl; m->ly = y; m->lw = cr - cl; m->lh = cb - y;
    if (m->lh < 4 * 9) m->lh = 4 * 9;
    ng.ng_LeftEdge = cl; ng.ng_TopEdge = y; ng.ng_Width = cr - cl; ng.ng_Height = m->lh;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0;
    ng.ng_GadgetID = GAD_LIST; ng.ng_TextAttr = &topaz8;   /* fixed width: columns line up */
    g = m->gList = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&m->lists[m->page],
                                GTLV_ShowSelected, 0UL, TAG_END);
    return g != NULL;
}

static void relayout(struct Mon *m)
{
    struct Window *w = m->win;
    WORD fh = m->scr->Font ? m->scr->Font->ta_YSize : 8;

    if (m->glist) {
        detach(m, m->page);
        RemoveGList(w, m->glist, -1);
        FreeGadgets(m->glist);
        m->glist = NULL;
    }
    tabs_remove(&m->tabs, w);
    EraseRect(w->RPort, w->BorderLeft, w->BorderTop,
              w->Width - w->BorderRight - 1, w->Height - w->BorderBottom - 1);
    m->gStatus = m->gStart = m->gStop = m->gList = NULL;
    SetFont(w->RPort, m->scr->RastPort.Font);
    tabs_init(&m->tabs, w->RPort, page_labels, PG_COUNT, GAD_TAB0, w->BorderLeft + 4,
              w->BorderTop + 3 + 2 * (fh + 6) + 2, w->Width - w->BorderLeft - w->BorderRight - 8);
    tabs_add(&m->tabs, w);
    if (make_gadgets(m)) {
        AddGList(w, m->glist, ~0, -1, NULL);
        RefreshGList(m->glist, w, NULL, -1);
        GT_RefreshWindow(w, NULL);
        tabs_draw(&m->tabs, w);
        show_list(m);
    } else if (m->glist) {
        FreeGadgets(m->glist);
        m->glist = NULL;
    }
}

static struct NewMenu newmenu[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"About...", (STRPTR)"?", 0, 0, (APTR)MENU_ABOUT },
    { NM_ITEM,  NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
    { NM_END,   NULL, NULL, 0, 0, NULL }
};

static void about(struct Mon *m)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)"About PrismMon",
        (STRPTR)"PrismMon 0.6\nPrismNet network monitor\n\nOpen source (GPL-3) - lwIP inside", (STRPTR)"OK" };
    EasyRequestArgs(m->win, &es, NULL, NULL);
}

static void set_page(struct Mon *m, int pg)
{
    if (pg == m->page) return;
    detach(m, m->page);
    m->page = pg;
    m->tabs.active = pg;
    tabs_draw(&m->tabs, m->win);
    show_list(m);
}

int main(void)
{
    struct Mon m;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL quit = FALSE, timer_ok = FALSE;
    WORD w, h;
    int i;
    char v[8];

    (void)verstag;
    memset(&m, 0, sizeof(m));
    for (i = 0; i < PG_COUNT; i++) { list_init(&m.lists[i]); m.sel[i] = -1; }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    GadToolsBase  = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
    m.st     = AllocVec(sizeof(*m.st), MEMF_PUBLIC | MEMF_CLEAR);
    m.si     = AllocVec(MAX_SOCKS * sizeof(*m.si), MEMF_PUBLIC | MEMF_CLEAR);
    m.logbuf = AllocVec(4096, MEMF_PUBLIC | MEMF_CLEAR);
    if (!IntuitionBase || !GfxBase || !GadToolsBase || !m.st || !m.si || !m.logbuf)
        goto out;
    if (!(m.scr = LockPubScreen(NULL)) || !(m.vi = GetVisualInfo(m.scr, TAG_END)))
        goto out;
    /* PrismNet/monpage: open on that tab, 0-4 (testing) */
    if (GetVar((CONST_STRPTR)"PrismNet/monpage", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0 &&
        v[0] >= '0' && v[0] < '0' + PG_COUNT)
        m.page = m.tabs.active = v[0] - '0';

    strcpy(m.status, "...");
    w = 600; if (w > m.scr->Width) w = m.scr->Width;
    h = 280;
    if (h > m.scr->Height - m.scr->BarHeight - 2) h = m.scr->Height - m.scr->BarHeight - 2;
    m.win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"PrismMon", WA_ScreenTitle, (ULONG)"PrismMon - PrismNet network monitor",
        WA_Left, (m.scr->Width - w) / 2, WA_Top, m.scr->BarHeight + 2, WA_Width, w, WA_Height, h,
        WA_MinWidth, 400, WA_MinHeight, 160, WA_MaxWidth, ~0, WA_MaxHeight, ~0,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_SIZEGADGET |
                  WFLG_SIZEBBOTTOM | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE | IDCMP_MENUPICK |
                  IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_INTUITICKS | BUTTONIDCMP | LISTVIEWIDCMP,
        WA_PubScreen, (ULONG)m.scr, TAG_END);
    if (!m.win)
        goto out;
    relayout(&m);
    if ((m.menu = CreateMenus(newmenu, TAG_END)) != NULL) {
        if (LayoutMenus(m.menu, m.vi, TAG_END)) SetMenuStrip(m.win, m.menu);
        else { FreeMenus(m.menu); m.menu = NULL; }
    }

    if ((tport = CreateMsgPort()) && (treq = (struct timerequest *)CreateIORequest(tport, sizeof(*treq))) &&
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) == 0) {
        timer_ok = TRUE;
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs = 0; treq->tr_time.tv_micro = 10000;
        SendIO((struct IORequest *)treq);
    }

    refresh(&m);
    while (!quit) {
        ULONG sigs = Wait((1UL << m.win->UserPort->mp_SigBit) | (timer_ok ? 1UL << tport->mp_SigBit : 0) |
                          SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;

        if (sigs & SIGBREAKF_CTRL_C)
            quit = TRUE;
        if (timer_ok && CheckIO((struct IORequest *)treq)) {
            WaitIO((struct IORequest *)treq);
            refresh(&m);
            treq->tr_time.tv_secs = 1; treq->tr_time.tv_micro = 0;
            SendIO((struct IORequest *)treq);
        }
        while ((im = GT_GetIMsg(m.win->UserPort)) != NULL) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            switch (cls) {
            case IDCMP_CLOSEWINDOW: quit = TRUE; break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(m.win);
                tabs_draw(&m.tabs, m.win);
                GT_EndRefresh(m.win, TRUE);
                break;
            case IDCMP_NEWSIZE: relayout(&m); refresh(&m); break;
            case IDCMP_INTUITICKS: track_rmb(&m); break;
            case IDCMP_MOUSEBUTTONS:
                if (code == MENUDOWN && (m.win->Flags & WFLG_RMBTRAP))
                    context_menu(&m);
                break;
            case IDCMP_VANILLAKEY:
                switch (code | 0x20) {
                case 's': if (!m.running) run_async("C:PrismNet START"); break;
                case 't': if (m.running) stop_core(); break;
                case 'r': refresh(&m); break;
                case 'p': start_prefs(); break;
                case 'q': quit = TRUE; break;
                }
                if (code >= '1' && code < '1' + PG_COUNT) set_page(&m, code - '1');
                if (code == 27) quit = TRUE;
                break;
            case IDCMP_GADGETUP:
                if (tabs_hit(&m.tabs, gad->GadgetID) >= 0) {
                    set_page(&m, tabs_hit(&m.tabs, gad->GadgetID));
                    break;
                }
                switch (gad->GadgetID) {
                case GAD_START:   run_async("C:PrismNet START"); break;
                case GAD_STOP:    stop_core(); break;
                case GAD_REFRESH: refresh(&m); break;
                case GAD_PREFS:   start_prefs(); break;
                case GAD_LIST:    m.sel[m.page] = code; break;
                }
                break;
            case IDCMP_MENUPICK:
                while (code != MENUNULL) {
                    struct MenuItem *it = ItemAddress(m.menu, code);
                    if (!it) break;
                    if ((ULONG)GTMENUITEM_USERDATA(it) == MENU_QUIT) quit = TRUE;
                    if ((ULONG)GTMENUITEM_USERDATA(it) == MENU_ABOUT) about(&m);
                    code = it->NextSelect;
                }
                break;
            }
        }
    }

out:
    if (timer_ok) {
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
        CloseDevice((struct IORequest *)treq);
    }
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (m.win) {
        if (m.menu) { ClearMenuStrip(m.win); FreeMenus(m.menu); }
        if (m.glist) RemoveGList(m.win, m.glist, -1);
        tabs_remove(&m.tabs, m.win);
        CloseWindow(m.win);
    }
    if (m.glist) FreeGadgets(m.glist);
    if (m.vi) FreeVisualInfo(m.vi);
    if (m.scr) UnlockPubScreen(NULL, m.scr);
    for (i = 0; i < PG_COUNT; i++) list_free(&m.lists[i]);
    FreeVec(m.st); FreeVec(m.si); FreeVec(m.logbuf);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
