/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * upd_cfg.h - PrismNet's settings for the shared update tool (update.c).
 * Another product copies update.c and gives its own copy of this file and of
 * upd_load_prefs(); PrismRTG's tools/update/update.c is the same file, so
 * change it in both.
 */
#ifndef UPD_CFG_H
#define UPD_CFG_H

#include <exec/types.h>

#define UPD_TOOL        "PrismNetUpdate"            /* the command, and its messages */
#define UPD_PRODUCT     "PrismNet"                  /* T:<product>-update.lha, APPNAME */
#define UPD_REPO        "lainejones/PrismNet"       /* GitHub owner/repository */
#define UPD_ASSET       "PrismNet-"                 /* the release archive: <asset><version>.lha */
#define UPD_INSTALLER   "Install_PrismNet"          /* the Installer script in the archive */
#ifndef UPD_VERSION                                 /* a test build may claim another */
#define UPD_VERSION     "1.0.7"                     /* this release (dist.sh checks it) */
#endif
#define UPD_RELDATE     "20.10.2026"
#define UPD_PREFS_NAME  "ENV:PrismNet/startup"      /* named in messages */
#define UPD_MIRROR_MAX  200

/* UPDATES=: off, GitHub's releases, a mirror, GitHub then the mirror */
enum { UPD_OFF, UPD_GITHUB, UPD_MIRROR, UPD_BOTH, UPD_COUNT };

/* the product's settings: returns UPDATES= (UPD_*), fills MIRROR= */
UBYTE upd_load_prefs(char *mirror, int n);

#endif
