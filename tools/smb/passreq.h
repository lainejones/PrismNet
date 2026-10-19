/*
 * PrismNet SMB client: ask for a password in a small window (memory only).
 * Returns 1 when one was entered, 0 on Close. GPL-3.0-only, see LICENSE.
 */
#ifndef SMB_PASSREQ_H
#define SMB_PASSREQ_H

/* *remember is the checkbox: its initial state in, the user's choice out */
int ask_password(const char *title, char *out, unsigned max, int *remember);

#endif
