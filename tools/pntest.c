/*
 * pntest - PrismNet developer self-test: lwIP alone on one SANA-II driver.
 * GPL-3.0-only, see LICENSE.
 *
 * Brings up lwIP on one SANA-II device inside this CLI process and
 * exercises it with the raw API: DHCP (or static address), ICMP echo,
 * DNS lookup and an HTTP/1.0 GET. While it runs it also answers ARP and
 * ping, so the Amiga can be pinged from outside.
 *
 *   pntest DEVICE=cnet.device UNIT=0 PING=10.0.2.2 LOOKUP=aminet.net
 *          GET=aminet.net/ TIME=10
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "lwip/init.h"

/* lwIP's SNTP app is in liblwip.a and calls this when the core uses it;
 * pntest never starts SNTP */
void pn_sntp_set(unsigned long sec, unsigned long us) { (void)sec; (void)us; }

#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/stats.h"

#include "prismnet/pnsys.h"
#include "prismnet/sanaif.h"

static const char version[] = "$VER: pntest 0.1 (15.10.2026) PrismNet";

#define TEMPLATE "DEVICE/K,UNIT/K/N,IP/K,NETMASK/K,GATEWAY/K,DNS/K," \
                 "PING/K,COUNT/K/N,LOOKUP/K,GET/K,TIME/K/N"
enum { A_DEVICE, A_UNIT, A_IP, A_NETMASK, A_GATEWAY, A_DNS,
       A_PING, A_COUNT, A_LOOKUP, A_GET, A_TIME, A_COUNT_ };

static struct sanaif eth;
static int failures;

/* ---------------------------------------------------------------------- */
/* run the stack until done() is true, a timeout, or Ctrl-C                */
/* ---------------------------------------------------------------------- */

static BOOL stop_requested;

static BOOL run_until(BOOL (*done)(void), u32_t timeout_ms)
{
    u32_t start = sys_now();
    static u32_t last_wd;

    for (;;) {
        u32_t sleep, sigs;

        sanaif_poll(&eth);
        /* Like the core: requeue reads the driver refused while it was still
         * coming online (e.g. scsidayna.device before its WiFi is up). */
        if (sys_now() - last_wd >= 250) {
            last_wd = sys_now();
            sanaif_watchdog(&eth, last_wd);
        }
        sys_check_timeouts();
        if (done && done())
            return TRUE;
        if (sys_now() - start >= timeout_ms)
            return FALSE;

        sleep = sys_timeouts_sleeptime();
        if (sleep > 50)
            sleep = 50;     /* also bounds the timeout/done() check */
        if (sleep == 0)
            sleep = 1;
        pn_timer_arm(sleep);
        sigs = Wait(sanaif_sigmask(&eth) | pn_timer_sigmask() | SIGBREAKF_CTRL_C);
        pn_timer_fired();
        if (sigs & SIGBREAKF_CTRL_C) {
            stop_requested = TRUE;
            return FALSE;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* address                                                                 */
/* ---------------------------------------------------------------------- */

static BOOL have_address(void)
{
    return !ip4_addr_isany_val(*netif_ip4_addr(&eth.netif));
}

static void status_cb(struct netif *n)
{
    if (!ip4_addr_isany_val(*netif_ip4_addr(n))) {
        printf("  address  %s", ip4addr_ntoa(netif_ip4_addr(n)));
        printf("  netmask %s", ip4addr_ntoa(netif_ip4_netmask(n)));
        printf("  gateway %s\n", ip4addr_ntoa(netif_ip4_gw(n)));
    }
}

/* ---------------------------------------------------------------------- */
/* ping                                                                    */
/* ---------------------------------------------------------------------- */

#define PING_ID 0x504e          /* 'PN' */
static u16_t ping_seq;
static BOOL  ping_got;
static u32_t ping_sent_at, ping_rtt;

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr)
{
    struct icmp_echo_hdr echo;
    u16_t hlen;

    LWIP_UNUSED_ARG(arg); LWIP_UNUSED_ARG(pcb); LWIP_UNUSED_ARG(addr);
    if (p->tot_len < IP_HLEN + sizeof(echo))
        return 0;
    hlen = (u16_t)(IPH_HL_BYTES((struct ip_hdr *)p->payload));
    if (pbuf_copy_partial(p, &echo, sizeof(echo), hlen) != sizeof(echo))
        return 0;
    if (echo.type == ICMP_ER && echo.id == lwip_htons(PING_ID) &&
        echo.seqno == lwip_htons(ping_seq)) {
        ping_rtt = sys_now() - ping_sent_at;
        ping_got = TRUE;
        pbuf_free(p);
        return 1;           /* eaten */
    }
    return 0;
}

static BOOL ping_done(void) { return ping_got; }

static void do_ping(const char *host, int count)
{
    struct raw_pcb *pcb;
    ip_addr_t dst;
    int i, ok = 0;

    if (!ipaddr_aton(host, &dst)) {
        printf("ping: %s is not an IP address\n", host);
        failures++;
        return;
    }
    pcb = raw_new(IP_PROTO_ICMP);
    raw_recv(pcb, ping_recv, NULL);
    raw_bind(pcb, IP_ADDR_ANY);

    for (i = 0; i < count && !stop_requested; i++) {
        struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 32, PBUF_RAM);
        struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)p->payload;
        u8_t *data = (u8_t *)(e + 1);
        int j;

        ping_seq++;
        ICMPH_TYPE_SET(e, ICMP_ECHO);
        ICMPH_CODE_SET(e, 0);
        e->id     = lwip_htons(PING_ID);
        e->seqno  = lwip_htons(ping_seq);
        e->chksum = 0;
        for (j = 0; j < 32; j++)
            data[j] = (u8_t)('a' + j % 26);
        e->chksum = inet_chksum(e, p->len);

        ping_got = FALSE;
        ping_sent_at = sys_now();
        raw_sendto(pcb, p, &dst);
        pbuf_free(p);

        if (run_until(ping_done, 2000)) {
            printf("  reply from %s: seq=%u time=%lu ms\n", host,
                   (unsigned)ping_seq, (unsigned long)ping_rtt);
            ok++;
        } else if (!stop_requested) {
            printf("  seq=%u timed out\n", (unsigned)ping_seq);
        }
        if (i + 1 < count)
            run_until(NULL, 1000 - (ping_got ? ping_rtt : 0) % 1000);
    }
    raw_remove(pcb);
    printf("ping %s: %d/%d replies\n", host, ok, count);
    if (ok == 0)
        failures++;
}

/* ---------------------------------------------------------------------- */
/* DNS                                                                     */
/* ---------------------------------------------------------------------- */

static int       dns_state;     /* 0 pending, 1 found, -1 failed */
static ip_addr_t dns_result;

static void dns_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    LWIP_UNUSED_ARG(name); LWIP_UNUSED_ARG(arg);
    if (addr) {
        dns_result = *addr;
        dns_state = 1;
    } else {
        dns_state = -1;
    }
}

static BOOL dns_done(void) { return dns_state != 0; }

static BOOL resolve(const char *name, ip_addr_t *out)
{
    err_t e;
    u32_t t0 = sys_now();

    dns_state = 0;
    e = dns_gethostbyname(name, &dns_result, dns_cb, NULL);
    if (e == ERR_OK)
        dns_state = 1;
    else if (e != ERR_INPROGRESS)
        dns_state = -1;
    else
        run_until(dns_done, 10000);

    if (dns_state == 1) {
        printf("  %s -> %s (%lu ms)\n", name, ipaddr_ntoa(&dns_result),
               (unsigned long)(sys_now() - t0));
        *out = dns_result;
        return TRUE;
    }
    printf("  %s: lookup failed\n", name);
    return FALSE;
}

/* ---------------------------------------------------------------------- */
/* HTTP GET                                                                */
/* ---------------------------------------------------------------------- */

static struct {
    struct tcp_pcb *pcb;
    int    state;           /* 0 connecting, 1 connected, 2 closed, -1 error */
    err_t  err;
    char   request[256];
    char   status[100];
    int    status_len;
    BOOL   status_done;
    u32_t  bytes;
} http;

static void http_close(void)
{
    if (http.pcb) {
        tcp_arg(http.pcb, NULL);
        tcp_recv(http.pcb, NULL);
        tcp_err(http.pcb, NULL);
        if (tcp_close(http.pcb) != ERR_OK)
            tcp_abort(http.pcb);
        http.pcb = NULL;
    }
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct pbuf *q;

    LWIP_UNUSED_ARG(arg);
    if (!p || err != ERR_OK) {
        if (p)
            pbuf_free(p);
        http.state = 2;
        http_close();
        return ERR_OK;
    }
    for (q = p; q; q = q->next) {
        const char *c = (const char *)q->payload;
        u16_t i;
        for (i = 0; i < q->len && !http.status_done; i++) {
            if (c[i] == '\r' || c[i] == '\n' ||
                http.status_len == (int)sizeof(http.status) - 1)
                http.status_done = TRUE;
            else
                http.status[http.status_len++] = c[i];
        }
    }
    http.bytes += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void http_err(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    http.pcb   = NULL;      /* already freed by lwIP */
    http.err   = err;
    http.state = -1;
}

static err_t http_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    if (err != ERR_OK) {
        http.err = err;
        http.state = -1;
        return err;
    }
    http.state = 1;
    tcp_write(pcb, http.request, (u16_t)strlen(http.request), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static BOOL http_done(void) { return http.state == 2 || http.state == -1; }

static void do_get(const char *arg)
{
    char host[128];
    const char *path = strchr(arg, '/');
    size_t hl = path ? (size_t)(path - arg) : strlen(arg);
    ip_addr_t addr;
    u32_t t0, ms;

    if (hl >= sizeof(host))
        hl = sizeof(host) - 1;
    memcpy(host, arg, hl);
    host[hl] = 0;
    if (!path)
        path = "/";

    printf("GET http://%s%s\n", host, path);
    if (!resolve(host, &addr)) {
        failures++;
        return;
    }

    memset(&http, 0, sizeof(http));
    snprintf(http.request, sizeof(http.request),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PrismNet-pntest/0.1\r\n"
             "Connection: close\r\n\r\n", path, host);
    http.pcb = tcp_new();
    tcp_recv(http.pcb, http_recv);
    tcp_err(http.pcb, http_err);

    t0 = sys_now();
    if (tcp_connect(http.pcb, &addr, 80, http_connected) != ERR_OK) {
        printf("  tcp_connect failed\n");
        http_close();
        failures++;
        return;
    }
    if (!run_until(http_done, 30000)) {
        printf("  timed out after %lu bytes\n", (unsigned long)http.bytes);
        http_close();
        failures++;
        return;
    }
    ms = sys_now() - t0;
    if (http.state == -1) {
        printf("  connection error %d after %lu bytes\n", (int)http.err,
               (unsigned long)http.bytes);
        failures++;
        return;
    }
    printf("  status: %s\n", http.status);
    printf("  %lu bytes in %lu ms", (unsigned long)http.bytes, (unsigned long)ms);
    if (ms > 0)
        printf(" (%lu bytes/s)", (unsigned long)(http.bytes * 1000ULL / ms));
    printf("\n");
}

/* ---------------------------------------------------------------------- */

static BOOL parse_ip(const char *s, ip4_addr_t *out, const char *what)
{
    if (!ip4addr_aton(s, out)) {
        printf("pntest: bad %s address '%s'\n", what, s);
        return FALSE;
    }
    return TRUE;
}

/* The port code logs through the core's core_log(); here it just prints. */
void core_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int main(void)
{
    LONG args[A_COUNT_] = { 0 };
    struct RDArgs *rda;
    const char *dev = "cnet.device";
    ULONG unit = 0;
    int count = 4;
    ip4_addr_t ip, mask, gw;
    BOOL use_dhcp = TRUE;
    const char *why;
    LONG err;

    (void)version;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"pntest");
        return RETURN_FAIL;
    }
    if (args[A_DEVICE]) dev   = (const char *)args[A_DEVICE];
    if (args[A_UNIT])   unit  = *(LONG *)args[A_UNIT];
    if (args[A_COUNT])  count = (int)*(LONG *)args[A_COUNT];

    ip4_addr_set_zero(&ip);
    ip4_addr_set_zero(&mask);
    ip4_addr_set_zero(&gw);
    if (args[A_IP]) {
        use_dhcp = FALSE;
        if (!parse_ip((const char *)args[A_IP], &ip, "IP") ||
            !parse_ip(args[A_NETMASK] ? (const char *)args[A_NETMASK] : "255.255.255.0",
                      &mask, "NETMASK") ||
            (args[A_GATEWAY] && !parse_ip((const char *)args[A_GATEWAY], &gw, "GATEWAY"))) {
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    printf("PrismNet pntest 0.1 - lwIP %s\n", LWIP_VERSION_STRING);

    if (!pn_sys_init()) {
        printf("pntest: cannot open timer.device\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    lwip_init();

    err = sanaif_open(&eth, dev, unit, &ip, &mask, &gw, &why);
    if (err != 0) {
        printf("pntest: %s unit %lu: %s (error %ld, wire error %lu)\n", dev,
               (unsigned long)unit, why, (long)err, (unsigned long)eth.open_wire_error);
        pn_sys_cleanup();
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    printf("%s unit %lu: hw %02x:%02x:%02x:%02x:%02x:%02x  mtu %lu  %lu bit/s\n",
           dev, (unsigned long)unit,
           eth.mac[0], eth.mac[1], eth.mac[2], eth.mac[3], eth.mac[4], eth.mac[5],
           (unsigned long)eth.mtu, (unsigned long)eth.bps);

    netif_set_status_callback(&eth.netif, status_cb);
    netif_set_hostname(&eth.netif, "prismnet");
    netif_set_default(&eth.netif);
    netif_set_up(&eth.netif);

    if (use_dhcp) {
        printf("DHCP...\n");
        dhcp_start(&eth.netif);
        if (!run_until(have_address, 20000)) {
            printf("pntest: no DHCP lease%s\n", stop_requested ? " (break)" : "");
            failures++;
            goto out;
        }
    }

    if (args[A_DNS]) {
        ip_addr_t d;
        if (ipaddr_aton((const char *)args[A_DNS], &d))
            dns_setserver(0, &d);
    }
    {
        const ip_addr_t *d = dns_getserver(0);
        if (!ip_addr_isany(d))
            printf("  dns      %s\n", ipaddr_ntoa(d));
    }

    if (args[A_PING] && !stop_requested) {
        printf("PING %s\n", (const char *)args[A_PING]);
        do_ping((const char *)args[A_PING], count);
    }
    if (args[A_LOOKUP] && !stop_requested) {
        ip_addr_t a;
        printf("LOOKUP %s\n", (const char *)args[A_LOOKUP]);
        if (!resolve((const char *)args[A_LOOKUP], &a))
            failures++;
    }
    if (args[A_GET] && !stop_requested)
        do_get((const char *)args[A_GET]);

    if (args[A_TIME] && !stop_requested) {
        printf("Staying up %ld s (answering ping)...\n", (long)*(LONG *)args[A_TIME]);
        run_until(NULL, (u32_t)*(LONG *)args[A_TIME] * 1000);
    }

out:
    printf("stats: rx %lu pkts/%lu bytes (%lu dropped, %lu err)  "
           "tx %lu pkts/%lu bytes (%lu dropped, %lu err)\n",
           (unsigned long)eth.stats.rx_packets, (unsigned long)eth.stats.rx_bytes,
           (unsigned long)eth.stats.rx_dropped, (unsigned long)eth.stats.rx_errors,
           (unsigned long)eth.stats.tx_packets, (unsigned long)eth.stats.tx_bytes,
           (unsigned long)eth.stats.tx_dropped, (unsigned long)eth.stats.tx_errors);
    if (use_dhcp)
        dhcp_release_and_stop(&eth.netif);
    sanaif_close(&eth);
    pn_sys_cleanup();
    FreeArgs(rda);
    printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
    return failures ? RETURN_WARN : RETURN_OK;
}

/* liblwip is built for the core (PN_CORE) and routes assertions here. */
void pn_assert_fail(const char *msg, const char *file, int line)
{
    printf("lwIP assert: %s (%s:%d)\n", msg, file, line);
    abort();
}

/* liblwip calls the core's static-route hooks; pntest has no routes. */
struct netif *pn_route_netif(const ip4_addr_t *dest) { (void)dest; return NULL; }
const ip4_addr_t *pn_route_gw(struct netif *netif, const ip4_addr_t *dest)
{
    (void)netif; (void)dest;
    return NULL;
}
