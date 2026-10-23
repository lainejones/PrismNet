/*
 * PrismNet Prefs, Services tab: the page. Included by prismprefs.c after the
 * GUI helpers (sets/gcopy/ask). GPL-3.0-only, see LICENSE.
 */

static void set_check(struct Gui *gui, int id, BOOL on)
{
    if (gui->g[id])
        GT_SetGadgetAttrs(gui->g[id], gui->win, NULL, GTCB_Checked, on, TAG_END);
}

static void show_services(struct Gui *gui)
{
    set_check(gui, G_FTPON, P.ftp_on);
    set_check(gui, G_FTPRO, P.ftp_ro);
    set_check(gui, G_FTPANON, P.ftp_anon);
    set_check(gui, G_TELON, P.tel_on);
    sets(gui, G_TELPORT, P.tel_port);
    sets(gui, G_FTPPORT, P.ftp_port);
    sets(gui, G_FTPDIR, P.ftp_anondir);
    sets(gui, G_FTPPASV, P.ftp_pasv);
    set_check(gui, G_WEBON, P.web_on);
    sets(gui, G_WEBPORT, P.web_port);
    sets(gui, G_WEBDIR, P.web_root);
}

static void store_services(struct Gui *gui)
{
    gcopy(gui, G_FTPPORT, P.ftp_port, sizeof(P.ftp_port));
    gcopy(gui, G_FTPDIR, P.ftp_anondir, sizeof(P.ftp_anondir));
    gcopy(gui, G_FTPPASV, P.ftp_pasv, sizeof(P.ftp_pasv));
    gcopy(gui, G_TELPORT, P.tel_port, sizeof(P.tel_port));
    gcopy(gui, G_WEBPORT, P.web_port, sizeof(P.web_port));
    gcopy(gui, G_WEBDIR, P.web_root, sizeof(P.web_root));
}

/* "..." next to the anonymous folder: an ASL drawer requester */
static void pick_anon_dir(struct Gui *gui)
{
    struct FileRequester *fr;
    if (!AslBase) { DisplayBeep(gui->scr); return; }
    store_services(gui);
    fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (ULONG)"Folder for anonymous FTP",
                             ASLFR_DrawersOnly, TRUE, ASLFR_Window, (ULONG)gui->win,
                             ASLFR_InitialDrawer, (ULONG)(P.ftp_anondir[0] ? P.ftp_anondir : "SYS:"), TAG_END);
    if (!fr) return;
    if (AslRequestTags(fr, TAG_END) && fr->fr_Drawer[0]) {
        copy(P.ftp_anondir, (const char *)fr->fr_Drawer, sizeof(P.ftp_anondir));
        P.ftp_anon = TRUE;
        show_services(gui);
    }
    FreeAslRequest(fr);
}

/* "..." next to the web server's folder */
static void pick_web_dir(struct Gui *gui)
{
    struct FileRequester *fr;
    if (!AslBase) { DisplayBeep(gui->scr); return; }
    store_services(gui);
    fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (ULONG)"Folder the web server shows",
                             ASLFR_DrawersOnly, TRUE, ASLFR_Window, (ULONG)gui->win,
                             ASLFR_InitialDrawer, (ULONG)(P.web_root[0] ? P.web_root : "SYS:"), TAG_END);
    if (!fr) return;
    if (AslRequestTags(fr, TAG_END) && fr->fr_Drawer[0]) {
        copy(P.web_root, (const char *)fr->fr_Drawer, sizeof(P.web_root));
        P.web_on = TRUE;
        show_services(gui);
    }
    FreeAslRequest(fr);
}
