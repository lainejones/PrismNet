/*
 * PrismNet NFS client: file names between the Amiga (Latin-1, case-
 * insensitive, max 107 bytes) and the server (UTF-8, case-sensitive).
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PNFS_NAMES_H
#define PNFS_NAMES_H

#include "nettypes.h"

#define AMIGA_NAMEMAX 107

/* server -> Amiga. Characters with no Latin-1 form become '?'. raw: copy
 * bytes as they are. Returns the length written (always terminated). */
u32  name_to_amiga(const char *src, u32 len, char *dst, u32 max, int raw);
/* Amiga -> server. Returns the length written, or 0 if it does not fit. */
u32  name_to_server(const char *src, char *dst, u32 max, int raw);
/* Latin-1 case-insensitive compare (like Stricmp in the international sense) */
int  name_equal(const char *a, const char *b);

#endif
