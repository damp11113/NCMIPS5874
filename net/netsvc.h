/*
 * NCAPPS network service (net/netsvc.c), hosted by the launcher's system
 * (launcher/system.c). Apps use sdk/net.h; this is the launcher's side.
 */
#ifndef NETSVC_H
#define NETSVC_H

#include "sdk.h"

#define NET_WIFI_TXT    "/NCAPPS/WIFI.TXT"      /* 512 bytes, written in place */
#define NET_WIFI_MAGIC  "# NCAPPS wifi"
#define NET_FIRMWARE    "/NCAPPS/RTLWIFI.BIN"   /* linux-firmware rtl8188fufw.bin */
/* read-only fallbacks: the files the stand-alone rtl* tools use */
#define NET_WIFI_TXT_ROOT "/WIFI.TXT"
#define NET_FIRMWARE_ROOT "/RTL8188FUFW.BIN"

/* A network seen by net_scan (one entry per name, strongest signal) */
struct net_ap {
    char ssid[33];
    int rssi, channel, security;        /* security: WLAN_* (wlan.h) */
};

enum { NET_SEC_OPEN, NET_SEC_WEP_WPA1, NET_SEC_WPA2_PSK, NET_SEC_OTHER };

/* From system_run: start the service (IPTV box only); its table for @sys */
const struct sdk_net_ops *net_service_start(void);
/* Before the scheduler ends (EXIT to the stock firmware): WiFi chip off */
void net_service_stop(void);
/* After an app: close the sockets it left open */
void net_app_end(void);

/* Settings page (UI task). Scan: blocks ~2-3 s, the connection drops
 * meanwhile and comes back after. Count, or -1 (no WiFi). */
int net_scan(struct net_ap *list, int max);
/* Save WIFI.TXT and (re)connect (on = 1) or switch off. 0 / -1 not saved */
int net_configure(const char *ssid, const char *psk, int on);
/* What WIFI.TXT says */
void net_config(char *ssid, int n, int *on, int *has_psk);

#endif
