/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * upd_prismnet.c - where PrismNet keeps the update settings: UPDATES= and
 * MIRROR= lines in ENV:PrismNet/startup (PrismNet Prefs' Startup page writes
 * them; other keys in the file are PrismNet's own).
 *
 *   UPDATES=OFF|GITHUB|MIRROR|BOTH     (default OFF)
 *   MIRROR=https://...                 (a directory with latest.txt, or a
 *                                       Forgejo/Gitea repository's address)
 */
#include <string.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "upd_cfg.h"

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

UBYTE upd_load_prefs(char *mirror, int n)
{
    char line[300];
    UBYTE source = UPD_OFF;
    BPTR f;

    mirror[0] = 0;
    if (!(f = Open((STRPTR)UPD_PREFS_NAME, MODE_OLDFILE)))
        return UPD_OFF;
    while (FGets(f, (STRPTR)line, sizeof(line))) {
        char *v = strchr(line, '='), *e;
        if (line[0] == '#' || line[0] == ';' || !v)
            continue;
        *v++ = 0;
        for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); )
            *--e = 0;
        if (ieq(line, "UPDATES"))
            source = ieq(v, "GITHUB") ? UPD_GITHUB : ieq(v, "MIRROR") ? UPD_MIRROR :
                     ieq(v, "BOTH") ? UPD_BOTH : UPD_OFF;
        else if (ieq(line, "MIRROR")) {
            strncpy(mirror, v, n - 1);
            mirror[n - 1] = 0;
        }
    }
    Close(f);
    return source;
}
