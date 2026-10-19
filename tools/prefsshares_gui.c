/*
 * PrismNet Prefs, Shares tab: the page. Included by prismprefs.c after the
 * GUI helpers (gcopy/sets/dis/ask). GPL-3.0-only, see LICENSE.
 */

static const char *share_type_labels[] = { "NFS", "SMB", NULL };

static void build_shlist(struct Gui *gui)
{
    int i;
    if (gui->g[G_SHLIST])
        GT_SetGadgetAttrs(gui->g[G_SHLIST], gui->win, NULL, GTLV_Labels, ~0UL, TAG_END);
    gui->shlist.lh_Head = (struct Node *)&gui->shlist.lh_Tail;
    gui->shlist.lh_Tail = NULL;
    gui->shlist.lh_TailPred = (struct Node *)&gui->shlist.lh_Head;
    for (i = 0; i < P.nshares; i++) {
        sprintf(gui->shlabel[i], "%-8.8s %s", P.shares[i].name, P.shares[i].smb ? "SMB" : "NFS");
        memset(&gui->shnodes[i], 0, sizeof(struct Node));
        gui->shnodes[i].ln_Name = gui->shlabel[i];
        AddTail(&gui->shlist, &gui->shnodes[i]);
    }
    if (gui->g[G_SHLIST])
        GT_SetGadgetAttrs(gui->g[G_SHLIST], gui->win, NULL, GTLV_Labels, (ULONG)&gui->shlist,
                          GTLV_Selected, gui->shsel >= 0 ? (ULONG)gui->shsel : ~0UL, TAG_END);
}

static void store_share(struct Gui *gui)
{
    struct PrefShare *s;
    if (gui->shsel < 0 || gui->shsel >= P.nshares) return;
    s = &P.shares[gui->shsel];
    gcopy(gui, G_SHNAME, s->name, sizeof(s->name));
    gcopy(gui, G_SHHOST, s->host, sizeof(s->host));
    gcopy(gui, G_SHSHARE, s->share, sizeof(s->share));
    gcopy(gui, G_SHVOL, s->volume, sizeof(s->volume));
    gcopy(gui, G_SHUSER, s->user, sizeof(s->user));
    gcopy(gui, G_SHUID, s->uid, sizeof(s->uid));
    gcopy(gui, G_SHGID, s->gid, sizeof(s->gid));
}

static void show_share(struct Gui *gui)
{
    static const int fields[] = { G_SHNAME, G_SHTYPE, G_SHHOST, G_SHSHARE, G_SHVOL, G_SHBOOT, G_SHRO,
                                  G_SHMOUNT, G_SHUNMOUNT };
    struct PrefShare *s = (gui->shsel >= 0 && gui->shsel < P.nshares) ? &P.shares[gui->shsel] : NULL;
    unsigned i;

    sets(gui, G_SHNAME, s ? s->name : "");
    sets(gui, G_SHHOST, s ? s->host : "");
    sets(gui, G_SHSHARE, s ? s->share : "");
    sets(gui, G_SHVOL, s ? s->volume : "");
    sets(gui, G_SHUSER, s ? s->user : "");
    sets(gui, G_SHUID, s ? s->uid : "");
    sets(gui, G_SHGID, s ? s->gid : "");
    if (gui->g[G_SHTYPE])
        GT_SetGadgetAttrs(gui->g[G_SHTYPE], gui->win, NULL, GTCY_Active, s && s->smb ? 1 : 0, TAG_END);
    if (gui->g[G_SHBOOT])
        GT_SetGadgetAttrs(gui->g[G_SHBOOT], gui->win, NULL, GTCB_Checked, s && s->atboot, TAG_END);
    if (gui->g[G_SHRO])
        GT_SetGadgetAttrs(gui->g[G_SHRO], gui->win, NULL, GTCB_Checked, s && s->readonly, TAG_END);
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        dis(gui, fields[i], s == NULL);
    dis(gui, G_SHUSER, !s || !s->smb);
    dis(gui, G_SHBROWSE, !s || !s->smb);
    dis(gui, G_SHUID, !s || s->smb);
    dis(gui, G_SHGID, !s || s->smb);
    dis(gui, G_SHREMOVE, s == NULL);
    dis(gui, G_SHADD, P.nshares >= MAXSHARES);
}

static void add_share(struct Gui *gui)
{
    struct PrefShare *s;
    int n = 0, i;

    if (P.nshares >= MAXSHARES) return;
    store_share(gui);
    s = &P.shares[P.nshares];
    memset(s, 0, sizeof(*s));
    for (;;) {                          /* first free NETn */
        sprintf(s->name, "NET%d", n);
        for (i = 0; i < P.nshares && !ieq(P.shares[i].name, s->name); i++) ;
        if (i == P.nshares) break;
        n++;
    }
    s->smb = 1;
    s->handler = sh_default_handler(1);
    strcpy(s->stack, "16384");
    strcpy(s->pri, "5");
    gui->shsel = P.nshares++;
    build_shlist(gui);
    show_share(gui);
    if (gui->g[G_SHHOST]) ActivateGadget(gui->g[G_SHHOST], gui->win, NULL);
}

static void remove_share(struct Gui *gui)
{
    int i;
    if (gui->shsel < 0 || gui->shsel >= P.nshares) return;
    for (i = gui->shsel; i < P.nshares - 1; i++) P.shares[i] = P.shares[i + 1];
    P.nshares--;
    if (gui->shsel >= P.nshares) gui->shsel = P.nshares - 1;
    build_shlist(gui);
    show_share(gui);
}

/* run a command with no console, wait for it; its return code */
static LONG run_quiet(const char *cmd, const char *outfile)
{
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)(outfile ? outfile : "NIL:"), MODE_NEWFILE);
    LONG rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, in, SYS_Output, out, TAG_DONE);
    if (in) Close(in);
    if (out) Close(out);
    return rc;
}

#define PICK_MAX 32
#define PICK_LEN 40

/* A window listing every entry to pick one from (scrolls when there are more
 * than fit). Double-click, OK or Return picks; Cancel, Esc or the close
 * gadget gives -1. A requester with a button per entry ran out of width after
 * four or five share names. */
static int pick_from_list(struct Gui *gui, const char *title, char (*names)[PICK_LEN], int n)
{
    struct Screen *scr = gui->scr;
    struct Window *w = NULL;
    struct Gadget *glist = NULL, *g, *lv;
    struct NewGadget ng;
    struct List list;
    struct Node *nodes;
    WORD fh = scr->Font->ta_YSize, gh = fh + 4, ww = 0, wh, lvh, rows, bw, i;
    int sel = 0, pick = -1, lastsel = -1;
    ULONG lastsec = 0, lastmic = 0;
    BOOL done = FALSE;

    if (n <= 0 || !(nodes = AllocVec(n * sizeof(struct Node), MEMF_CLEAR))) return -1;
    list.lh_Head = (struct Node *)&list.lh_Tail; list.lh_Tail = NULL; list.lh_TailPred = (struct Node *)&list.lh_Head;
    for (i = 0; i < n; i++) {
        WORD tw = TextLength(&scr->RastPort, (STRPTR)names[i], strlen(names[i]));
        if (tw > ww) ww = tw;
        nodes[i].ln_Name = names[i];
        AddTail(&list, &nodes[i]);
    }
    bw = TextLength(&scr->RastPort, (STRPTR)"Cancel", 6) + 24;
    ww += 40 + scr->WBorLeft + scr->WBorRight;                  /* room for the scroller */
    if (ww < 2 * bw + 40) ww = 2 * bw + 40;
    if (ww < 220) ww = 220;
    if (ww > scr->Width - 16) ww = scr->Width - 16;
    rows = n < 4 ? 4 : n > 10 ? 10 : n;
    lvh = rows * fh + 4;
    wh = scr->WBorTop + fh + 1 + 6 + lvh + 6 + gh + 6 + scr->WBorBottom;
    w = OpenWindowTags(NULL, WA_Title, (ULONG)title, WA_PubScreen, (ULONG)scr,
        WA_Left, gui->win->LeftEdge + 40, WA_Top, gui->win->TopEdge + 30,
        WA_Width, ww, WA_Height, wh,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | BUTTONIDCMP | LISTVIEWIDCMP,
        TAG_END);
    if (!w) goto out;
    if (!(g = CreateContext(&glist))) goto out;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = gui->vi;
    ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = w->BorderLeft + 8; ng.ng_TopEdge = w->BorderTop + 6;
    ng.ng_Width = w->Width - w->BorderLeft - w->BorderRight - 16; ng.ng_Height = lvh;
    ng.ng_GadgetID = 1;
    g = lv = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&list, GTLV_ShowSelected, 0UL,
                          GTLV_Selected, 0UL, TAG_END);
    ng.ng_TopEdge = w->Height - w->BorderBottom - gh - 6; ng.ng_Width = bw; ng.ng_Height = gh;
    ng.ng_GadgetText = (UBYTE *)"_OK"; ng.ng_Flags = PLACETEXT_IN; ng.ng_GadgetID = 2;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);
    ng.ng_LeftEdge = w->Width - w->BorderRight - 8 - bw;
    ng.ng_GadgetText = (UBYTE *)"_Cancel"; ng.ng_GadgetID = 3;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);
    if (!g) goto out;
    AddGList(w, glist, ~0, -1, NULL);
    RefreshGList(glist, w, NULL, -1);
    GT_RefreshWindow(w, NULL);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(w->UserPort);
        while ((im = GT_GetIMsg(w->UserPort)) != NULL) {
            ULONG cls = im->Class, secs = im->Seconds, mics = im->Micros;
            UWORD code = im->Code;
            struct Gadget *gd = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
            else if (cls == IDCMP_CLOSEWINDOW) done = TRUE;
            else if (cls == IDCMP_VANILLAKEY) {
                if (code == 27 || (code | 0x20) == 'c') done = TRUE;
                else if (code == 13 || (code | 0x20) == 'o') { pick = sel; done = TRUE; }
            } else if (cls == IDCMP_GADGETUP) {
                if (gd->GadgetID == 1) {
                    if ((int)code == lastsel && DoubleClick(lastsec, lastmic, secs, mics)) { pick = code; done = TRUE; }
                    sel = lastsel = code; lastsec = secs; lastmic = mics;
                } else if (gd->GadgetID == 2) { pick = sel; done = TRUE; }
                else if (gd->GadgetID == 3) done = TRUE;
            }
        }
    }
out:
    if (w) CloseWindow(w);
    FreeGadgets(glist);
    FreeVec(nodes);
    return pick;
}

/* Browse...: the server's share list through C:SMBShares, picked from a list
 * window (every disk share; printers and IPC$ are left out) */
static void browse_shares(struct Gui *gui)
{
    struct PrefShare *s;
    char cmd[200], (*names)[PICK_LEN], *text, *p;
    int n = 0, pick;

    store_share(gui);
    if (gui->shsel < 0 || gui->shsel >= P.nshares) return;
    s = &P.shares[gui->shsel];
    if (!s->host[0]) { ask(gui, "Shares", "Set the server first.", "OK"); return; }
    if (!exists("C:SMBShares")) {
        ask(gui, "Shares", "C:SMBShares is not installed\n(PrismNet's SMB client was not chosen at install).", "OK");
        return;
    }
    sprintf(cmd, "C:SMBShares %s%s%s", s->host, s->user[0] ? " USER=" : "", s->user);
    if (run_quiet(cmd, "T:pn-shares") != 0 || !(text = load_file("T:pn-shares"))) {
        ask(gui, "Shares", s->user[0]
            ? "Could not list the shares. The saved password\nis used when there is one: mount a share of\nthis server once to save it."
            : "Could not list the shares as a guest.\nSet a user name, or type the share name.", "OK");
        return;
    }
    if (!(names = AllocVec(PICK_MAX * PICK_LEN, 0))) { FreeVec(text); return; }
    for (p = text; *p && n < PICK_MAX; ) {
        char *eol = strchr(p, '\n'), *q;
        if (eol) *eol = 0;
        for (q = p; *q && *q != ' '; q++) ;
        if (q > p && (int)(q - p) < PICK_LEN) {
            char *kind = q;
            while (*kind == ' ') kind++;
            if (!ieqn(kind, "disk", 4)) {
                memcpy(names[n], p, q - p); names[n][q - p] = 0; n++;
            }
        }
        p = eol ? eol + 1 : p + strlen(p);
    }
    FreeVec(text);
    DeleteFile((CONST_STRPTR)"T:pn-shares");
    if (!n) ask(gui, "Shares", "The server lists no disk shares.", "OK");
    else if ((pick = pick_from_list(gui, "Shares on the server", names, n)) >= 0) {
        copy(s->share, names[pick], sizeof(s->share));
        if (!s->volume[0]) copy(s->volume, names[pick], sizeof(s->volume));
        show_share(gui);
    }
    FreeVec(names);
}

static void mount_share(struct Gui *gui, BOOL mount)
{
    struct PrefShare *s;
    char cmd[200], msg[200];
    const char *err;
    int bad;
    LONG rc;

    store_share(gui);
    if (gui->shsel < 0 || gui->shsel >= P.nshares) return;
    s = &P.shares[gui->shsel];
    if (mount) {
        if ((err = check_shares(&P, &bad)) != NULL) { ask(gui, "Shares", err, "OK"); return; }
        if (!save_shares(&P)) { ask(gui, "Shares", "Could not write the mountlist file.", "OK"); return; }
        P.nloaded = 0;                  /* what is on disk now is the baseline */
        for (bad = 0; bad < P.nshares; bad++) {
            copy(P.sh_loaded[P.nloaded], P.shares[bad].name, 16);
            P.sh_loaded_devs[P.nloaded++] = P.shares[bad].atboot;
        }
        sprintf(cmd, "C:Mount %s/%s", sh_dir(s), s->name);
        rc = run_quiet(cmd, NULL);
        if (rc == 0)
            sprintf(msg, "%s: is mounted. The volume appears on\nthe Workbench at the first access%s.", s->name,
                    s->smb && s->user[0] ? ",\nwhich asks for the password once" : "");
        else
            sprintf(msg, "Mount %s failed (return code %ld).\nIs %s installed?", s->name, (long)rc,
                    sh_handlers[s->handler]);
    } else {
        sprintf(cmd, "C:ShareUnmount %s:", s->name);
        rc = run_quiet(cmd, NULL);
        sprintf(msg, rc == 0 ? "%s: is dismounted." : "ShareUnmount %s: failed (return code %ld).", s->name, (long)rc);
    }
    ask(gui, "Shares", msg, "OK");
}
