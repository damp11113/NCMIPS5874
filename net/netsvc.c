/*
 * NCAPPS network service: the IPTV box's internal WiFi (RTL8188FTV on its
 * own USB controller, ehci.h + rtl8188.h + wlan.h, WPA2-PSK) under lwIP,
 * as tasks of the launcher's system (launcher/system.c).
 *
 *   net    2   brings the chip up, joins the network in /NCAPPS/WIFI.TXT,
 *              moves received frames into lwIP, notices a lost link
 *              (deauthentication, no beacons for 10 s) and joins again
 *   tcpip  3   lwIP's own thread (DHCP as "ncwifi", DNS, TCP, UDP)
 *
 * Frames out (lwIP's linkoutput, any task) and the net task share the
 * chip's TX path: wlan_lock. Apps reach the service through @sys
 * (sys->net = net_ops, sdk/net.h). Everything lives in the launcher's own
 * area: static data, FreeRTOS heap for task stacks / lwIP mailboxes.
 *
 * GPL-2.0-only parts: rtl8188.h, wlan.h (from Linux rtl8xxxu).
 */
#define RTL_EHCI
#define WLAN_IDLE()     vTaskDelay(1)

#include "sdk.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "lwip/init.h"
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"
#include "lwip/inet_chksum.h"
#include "lwip/icmp.h"
#include "lwip/prot/icmp.h"
#include "netif/ethernet.h"
#include "wlan.h"
#include "netsvc.h"

#define PRIO_NET        2
#define PRIO_NET_BUSY   1               /* chip init / PMK: share the CPU with the UI */
#define NET_STACK       (16 * 1024)     /* words */
#define AP_TIMEOUT_MS   10000           /* no frame from the AP: link lost */
#define NET_MAX_SOCK    MEMP_NUM_NETCONN

enum { CMD_RECONNECT = 1, CMD_SCAN, CMD_OFF, CMD_STOP };

static TaskHandle_t net_task;
static QueueHandle_t net_q;
static SemaphoreHandle_t wlan_lock, scan_done, stop_done;
static struct netif wifi_netif;
static volatile int ns = SDK_NET_OFF;   /* SDK_NET_* */
static int link_on, tcpip_on;
static u32 retry_at, retry_ms, reconnects, drops;
static char msg[64];

static unsigned char fw[32768] __attribute__((aligned(32)));
static long fw_size;

/* configuration (WIFI.TXT) */
static char cfg_ssid[33], cfg_psk[64];
static int cfg_on = 1;

/* scan results for the UI */
static struct net_ap *scan_list;
static int scan_max, scan_n;

static void set_msg(const char *m) {
    snprintf(msg, sizeof(msg), "%s", m);
    printf("net: %s\n", m);
}

/* ---- WIFI.TXT ---- */

static void config_load(void) {
    static char t[513];
    long n = sdk_read_file(NET_WIFI_TXT, t, 512);

    if (n <= 0) {
        n = sdk_read_file(NET_WIFI_TXT_ROOT, t, 512);      /* the rtl* tools' file */
    }
    char *p = t;

    cfg_ssid[0] = cfg_psk[0] = 0;
    cfg_on = 1;
    if (n <= 0) {
        return;
    }
    t[n] = 0;
    while (*p) {
        char *e = p, *v;
        int len;

        while (*e && *e != '\n') {
            e++;
        }
        len = e - p;
        if (len && p[len - 1] == '\r') {
            len--;
        }
        if (*p != '#' && (v = memchr(p, '=', len)) != 0) {
            int kl = v - p, vl = len - kl - 1;

            v++;
            if (kl == 4 && !memcmp(p, "ssid", 4) && vl < (int) sizeof(cfg_ssid)) {
                memcpy(cfg_ssid, v, vl);
                cfg_ssid[vl] = 0;
            } else if (kl == 3 && !memcmp(p, "psk", 3) && vl < (int) sizeof(cfg_psk)) {
                memcpy(cfg_psk, v, vl);
                cfg_psk[vl] = 0;
            } else if (kl == 2 && !memcmp(p, "on", 2)) {
                cfg_on = *v != '0';
            }
        }
        p = *e ? e + 1 : e;
    }
}

static int config_save(void) {
    static char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
                 NET_WIFI_MAGIC "\n"
                 "# Written by the launcher (SETTINGS > WiFi). on=0: WiFi off.\n"
                 "# The passphrase is stored as typed: keep this stick private.\n"
                 "on=%d\nssid=%s\npsk=%s\n", cfg_on, cfg_ssid, cfg_psk);
    if (n >= (int) sizeof(buf) - 1) {
        return -1;
    }
    memset(buf + n, '#', sizeof(buf) - 1 - n);
    buf[sizeof(buf) - 1] = '\n';
    return sdk_overwrite_sector_file(NET_WIFI_TXT, buf, NET_WIFI_MAGIC);
}

/* ---- lwIP interface ---- */

/* An Ethernet-style frame from the AP (wlan.h, net task) into lwIP */
static void wifi_rx(const unsigned char *dst, const unsigned char *src, u32 type,
                    const unsigned char *data, u32 len) {
    unsigned char eh[14];
    struct pbuf *p;

    if (!link_on || len > 1500) {
        return;
    }
    p = pbuf_alloc(PBUF_RAW, 14 + len, PBUF_POOL);
    if (!p) {
        drops++;
        return;
    }
    memcpy(eh, dst, 6);
    memcpy(eh + 6, src, 6);
    eh[12] = type >> 8;
    eh[13] = type;
    pbuf_take(p, eh, 14);
    pbuf_take_at(p, data, len, 14);
    if (wifi_netif.input(p, &wifi_netif) != ERR_OK) {
        pbuf_free(p);
        drops++;
    }
}

/* lwIP's frame out (tcpip thread or an app's task) */
static err_t wifi_output(struct netif *n, struct pbuf *p) {
    static unsigned char copy[1514];
    const unsigned char *f = p->payload;
    int r;

    (void) n;
    if (!link_on || p->tot_len < 14 || p->tot_len > sizeof(copy)) {
        return ERR_IF;
    }
    if (xSemaphoreTake(wlan_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        drops++;
        return ERR_IF;
    }
    if (p->len != p->tot_len) {
        pbuf_copy_partial(p, copy, p->tot_len, 0);
        f = copy;
    }
    r = wlan_send(f, f[12] << 8 | f[13], f + 14, p->tot_len - 14);
    xSemaphoreGive(wlan_lock);
    return r < 0 ? ERR_IF : ERR_OK;
}

static err_t wifi_netif_init(struct netif *n) {
    n->name[0] = 'w';
    n->name[1] = 'l';
    n->output = etharp_output;
    n->linkoutput = wifi_output;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, mac, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif_set_hostname(n, "ncwifi");
    return ERR_OK;
}

static void link_up(void) {
    LOCK_TCPIP_CORE();
    link_on = 1;
    netif_set_link_up(&wifi_netif);
    dhcp_start(&wifi_netif);
    UNLOCK_TCPIP_CORE();
}

static void link_down(void) {
    if (!link_on) {
        return;
    }
    LOCK_TCPIP_CORE();
    link_on = 0;
    dhcp_release_and_stop(&wifi_netif);
    netif_set_link_down(&wifi_netif);
    UNLOCK_TCPIP_CORE();
}

/* ---- the net task ---- */

static void scan_found(const char *name, u32 len, const unsigned char *b, int ch, int rssi,
                       int sec) {
    int i;

    (void) b;
    for (i = 0; i < scan_n; i++) {
        if (strlen(scan_list[i].ssid) == len && !memcmp(scan_list[i].ssid, name, len)) {
            if (rssi > scan_list[i].rssi) {
                scan_list[i].rssi = rssi;
                scan_list[i].channel = ch;
            }
            return;
        }
    }
    if (scan_n < scan_max) {
        memcpy(scan_list[scan_n].ssid, name, len);
        scan_list[scan_n].ssid[len] = 0;
        scan_list[scan_n].rssi = rssi;
        scan_list[scan_n].channel = ch;
        scan_list[scan_n].security = sec;
        scan_n++;
    }
}

static void do_scan(void) {
    int i, j;

    link_down();
    ns = SDK_NET_SCANNING;
    xSemaphoreTake(wlan_lock, portMAX_DELAY);
    scan_n = 0;
    wlan_scan(scan_found, 120);
    xSemaphoreGive(wlan_lock);
    for (i = 1; i < scan_n; i++) {          /* strongest first */
        struct net_ap t = scan_list[i];

        for (j = i; j > 0 && scan_list[j - 1].rssi < t.rssi; j--) {
            scan_list[j] = scan_list[j - 1];
        }
        scan_list[j] = t;
    }
    printf("net: scan found %d networks\n", scan_n);
    ns = cfg_ssid[0] && cfg_on ? SDK_NET_FAILED : SDK_NET_NO_CONFIG;
    retry_at = get_timer(0);                /* reconnect at once */
    xSemaphoreGive(scan_done);
}

static void do_connect(void) {
    int r;

    link_down();
    if (!cfg_on || !cfg_ssid[0]) {
        ns = SDK_NET_NO_CONFIG;
        set_msg(cfg_on ? "no network chosen (SETTINGS > WiFi)" : "WiFi switched off");
        return;
    }
    ns = SDK_NET_CONNECTING;
    vTaskPrioritySet(NULL, PRIO_NET_BUSY);
    xSemaphoreTake(wlan_lock, portMAX_DELAY);
    r = wlan_connect(cfg_ssid, cfg_psk);
    if (r == 0) {
        wlan_install_keys();
    }
    xSemaphoreGive(wlan_lock);
    vTaskPrioritySet(NULL, PRIO_NET);
    if (r < 0) {
        ns = SDK_NET_FAILED;
        retry_ms = retry_ms ? (retry_ms * 2 > 120000 ? 120000 : retry_ms * 2) : 10000;
        retry_at = get_timer(0) + retry_ms;
        snprintf(msg, sizeof(msg), "could not join \"%.24s\", retry in %u s", cfg_ssid,
                 (unsigned) (retry_ms / 1000));
        printf("net: %s\n", msg);
        return;
    }
    retry_ms = 0;
    ns = SDK_NET_DHCP;
    snprintf(msg, sizeof(msg), "joined \"%.40s\"", cfg_ssid);
    link_up();
}

static void net_entry(void *arg) {
    int cmd;

    (void) arg;
    ns = SDK_NET_STARTING;
    fw_size = sdk_read_file(NET_FIRMWARE, fw, sizeof(fw));
    if (fw_size <= 0) {
        fw_size = sdk_read_file(NET_FIRMWARE_ROOT, fw, sizeof(fw));
    }
    if (fw_size <= 0) {
        set_msg("no " NET_FIRMWARE " on the stick (WiFi firmware)");
        ns = SDK_NET_OFF;
        vTaskSuspend(NULL);
    }
    config_load();
    vTaskPrioritySet(NULL, PRIO_NET_BUSY);
    if (wlan_up(fw, fw_size) < 0) {
        set_msg("WiFi chip did not start (see serial)");
        ns = SDK_NET_OFF;
        vTaskSuspend(NULL);
    }
    vTaskPrioritySet(NULL, PRIO_NET);
    wlan_rx_cb = wifi_rx;

    tcpip_init(NULL, NULL);
    LOCK_TCPIP_CORE();
    netif_add(&wifi_netif, NULL, NULL, NULL, NULL, wifi_netif_init, tcpip_input);
    netif_set_default(&wifi_netif);
    netif_set_up(&wifi_netif);
    UNLOCK_TCPIP_CORE();
    tcpip_on = 1;
    printf("net: lwIP %s up, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", LWIP_VERSION_STRING,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    retry_at = get_timer(0);
    ns = SDK_NET_NO_CONFIG;
    for (;;) {
        int busy = 0;

        while (xQueueReceive(net_q, &cmd, 0) == pdPASS) {
            if (cmd == CMD_SCAN) {
                do_scan();
            } else if (cmd == CMD_RECONNECT) {
                link_down();
                ns = SDK_NET_NO_CONFIG;
                retry_ms = 0;
                retry_at = get_timer(0);
            } else if (cmd == CMD_OFF) {
                link_down();
                ns = SDK_NET_NO_CONFIG;
                set_msg("WiFi switched off");
            } else if (cmd == CMD_STOP) {
                link_down();
                xSemaphoreTake(wlan_lock, portMAX_DELAY);
                rtl_power_off();
                ehci_close(&rtl_hc);
                ns = SDK_NET_OFF;
                xSemaphoreGive(stop_done);
                vTaskSuspend(NULL);
            }
        }
        if ((ns == SDK_NET_NO_CONFIG || ns == SDK_NET_FAILED) && cfg_on && cfg_ssid[0] &&
            (int) (get_timer(0) - retry_at) >= 0) {
            do_connect();
            continue;
        }
        if (ns == SDK_NET_DHCP || ns == SDK_NET_UP) {
            xSemaphoreTake(wlan_lock, portMAX_DELAY);
            busy = wlan_rx_pump();
            xSemaphoreGive(wlan_lock);
            if (state != S_DONE || get_timer(wlan_ap_seen) > AP_TIMEOUT_MS) {
                set_msg(state != S_DONE ? "the access point dropped us, joining again" :
                        "access point gone (no beacons), joining again");
                link_down();
                ns = SDK_NET_FAILED;
                reconnects++;
                retry_at = get_timer(0) + 2000;
                continue;
            }
            if (ns == SDK_NET_DHCP && dhcp_supplied_address(&wifi_netif)) {
                char a[16];

                ns = SDK_NET_UP;
                snprintf(msg, sizeof(msg), "address %s",
                         sdk_ip_str(lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&wifi_netif))),
                                    a));
                printf("net: %s (host name ncwifi)\n", msg);
            }
        }
        if (!busy) {
            vTaskDelay(1);
        }
    }
}

static void net_send_cmd(int cmd) {
    if (net_q) {
        xQueueSend(net_q, &cmd, pdMS_TO_TICKS(100));
    }
}

/* ---- the launcher's side ---- */

int net_scan(struct net_ap *list, int max) {
    if (!net_task || ns == SDK_NET_OFF || ns == SDK_NET_STARTING) {
        return -1;
    }
    xSemaphoreTake(scan_done, 0);
    scan_list = list;
    scan_max = max;
    net_send_cmd(CMD_SCAN);
    if (xSemaphoreTake(scan_done, pdMS_TO_TICKS(25000)) != pdTRUE) {
        return -1;
    }
    return scan_n;
}

int net_configure(const char *ssid, const char *psk, int on) {
    int r;

    if (ssid) {
        snprintf(cfg_ssid, sizeof(cfg_ssid), "%s", ssid);
    }
    if (psk) {
        snprintf(cfg_psk, sizeof(cfg_psk), "%s", psk);
    }
    cfg_on = on;
    r = config_save();
    net_send_cmd(on ? CMD_RECONNECT : CMD_OFF);
    return r;
}

void net_config(char *ssid, int n, int *on, int *has_psk) {
    snprintf(ssid, n, "%s", cfg_ssid);
    *on = cfg_on;
    *has_psk = cfg_psk[0] != 0;
}

void net_service_stop(void) {
    if (net_task && ns != SDK_NET_OFF && ns != SDK_NET_STARTING) {
        net_send_cmd(CMD_STOP);
        xSemaphoreTake(stop_done, pdMS_TO_TICKS(1000));
    }
}

/* ---- sockets for apps (sdk/net.h) ---- */

static unsigned char app_sock[NET_MAX_SOCK];

static int ready(void) {
    return ns == SDK_NET_UP;
}

static int track(int s) {
    if (s >= 0 && s < NET_MAX_SOCK) {
        app_sock[s] = 1;
    }
    return s;
}

static int net_err(void) {
    switch (errno) {
    case EWOULDBLOCK:
        return SDK_NET_TIMEOUT;
    case ECONNREFUSED:
    case ECONNRESET:
    case ECONNABORTED:
        return SDK_NET_REFUSED;
    case ENOTCONN:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
        return SDK_NET_DOWN;
    default:
        return SDK_NET_ERR;
    }
}

static void sin_set(struct sockaddr_in *sa, u32 ip, int port) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = AF_INET;
    sa->sin_port = lwip_htons(port);
    sa->sin_addr.s_addr = lwip_htonl(ip);
}

static void ms_tv(struct timeval *tv, int ms) {
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

/* Wait until s is readable (rd) or writable: 1, 0 = timeout, < 0 error */
static int wait_sock(int s, int rd, int timeout_ms) {
    fd_set set;
    struct timeval tv;

    FD_ZERO(&set);
    FD_SET(s, &set);
    ms_tv(&tv, timeout_ms);
    return lwip_select(s + 1, rd ? &set : NULL, rd ? NULL : &set, NULL,
                       timeout_ms < 0 ? NULL : &tv);
}

static int op_info(struct sdk_net_info *i) {
    memset(i, 0, sizeof(*i));
    i->state = ns;
    snprintf(i->ssid, sizeof(i->ssid), "%s", cfg_ssid);
    memcpy(i->mac, mac, 6);
    i->rssi = (ns == SDK_NET_DHCP || ns == SDK_NET_UP) ? wlan_rssi : 0;
    i->channel = (ns == SDK_NET_DHCP || ns == SDK_NET_UP) ? ap_channel : 0;
    if (ns == SDK_NET_UP) {
        const ip_addr_t *d = dns_getserver(0);

        i->ip = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&wifi_netif)));
        i->netmask = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(&wifi_netif)));
        i->gateway = lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(&wifi_netif)));
        i->dns = d ? lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(d))) : 0;
    }
    i->rx_frames = wlan_rx_data;
    i->tx_frames = wlan_tx_data;
    i->reconnects = reconnects;
    snprintf(i->message, sizeof(i->message), "%s", msg);
    return 0;
}

static int op_resolve(const char *host, u32 *ip) {
    ip_addr_t a;

    if (ip4addr_aton(host, ip_2_ip4(&a))) {
        *ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&a)));
        return 0;
    }
    if (!ready()) {
        return SDK_NET_DOWN;
    }
    if (netconn_gethostbyname(host, &a) != ERR_OK) {
        return SDK_NET_NOTFOUND;
    }
    *ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&a)));
    return 0;
}

static int op_tcp_connect(u32 ip, int port, int timeout_ms) {
    struct sockaddr_in sa;
    int s, r, on = 1, off = 0, e;
    socklen_t el = sizeof(e);

    if (!ready()) {
        return SDK_NET_DOWN;
    }
    s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return SDK_NET_ERR;
    }
    sin_set(&sa, ip, port);
    lwip_ioctl(s, FIONBIO, &on);
    r = lwip_connect(s, (struct sockaddr *) &sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) {
        r = net_err();
        lwip_close(s);
        return r;
    }
    if (r < 0) {
        r = wait_sock(s, 0, timeout_ms);
        if (r <= 0) {
            lwip_close(s);
            return r == 0 ? SDK_NET_TIMEOUT : SDK_NET_ERR;
        }
        if (lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
            lwip_close(s);
            return e == ECONNREFUSED || e == ECONNRESET ? SDK_NET_REFUSED : SDK_NET_ERR;
        }
    }
    lwip_ioctl(s, FIONBIO, &off);
    return track(s);
}

static int op_tcp_listen(int port, int backlog) {
    struct sockaddr_in sa;
    int s, on = 1;

    if (!tcpip_on) {
        return SDK_NET_DOWN;
    }
    s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return SDK_NET_ERR;
    }
    lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sin_set(&sa, 0, port);
    if (lwip_bind(s, (struct sockaddr *) &sa, sizeof(sa)) < 0 ||
        lwip_listen(s, backlog > 0 ? backlog : 1) < 0) {
        lwip_close(s);
        return SDK_NET_ERR;
    }
    return track(s);
}

static int op_tcp_accept(int s, u32 *ip, int *port, int timeout_ms) {
    struct sockaddr_in sa;
    socklen_t l = sizeof(sa);
    int c, r = wait_sock(s, 1, timeout_ms);

    if (r <= 0) {
        return r == 0 ? SDK_NET_TIMEOUT : SDK_NET_ERR;
    }
    c = lwip_accept(s, (struct sockaddr *) &sa, &l);
    if (c < 0) {
        return net_err();
    }
    if (ip) {
        *ip = lwip_ntohl(sa.sin_addr.s_addr);
    }
    if (port) {
        *port = lwip_ntohs(sa.sin_port);
    }
    return track(c);
}

static int op_udp_open(int port) {
    struct sockaddr_in sa;
    int s;

    if (!tcpip_on) {
        return SDK_NET_DOWN;
    }
    s = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return SDK_NET_ERR;
    }
    if (port) {
        sin_set(&sa, 0, port);
        if (lwip_bind(s, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
            lwip_close(s);
            return SDK_NET_ERR;
        }
    }
    return track(s);
}

static int op_send(int s, const void *data, int len) {
    const char *p = data;
    int done = 0;

    while (done < len) {
        int n = lwip_send(s, p + done, len - done, 0);

        if (n < 0) {
            return net_err();
        }
        done += n;
    }
    return done;
}

static int op_recv(int s, void *buf, int len, int timeout_ms) {
    int n, r;

    if (timeout_ms >= 0) {
        r = wait_sock(s, 1, timeout_ms);
        if (r <= 0) {
            return r == 0 ? SDK_NET_TIMEOUT : SDK_NET_ERR;
        }
    }
    n = lwip_recv(s, buf, len, 0);
    return n < 0 ? net_err() : n;
}

static int op_sendto(int s, u32 ip, int port, const void *data, int len) {
    struct sockaddr_in sa;
    int n;

    sin_set(&sa, ip, port);
    n = lwip_sendto(s, data, len, 0, (struct sockaddr *) &sa, sizeof(sa));
    return n < 0 ? net_err() : n;
}

static int op_recvfrom(int s, void *buf, int len, u32 *ip, int *port, int timeout_ms) {
    struct sockaddr_in sa;
    socklen_t l = sizeof(sa);
    int n, r;

    if (timeout_ms >= 0) {
        r = wait_sock(s, 1, timeout_ms);
        if (r <= 0) {
            return r == 0 ? SDK_NET_TIMEOUT : SDK_NET_ERR;
        }
    }
    n = lwip_recvfrom(s, buf, len, 0, (struct sockaddr *) &sa, &l);
    if (n < 0) {
        return net_err();
    }
    if (ip) {
        *ip = lwip_ntohl(sa.sin_addr.s_addr);
    }
    if (port) {
        *port = lwip_ntohs(sa.sin_port);
    }
    return n;
}

static int op_close(int s) {
    if (s >= 0 && s < NET_MAX_SOCK) {
        app_sock[s] = 0;
    }
    return lwip_close(s) < 0 ? SDK_NET_ERR : 0;
}

static int op_ping(u32 ip, int timeout_ms) {
    static u16_t ping_seq;
    struct sockaddr_in sa;
    unsigned char pkt[sizeof(struct icmp_echo_hdr) + 32], in[128];
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *) pkt;
    u32 t0;
    int s, i, r = SDK_NET_TIMEOUT;

    if (!ready()) {
        return SDK_NET_DOWN;
    }
    s = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (s < 0) {
        return SDK_NET_ERR;
    }
    memset(pkt, 0, sizeof(pkt));
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    e->id = lwip_htons(0x4e43);
    e->seqno = lwip_htons(++ping_seq);
    for (i = sizeof(*e); i < (int) sizeof(pkt); i++) {
        pkt[i] = 'a' + i % 26;
    }
    e->chksum = inet_chksum(pkt, sizeof(pkt));
    sin_set(&sa, ip, 0);
    t0 = get_timer(0);
    if (lwip_sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        lwip_close(s);
        return net_err();
    }
    while ((int) get_timer(t0) < timeout_ms) {
        int left = timeout_ms - (int) get_timer(t0), n, h;

        if (wait_sock(s, 1, left) <= 0) {
            break;
        }
        n = lwip_recv(s, in, sizeof(in), 0);
        h = (in[0] & 15) * 4;
        if (n >= h + (int) sizeof(*e)) {
            struct icmp_echo_hdr *rp = (struct icmp_echo_hdr *) (in + h);

            if (ICMPH_TYPE(rp) == ICMP_ER && rp->id == e->id && rp->seqno == e->seqno) {
                r = get_timer(t0);
                break;
            }
        }
    }
    lwip_close(s);
    return r;
}

static const struct sdk_net_ops net_ops = {
    sizeof(struct sdk_net_ops), op_info, op_resolve, op_tcp_connect, op_tcp_listen,
    op_tcp_accept, op_udp_open, op_send, op_recv, op_sendto, op_recvfrom, op_close, op_ping,
};

void net_app_end(void) {
    int s;

    for (s = 0; s < NET_MAX_SOCK; s++) {
        if (app_sock[s]) {
            printf("net: closing socket %d the app left open\n", s);
            op_close(s);
        }
    }
}

const struct sdk_net_ops *net_service_start(void) {
    if (sdk_box_sat) {
        return 0;                           /* no WiFi module on the satellite box */
    }
    net_q = xQueueCreate(8, sizeof(int));
    wlan_lock = xSemaphoreCreateMutex();
    scan_done = xSemaphoreCreateBinary();
    stop_done = xSemaphoreCreateBinary();
    if (!net_q || !wlan_lock || !scan_done || !stop_done ||
        xTaskCreate(net_entry, "net", NET_STACK, NULL, PRIO_NET, &net_task) != pdPASS) {
        printf("net: cannot create the task\n");
        net_task = 0;
        return 0;
    }
    sdk_net_host = &net_ops;
    return &net_ops;
}
