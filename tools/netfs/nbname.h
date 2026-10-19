/*
 * PrismNet network shares: NetBIOS name queries (RFC 1002), packets only.
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PNFS_NBNAME_H
#define PNFS_NBNAME_H

#include "nettypes.h"

/* a bare name of 1-15 characters with no dot: worth asking NetBIOS */
int  nb_wanted(const char *host);
/* a name query for name<20> into out (50 bytes); returns the length */
u32  nb_query(u8 *out, const char *name, int bcast, u32 id);
/* the first address in a positive answer to query id, host order; 0 if none */
u32  nb_answer(const u8 *pkt, u32 len, u32 id);

#endif
