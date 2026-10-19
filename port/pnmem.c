/*
 * PrismNet: lwIP's memory, allocated at run time within a budget.
 *
 * lwIP normally reserves its heap and every pool as static arrays: 290 KB of
 * BSS in the default build, needed in one piece before the core can even
 * start. Instead lwIP's mem_malloc (and with MEMP_MEM_MALLOC every pool
 * element) comes from here: AllocMem() on demand, freed blocks kept on a
 * free list per block size so packet buffers are reused without a trip to
 * exec, and a byte budget (ENV:PrismNet/startup MEMORY=<KB>) that the cache
 * and the live blocks together never exceed. lwIP's per-pool limits still
 * apply (patched memp.c), so behaviour under load is unchanged; only the
 * memory is no longer reserved up front.
 *
 * Used by the core task only, so no locking. GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "prismnet/pnmem.h"

#define HDR        8            /* size word + spare; keeps blocks 8-aligned */
#define NCLASS     24           /* distinct block sizes kept on free lists */

struct fblock { struct fblock *next; };
struct sclass {
    ULONG          size;        /* block size including HDR; 0 = slot unused */
    struct fblock *head;
    UWORD          count;
};

static struct sclass cls[NCLASS];
static ULONG budget = PN_MEM_BUDGET_DEFAULT;
static ULONG cache_max = PN_MEM_CACHE_DEFAULT;  /* blocks per size on a free list; */
                                                /* all of them together at most budget/4 */
struct PnMemInfo pn_mem;

void pn_mem_init(ULONG budget_bytes)
{
    if (budget_bytes)
        budget = budget_bytes;
    pn_mem.budget = budget;
}

static struct sclass *find_class(ULONG size, BOOL create)
{
    int i, free_slot = -1;
    for (i = 0; i < NCLASS; i++) {
        if (cls[i].size == size)
            return &cls[i];
        if (!cls[i].size && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return NULL;
    cls[free_slot].size = size;
    return &cls[free_slot];
}

/* Give every cached block back to exec (low budget or low system memory). */
void pn_mem_trim(void)
{
    int i;
    for (i = 0; i < NCLASS; i++) {
        struct fblock *b;
        while ((b = cls[i].head) != NULL) {
            cls[i].head = b->next;
            cls[i].count--;
            pn_mem.held -= cls[i].size;
            pn_mem.cached -= cls[i].size;
            FreeMem(b, cls[i].size);
        }
    }
}

void *pn_mem_malloc(ULONG n)
{
    ULONG size = (n + HDR + 7) & ~7UL;
    struct sclass *c = find_class(size, FALSE);
    ULONG *p;

    if (c && c->head) {                         /* reuse a freed block */
        p = (ULONG *)c->head;
        c->head = c->head->next;
        c->count--;
        pn_mem.cached -= size;
    } else {
        if (pn_mem.held + size > budget)
            pn_mem_trim();
        if (pn_mem.held + size > budget) {
            pn_mem.fails++;
            return NULL;
        }
        if (!(p = AllocMem(size, MEMF_ANY))) {
            pn_mem_trim();
            if (!(p = AllocMem(size, MEMF_ANY))) {
                pn_mem.fails++;
                return NULL;
            }
        }
        pn_mem.held += size;
    }
    p[0] = size;
    if (pn_mem.held - pn_mem.cached > pn_mem.peak)
        pn_mem.peak = pn_mem.held - pn_mem.cached;
    return p + HDR / 4;
}

void *pn_mem_calloc(ULONG count, ULONG n)
{
    void *p = pn_mem_malloc(count * n);
    if (p)
        memset(p, 0, count * n);
    return p;
}

void pn_mem_free(void *q)
{
    ULONG *p, size;
    struct sclass *c;

    if (!q)
        return;
    p = (ULONG *)q - HDR / 4;
    size = p[0];
    c = find_class(size, TRUE);
    if (c && c->count < cache_max && pn_mem.cached + size <= budget / 4) {
        ((struct fblock *)p)->next = c->head;
        c->head = (struct fblock *)p;
        c->count++;
        pn_mem.cached += size;
    } else {
        pn_mem.held -= size;
        FreeMem(p, size);
    }
}

/* At shutdown: everything lwIP still holds is gone with the core anyway,
 * but the cache is ours to return. */
void pn_mem_cleanup(void)
{
    pn_mem_trim();
}
