/*
 * PrismNet: lwIP's memory, allocated at run time within a budget (pnmem.c).
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_PNMEM_H
#define PRISMNET_PNMEM_H

#include <exec/types.h>

/* Defaults per build profile; ENV:PrismNet/startup MEMORY=<KB> overrides
 * the budget. The budget covers lwIP's packet buffers, connections and
 * queues (not the core's code, stack or socket bookkeeping). */
#ifdef PN_SMALL
#define PN_MEM_BUDGET_DEFAULT   (96UL * 1024)
#define PN_MEM_CACHE_DEFAULT    6
#else
#define PN_MEM_BUDGET_DEFAULT   (384UL * 1024)
#define PN_MEM_CACHE_DEFAULT    32
#endif

struct PnMemInfo {
    ULONG budget;       /* most lwIP may hold */
    ULONG held;         /* taken from exec: live blocks + cache */
    ULONG cached;       /* ... of which freed blocks kept for reuse */
    ULONG peak;         /* most live bytes ever */
    ULONG fails;        /* allocations refused (budget or system memory) */
};
extern struct PnMemInfo pn_mem;

void  pn_mem_init(ULONG budget_bytes);     /* 0 = profile default */
void *pn_mem_malloc(ULONG n);
void *pn_mem_calloc(ULONG count, ULONG n);
void  pn_mem_free(void *p);
void  pn_mem_trim(void);
void  pn_mem_cleanup(void);

#endif
