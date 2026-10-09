/*
 * Network for apps (sdk.h includes it). Served by the NCAPPS system: the
 * launcher with Multitasking on runs the WiFi (IPTV box: internal
 * RTL8188FTV) and lwIP; apps reach it through @sys (sys.h, version 3).
 * Without the system, or on a box without WiFi, every call returns
 * SDK_NET_DOWN.
 *
 * Addresses are IPv4 in host order: SDK_IP(192, 168, 2, 1). Calls block
 * the app (other tasks run meanwhile) for at most their timeout_ms
 * (0 = return at once, -1 = no limit).
 *
 *   u32 ip;
 *   char buf[512];
 *   int s, n;
 *
 *   if (sdk_net_resolve("example.com", &ip) == 0 &&
 *       (s = sdk_tcp_connect(ip, 80, 5000)) >= 0) {
 *       sdk_net_send(s, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", 37);
 *       while ((n = sdk_net_recv(s, buf, sizeof(buf), 5000)) > 0) {
 *           ...
 *       }
 *       sdk_net_close(s);
 *   }
 *
 * Sockets an app leaves open are closed when it ends. Plain TCP / UDP
 * only (no TLS).
 */
#ifndef SDK_NET_H
#define SDK_NET_H

#define SDK_IP(a, b, c, d)  ((u32) (a) << 24 | (u32) (b) << 16 | (u32) (c) << 8 | (u32) (d))

/* errors (all negative) */
#define SDK_NET_ERR         (-1)    /* other error */
#define SDK_NET_TIMEOUT     (-2)
#define SDK_NET_DOWN        (-3)    /* no network: no system / WiFi, not connected */
#define SDK_NET_NOTFOUND    (-4)    /* name not resolved */
#define SDK_NET_REFUSED     (-5)    /* connection refused / reset by the other side */

/* state */
enum {
    SDK_NET_OFF,                    /* no WiFi on this box / switched off */
    SDK_NET_STARTING,               /* chip being set up */
    SDK_NET_NO_CONFIG,              /* chip ready, no network chosen */
    SDK_NET_CONNECTING,             /* looking for the network, handshake */
    SDK_NET_DHCP,                   /* connected, waiting for an address */
    SDK_NET_UP,                     /* address: ready */
    SDK_NET_FAILED,                 /* join failed (retries later) */
    SDK_NET_SCANNING,               /* looking for networks (settings page) */
};

struct sdk_net_info {
    int state;                      /* SDK_NET_* */
    char ssid[33];
    unsigned char mac[6];
    int rssi;                       /* dBm of the access point, smoothed */
    int channel;
    u32 ip, netmask, gateway, dns;  /* host order, 0 = none */
    u32 rx_frames, tx_frames;       /* data frames since the start */
    u32 reconnects;                 /* links lost and joined again */
    char message[64];               /* last problem / event, for people */
};

/* The service table behind it (@sys version 3, sys->net). Apps use the
 * functions below, not this. */
struct sdk_net_ops {
    u32 size;                       /* sizeof (struct sdk_net_ops) of the system */
    int (*info)(struct sdk_net_info *info);
    int (*resolve)(const char *host, u32 *ip);
    int (*tcp_connect)(u32 ip, int port, int timeout_ms);
    int (*tcp_listen)(int port, int backlog);
    int (*tcp_accept)(int s, u32 *ip, int *port, int timeout_ms);
    int (*udp_open)(int port);
    int (*send)(int s, const void *data, int len);
    int (*recv)(int s, void *buf, int len, int timeout_ms);
    int (*sendto)(int s, u32 ip, int port, const void *data, int len);
    int (*recvfrom)(int s, void *buf, int len, u32 *ip, int *port, int timeout_ms);
    int (*close)(int s);
    int (*ping)(u32 ip, int timeout_ms);
};

/* Fills info (state SDK_NET_OFF without the network). 0 / SDK_NET_DOWN */
int sdk_net_info(struct sdk_net_info *info);
/* 1 = connected with an address */
int sdk_net_ready(void);
/* Name (or "1.2.3.4") to address. 0 / SDK_NET_NOTFOUND / SDK_NET_DOWN */
int sdk_net_resolve(const char *host, u32 *ip);
/* TCP: socket number (>= 0) or error */
int sdk_tcp_connect(u32 ip, int port, int timeout_ms);
int sdk_tcp_listen(int port, int backlog);
int sdk_tcp_accept(int s, u32 *ip, int *port, int timeout_ms);
/* UDP socket on local port (0 = any) */
int sdk_udp_open(int port);
/* Bytes sent (all of them for TCP) or error */
int sdk_net_send(int s, const void *data, int len);
/* Bytes received, 0 = closed by the other side, or error */
int sdk_net_recv(int s, void *buf, int len, int timeout_ms);
int sdk_net_sendto(int s, u32 ip, int port, const void *data, int len);
int sdk_net_recvfrom(int s, void *buf, int len, u32 *ip, int *port, int timeout_ms);
int sdk_net_close(int s);
/* ICMP echo: round trip in ms or error */
int sdk_net_ping(u32 ip, int timeout_ms);

/* "192.168.2.1" into buf (16 bytes) */
char *sdk_ip_str(u32 ip, char *buf);

#endif
