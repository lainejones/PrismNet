/*
 * PrismNet - lwIP hook prototypes (LWIP_HOOK_FILENAME).
 * GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMNET_LWIPHOOKS_H
#define PRISMNET_LWIPHOOKS_H

#include "lwip/ip_addr.h"

u32_t prismnet_tcp_isn(const ip_addr_t *local_ip, u16_t local_port,
                       const ip_addr_t *remote_ip, u16_t remote_port);

/* Static routes (core.c): the interface and the gateway for a destination
 * that is on no interface's own subnet. NULL = no static route. */
struct netif;
struct netif *pn_route_netif(const ip4_addr_t *dest);
const ip4_addr_t *pn_route_gw(struct netif *netif, const ip4_addr_t *dest);

#endif
