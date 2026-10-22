/*
 * PrismNet core: configuration from ENV:PrismNet/.
 * GPL-3.0-only, see LICENSE.
 *
 *   ENV:PrismNet/interfaces/<name>  DEVICE=cnet.device UNIT=0 CONFIG=DHCP
 *                                   CONFIG=STATIC ADDRESS=a.b.c.d NETMASK=.. [GATEWAY=..]
 *                                   [MTU=n]   (ADDRESS=DHCP also means DHCP)
 *   ENV:PrismNet/routes             DEFAULT a.b.c.d
 *                                   ROUTE a.b.c.d[/bits] gateway   (static, up to 16)
 *   ENV:PrismNet/dns                NAMESERVER a.b.c.d   (up to 3)
 *                                   DOMAIN example.lan
 *   ENV:PrismNet/hostname           name
 *   ENV:PrismNet/hosts              a.b.c.d name [alias...]   (read by the resolver)
 *   ENV:PrismNet/startup            AUTOSTART=YES|NO    (library starts the core)
 *                                   MEMORY=<KB>         (most lwIP may use)
 *
 * Keys are case-insensitive; '#' starts a comment.
 */
#ifndef PRISMNET_CONFIG_H
#define PRISMNET_CONFIG_H

#include <exec/types.h>

#define PN_CONFIG_DIR     "ENV:PrismNet"
#define PN_CONFIG_ARC     "ENVARC:PrismNet"
#define PN_HOSTS_FILE     "ENV:PrismNet/hosts"
#ifndef PN_MAX_IFACES
#define PN_MAX_IFACES     4
#endif
#define PN_MAX_ROUTES     16

struct PnRoute {
    ULONG net, mask, gw;                /* network order */
    UBYTE bits;
};

struct PnIfConfig {
    char  name[16];
    char  device[64];
    ULONG unit;
    BOOL  dhcp;
    ULONG address, netmask, gateway;    /* network order, 0 = unset */
    ULONG mtu;                          /* 0 = device default */
    ULONG stall;                        /* ms a write may be held before the driver is reset, 0 = default */
    BOOL  slip;                         /* built-in SLIP over a serial port */
    ULONG baud;                         /* ... its speed, 0 = 19200 */
    BOOL  rtscts;                       /* ... with RTS/CTS handshake */
};

struct PnConfig {
    struct PnIfConfig ifs[PN_MAX_IFACES];
    int    nifs;
    ULONG  default_gw;                  /* network order, 0 = none */
    struct PnRoute routes[PN_MAX_ROUTES];
    int    nroutes;
    ULONG  nameservers[3];
    int    nns;
    char   domain[64];
    char   hostname[64];
    BOOL   autostart;
    char   inetd[96];           /* startup INETD=: started when ENV:PrismNet/services exists */
    ULONG  mem_budget;          /* startup MEMORY=<KB>, in bytes; 0 = default */
    /* ENV:PrismNet/time: SERVER=host|DHCP SETCLOCK=YES|NO. No file = off. */
    BOOL   setclock;
    BOOL   ntp_dhcp;            /* take the server DHCP offers (option 42) first */
    char   ntp_server[64];      /* the name to ask (also the fallback with DHCP) */
};

/* Read ENV:PrismNet. Returns FALSE with a message in `err` on a hard error.
 * Unknown keys produce warnings via `warn` (may be NULL). */
BOOL pn_config_load(struct PnConfig *cfg, char *err, int errsize,
                    void (*warn)(const char *msg));

/* Parse a dotted quad into network order. */
BOOL pn_parse_ip(const char *s, ULONG *out);

#endif
