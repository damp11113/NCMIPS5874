/*
 * Launcher: WiFi page (wifiui.c).
 */
#ifndef LAUNCHER_WIFIUI_H
#define LAUNCHER_WIFIUI_H

/* SETTINGS > WiFi: state, scan, choose a network, passphrase */
void wifi_page(struct fb *fb);
/* "WiFi 192.168.2.233" / "WiFi: Connecting" / "" (no WiFi): 1 = changed */
int wifi_status_line(char *buf, int n);

#endif
