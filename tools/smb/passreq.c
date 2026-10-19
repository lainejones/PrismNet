/*
 * PrismNet SMB client: the password window. Opened by the handler process
 * on the default public screen the first time a USER= share is used; the
 * password stays in memory only. Typing shows '*': an edit hook keeps the
 * real characters aside (append and backspace only, so the two cannot get
 * out of step). OS 2.04+. GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>
#include <intuition/screens.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "passreq.h"

struct IntuitionBase *IntuitionBase;

extern ULONG pnfs_hookentry();          /* smb_start.S: a0/a2/a1 -> C arguments */

static char secret[64];
static int  nsecret;

static ULONG edit_hook(struct Hook *h, struct SGWork *sgw, ULONG *msg)
{
    (void)h;
    if (*msg != SGH_KEY)
        return 0;
    switch (sgw->EditOp) {
    case EO_INSERTCHAR:
    case EO_REPLACECHAR:
        if (sgw->BufferPos == sgw->NumChars && nsecret < (int)sizeof(secret) - 1) {
            secret[nsecret++] = sgw->WorkBuffer[sgw->BufferPos - 1];
            sgw->WorkBuffer[sgw->BufferPos - 1] = '*';
        } else {
            sgw->Actions &= ~SGA_USE;
            sgw->Actions |= SGA_BEEP;
        }
        break;
    case EO_DELBACKWARD:
        if (nsecret > 0 && sgw->BufferPos == sgw->NumChars) nsecret--;
        else sgw->Actions &= ~SGA_USE;
        break;
    case EO_CLEAR:
    case EO_RESET:
        nsecret = 0;
        break;
    case EO_ENTER:
    case EO_NOOP:
        break;
    default:                            /* cursor moves, delete-forward: no */
        sgw->Actions &= ~SGA_USE;
        break;
    }
    return ~0UL;
}

int ask_password(const char *title, char *out, unsigned max, int *remember)
{
    static UBYTE buf[64], undo[64], work[64];
    static WORD box[10], cbox[10];
    struct Hook hook;
    struct StringExtend sx;
    struct StringInfo si;
    struct Border border, cborder;
    struct Gadget gad, check;
    struct IntuiText label;
    struct Screen *scr;
    struct Window *win;
    int ok = 0, done = 0;
    WORD fh, w = 280, cb;

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37)))
        return 0;
    if (!(scr = LockPubScreen(NULL))) {
        CloseLibrary((struct Library *)IntuitionBase);
        return 0;
    }
    fh = scr->RastPort.TxHeight;
    nsecret = 0;
    memset(buf, 0, sizeof(buf));
    memset(&hook, 0, sizeof(hook));
    *(APTR *)&hook.h_Entry = (APTR)pnfs_hookentry;      /* field types vary between NDKs */
    *(APTR *)&hook.h_SubEntry = (APTR)edit_hook;
    memset(&sx, 0, sizeof(sx));
    sx.Pens[0] = sx.ActivePens[0] = 1;
    sx.Pens[1] = sx.ActivePens[1] = 0;
    sx.EditHook = &hook;
    sx.WorkBuffer = work;
    memset(&si, 0, sizeof(si));
    si.Buffer = buf;
    si.UndoBuffer = undo;
    si.MaxChars = sizeof(buf) - 1;
    si.Extension = &sx;
    box[0] = -2; box[1] = -2; box[2] = w + 1; box[3] = -2; box[4] = w + 1;
    box[5] = fh + 1; box[6] = -2; box[7] = fh + 1; box[8] = -2; box[9] = -2;
    memset(&border, 0, sizeof(border));
    border.FrontPen = 1;
    border.DrawMode = JAM1;
    border.Count = 5;
    border.XY = box;
    memset(&gad, 0, sizeof(gad));
    gad.LeftEdge = scr->WBorLeft + 10;
    gad.TopEdge = scr->WBorTop + fh + 1 + 8;
    gad.Width = w;
    gad.Height = fh;
    gad.Flags = GFLG_GADGHCOMP | GFLG_STRINGEXTEND;
    gad.Activation = GACT_RELVERIFY;
    gad.GadgetType = GTYP_STRGADGET;
    gad.GadgetRender = &border;
    gad.SpecialInfo = &si;
    gad.NextGadget = &check;

    /* "Remember" checkbox: a toggling boolean gadget drawn as a box that
     * fills when selected, with its label to the right */
    cb = fh + 2;
    cbox[0] = 0; cbox[1] = 0; cbox[2] = cb + 3; cbox[3] = 0; cbox[4] = cb + 3;
    cbox[5] = cb - 1; cbox[6] = 0; cbox[7] = cb - 1; cbox[8] = 0; cbox[9] = 0;
    memset(&cborder, 0, sizeof(cborder));
    cborder.FrontPen = 1;
    cborder.DrawMode = JAM1;
    cborder.Count = 5;
    cborder.XY = cbox;
    memset(&label, 0, sizeof(label));
    label.FrontPen = 1;
    label.DrawMode = JAM1;
    label.LeftEdge = cb + 10;
    label.TopEdge = 1;
    label.IText = (UBYTE *)"Remember this password";
    memset(&check, 0, sizeof(check));
    check.LeftEdge = gad.LeftEdge;
    check.TopEdge = gad.TopEdge + fh + 8;
    check.Width = cb + 4;
    check.Height = cb;
    check.Flags = GFLG_GADGHCOMP | (remember && *remember ? GFLG_SELECTED : 0);
    check.Activation = GACT_TOGGLESELECT | GACT_RELVERIFY;
    check.GadgetType = GTYP_BOOLGADGET;
    check.GadgetRender = &cborder;
    check.GadgetText = &label;

    win = OpenWindowTags(NULL,
                         WA_Title, (ULONG)title,
                         WA_PubScreen, (ULONG)scr,
                         WA_Left, (scr->Width - w - 40) / 2,
                         WA_Top, scr->Height / 3,
                         WA_Width, w + 20 + scr->WBorLeft + scr->WBorRight,
                         WA_Height, scr->WBorTop + fh + 1 + fh + 8 + cb + 18 + scr->WBorBottom,
                         WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW,
                         WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
                         WA_Gadgets, (ULONG)&gad,
                         TAG_END);
    UnlockPubScreen(NULL, scr);
    if (win) {
        ActivateGadget(&gad, win, NULL);
        while (!done) {
            struct IntuiMessage *m;
            WaitPort(win->UserPort);
            while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
                if (m->Class == IDCMP_GADGETUP && m->IAddress == (APTR)&gad) { ok = 1; done = 1; }
                else if (m->Class == IDCMP_CLOSEWINDOW) done = 1;
                ReplyMsg((struct Message *)m);
            }
        }
        CloseWindow(win);
    }
    if (ok) {
        unsigned n = (unsigned)nsecret < max - 1 ? (unsigned)nsecret : max - 1;
        memcpy(out, secret, n);
        out[n] = 0;
        if (remember) *remember = (check.Flags & GFLG_SELECTED) != 0;
    }
    memset(secret, 0, sizeof(secret));
    memset(buf, 0, sizeof(buf));
    memset(work, 0, sizeof(work));
    memset(undo, 0, sizeof(undo));
    nsecret = 0;
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = NULL;
    return ok;
}
