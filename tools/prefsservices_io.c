/*
 * PrismNet Prefs, Services tab: reading and writing ENV:PrismNet/services.
 * Included by prismprefs.c after the file helpers. GPL-3.0-only, see LICENSE.
 *
 * Only the ftp and telnet lines are ours to edit; every other line (comments,
 * other services) is kept as it was. A switched-off server stays in the file
 * as "#ftp ..." / "#telnet ...", so its options are still there when it is
 * switched on again.
 */

static void svc_words(char *s, char **w, int *n, int max)
{
    *n = 0;
    while (*s && *n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        w[(*n)++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
}

/* a service line for `name`: "[#]name <port> tcp <program> [args]"? */
static BOOL svc_is(const char *line, const char *name, BOOL *off)
{
    size_t n = strlen(name);
    while (*line == ' ' || *line == '\t') line++;
    *off = *line == '#';
    if (*off) { line++; while (*line == ' ' || *line == '\t') line++; }
    return !strncmp(line, name, n) && (line[n] == ' ' || line[n] == '\t');
}

static void load_services(struct Prefs *p, const char *dir)
{
    char path[160], *t, *line, *nl, *take = NULL, *ttake = NULL;
    BOOL off, take_off = TRUE, ttake_off = TRUE;

    p->ftp_on = p->ftp_anon = p->ftp_ro = FALSE;
    strcpy(p->ftp_port, "21");
    strcpy(p->ftp_prog, "C:ftpd");
    p->ftp_anondir[0] = p->ftp_pasv[0] = p->ftp_extra[0] = p->svc_other[0] = 0;
    p->tel_on = FALSE;
    strcpy(p->tel_port, "23");
    strcpy(p->tel_prog, "C:telnetd");
    p->tel_extra[0] = 0;
    p->have_services = FALSE;
    sprintf(path, "%s/services", dir);
    if (!(t = load_file(path)))
        return;
    p->have_services = TRUE;
    /* the line to edit: the first active ftp line, else the first commented one */
    for (line = t; *line; line = nl) {
        char save;
        nl = strchr(line, '\n');
        if (!nl) nl = line + strlen(line);
        save = *nl; *nl = 0;
        if (svc_is(line, "ftp", &off) && (!take || (take_off && !off))) { take = line; take_off = off; }
        if (svc_is(line, "telnet", &off) && (!ttake || (ttake_off && !off))) { ttake = line; ttake_off = off; }
        *nl = save;
        if (*nl) nl++;
    }
    for (line = t; *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0; else nl = line + strlen(line);
        if (line == take) {
            char *w[24], *s = line;
            int n, i;
            while (*s == ' ' || *s == '\t' || *s == '#') s++;
            svc_words(s, w, &n, 24);
            p->ftp_on = !take_off;
            if (n > 1) copy(p->ftp_port, w[1], sizeof(p->ftp_port));
            if (n > 3) copy(p->ftp_prog, w[3], sizeof(p->ftp_prog));
            for (i = 4; i < n; i++) {
                if (!strncmp(w[i], "ANONYMOUS=", 10)) { p->ftp_anon = TRUE; copy(p->ftp_anondir, w[i] + 10, sizeof(p->ftp_anondir)); }
                else if (ieq(w[i], "READONLY")) p->ftp_ro = TRUE;
                else if (!strncmp(w[i], "PASVPORTS=", 10)) copy(p->ftp_pasv, w[i] + 10, sizeof(p->ftp_pasv));
                else if (strlen(p->ftp_extra) + strlen(w[i]) + 2 < sizeof(p->ftp_extra)) {
                    strcat(p->ftp_extra, " ");          /* PASVADDRESS=, IDLE=: kept as they are */
                    strcat(p->ftp_extra, w[i]);
                }
            }
            continue;
        }
        if (line == ttake) {
            char *w[24], *s = line;
            int n, i;
            while (*s == ' ' || *s == '\t' || *s == '#') s++;
            svc_words(s, w, &n, 24);
            p->tel_on = !ttake_off;
            if (n > 1) copy(p->tel_port, w[1], sizeof(p->tel_port));
            if (n > 3) copy(p->tel_prog, w[3], sizeof(p->tel_prog));
            for (i = 4; i < n; i++)
                if (strlen(p->tel_extra) + strlen(w[i]) + 2 < sizeof(p->tel_extra)) {
                    strcat(p->tel_extra, " ");
                    strcat(p->tel_extra, w[i]);
                }
            continue;
        }
        if (strlen(p->svc_other) + strlen(line) + 2 < sizeof(p->svc_other)) {
            strcat(p->svc_other, line);
            strcat(p->svc_other, "\n");
        }
    }
    FreeVec(t);
}

static BOOL save_services(const struct Prefs *p, const char *dir)
{
    char path[160], *text;
    BOOL ok;

    if (!p->have_services && !p->ftp_on && !p->tel_on)
        return TRUE;                        /* never set up: don't create the file */
    if (!(text = AllocVec(sizeof(p->svc_other) + 400, MEMF_CLEAR)))
        return FALSE;
    strcpy(text, p->svc_other);
    sprintf(text + strlen(text), "%sftp %s tcp %s", p->ftp_on ? "" : "#", p->ftp_port, p->ftp_prog);
    if (p->ftp_anon && p->ftp_anondir[0]) sprintf(text + strlen(text), " ANONYMOUS=%s", p->ftp_anondir);
    if (p->ftp_ro) strcat(text, " READONLY");
    if (p->ftp_pasv[0]) sprintf(text + strlen(text), " PASVPORTS=%s", p->ftp_pasv);
    strcat(text, p->ftp_extra);
    strcat(text, "\n");
    sprintf(text + strlen(text), "%stelnet %s tcp %s%s\n", p->tel_on ? "" : "#", p->tel_port, p->tel_prog,
            p->tel_extra);
    sprintf(path, "%s/services", dir);
    ok = save_file(path, text);
    FreeVec(text);
    return ok;
}

/* NULL, or why the services can't be saved */
static const char *check_services(const struct Prefs *p)
{
    const char *s;
    long port = atol(p->ftp_port), tport = atol(p->tel_port);
    if (p->tel_on && (tport < 1 || tport > 65535))
        return "Services: the telnet port must be 1 to 65535.";
    if (p->tel_on && p->ftp_on && tport == port)
        return "Services: FTP and telnet can't share a port.";
    if (!p->ftp_on)
        return NULL;
    if (port < 1 || port > 65535)
        return "Services: the FTP port must be 1 to 65535.";
    if (!p->ftp_prog[0] || strchr(p->ftp_prog, ' '))
        return "Services: the FTP program path is empty or has a space.";
    if (p->ftp_anon && !p->ftp_anondir[0])
        return "Services: choose the folder anonymous\nFTP users may see, or switch it off.";
    if (strchr(p->ftp_anondir, ' '))
        return "Services: the anonymous folder's path\nmust not contain spaces.";
    for (s = p->ftp_pasv; *s; s++)
        if ((*s < '0' || *s > '9') && *s != '-')
            return "Services: passive ports look like\n50000-50019 (or leave it empty).";
    return NULL;
}
