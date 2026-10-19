/*
 * PrismNet NFS client: the few socket calls the protocol layer needs.
 * net_amiga.c implements them over bsdsocket.library, net_posix.c over the
 * PC's sockets (host tests). GPL-3.0-only, see LICENSE.
 */
#ifndef PNFS_NET_H
#define PNFS_NET_H

#include "nettypes.h"

/* errors returned (negative) by the calls below */
#define NET_ETIMEDOUT  (-1)
#define NET_ECLOSED    (-2)          /* peer closed or reset */
#define NET_EDOWN      (-3)          /* no stack / stack stopped */
#define NET_EHOST      (-4)          /* name did not resolve */
#define NET_EREFUSED   (-5)
#define NET_EBREAK     (-6)          /* interrupted (Ctrl-C / ACTION_DIE) */

/* open/drop the stack (Amiga: bsdsocket.library base of this task) */
int   net_up(void);
void  net_down(void);
/* host name or dotted quad -> IPv4 in host order; 0 if it does not resolve */
u32   net_resolve(const char *host);
/* TCP connect; resvport: bind a source port below 1024 first. >= 0 = handle */
long  net_connect(u32 ip, unsigned port, int resvport, unsigned timeout_s);
/* send all of buf; 0 or a negative error */
long  net_send(long s, const u8 *buf, u32 len);
/* receive exactly len bytes within timeout_s; 0 or a negative error */
long  net_recv(long s, u8 *buf, u32 len, unsigned timeout_s);
void  net_close(long s);
/* bytes that are hard to guess (NTLM client challenge); seconds since 1970 */
void  net_random(u8 *buf, u32 n);
u32   net_time(void);

#endif
