/*
 * rtljoin: join a WPA2-PSK (CCMP) network with the internal RTL8188FTV
 * (wlan.h: scan, open auth, association, 4-way handshake), then stay on
 * the channel and report whether the access point keeps us.
 *
 * WIFI.TXT on the stick (keep it private, never in git):
 *   ssid=MyNetwork
 *   psk=my passphrase
 *
 *   fatload usb 0 ${a} rtljoin.bin
 *   mw.b 83d00000 0 400; fatload usb 0 83d00000 WIFI.TXT
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize} 83d00000
 */
#include "wlan.h"

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: go ${a} <fw-addr> <fw-size> <WIFI.TXT addr>\n");
        return 1;
    }
    if (wlan_join((const unsigned char *) parse_hex(argv[1]), parse_hex(argv[2]),
                   (const char *) parse_hex(argv[3])) < 0) {
        return 1;
    }
    printf("Listening 20 s for a deauthentication...\n");
    wlan_listen(20000, 1);
    if (state == S_DONE) {
        printf("still associated after 20 s. Check your router's list of connected devices.\n");
    }
    return state == S_DONE ? 0 : 1;
}
