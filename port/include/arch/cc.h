/*
 * PrismNet - lwIP architecture definitions for AmigaOS (m68k, amiga-gcc).
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_ARCH_CC_H
#define PRISMNET_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>

/* m68k is big-endian; amiga-gcc does not define BYTE_ORDER itself. */
#ifndef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#endif

/* libnix provides errno.h but not the full BSD socket errno set; lwIP's
 * own table (lwip/errno.h) is used instead. */
#define LWIP_PROVIDE_ERRNO 1

/* Structure packing (gcc). m68k already aligns 32-bit members to 2. */
#define PACK_STRUCT_FIELD(x)  x
#define PACK_STRUCT_STRUCT    __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* Diagnostics (only with lwIP debugging on): printed to the Shell. */
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
/* The core logs the assertion and shuts down cleanly (core.c); other users
 * of the lwIP headers (pntest) get the plain abort(). */
#ifdef PN_CORE
void pn_assert_fail(const char *msg, const char *file, int line);
#define LWIP_PLATFORM_ASSERT(x) pn_assert_fail(x, __FILE__, __LINE__)
#else
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwIP assert: %s (%s:%d)\n", \
                                     x, __FILE__, __LINE__); abort(); } while (0)
#endif

/* Random numbers (DHCP xid, DNS id, local ports; the TCP ISN goes through
 * LWIP_HOOK_TCP_ISN, see lwipopts.h). */
unsigned long prismnet_rand(void);
#define LWIP_RAND() ((u32_t)prismnet_rand())

#endif /* PRISMNET_ARCH_CC_H */
