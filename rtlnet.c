/*
 * rtlnet: the box on the home network over the internal WiFi. Joins the
 * network in WIFI.TXT (wlan.h), puts the keys into the chip, gets an
 * address by DHCP as "ncwifi" (netstack.h), then answers ARP and ping.
 * Prints counters every 10 s; any serial key stops.
 *
 *   fatload usb 0 ${a} rtlnet.bin
 *   mw.b 83d00000 0 400; fatload usb 0 83d00000 WIFI.TXT
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize} 83d00000 [data=be]
 */
#include "wlan.h"
#include "netstack.h"

int main (int argc, char *argv[]) {
    u32 t;
    int i;

    if (argc < 4) {
        printf ("usage: go ${a} <fw-addr> <fw-size> <WIFI.TXT addr> [data=be]\n");
        return 1;
    }
    for (i = 4; i < argc; i++) {
        if (!memcmp (argv[i], "data=be", 7)) {
            wlan_data_be = 1;
            printf ("data frames on the BE queue / EP 0x03\n");
        }
    }
    if (wlan_join ((const unsigned char *) parse_hex (argv[1]), parse_hex (argv[2]),
                   (const char *) parse_hex (argv[3])) < 0) {
        return 1;
    }
    wlan_install_keys ();

    if (dhcp_run (4) < 0) {
        printf ("no DHCP answer. frames rx %d (data %d, not decrypted %d), data tx %d\n",
                wlan_rx_frames, wlan_rx_data, wlan_rx_undecrypted, wlan_tx_data);
        return 1;
    }
    arp_send (1, 0, net_gw);                    /* learn the router's MAC */
    print_ip ("\nREADY: ping ", net_ip);
    printf (" from your PC (host name %s). Any serial key stops.\n\n", NET_HOSTNAME);

    for (t = 0; state == S_DONE; t += 10) {
        wlan_poll (10000, net_rx);
        printf ("[%4d s] pings answered %d, ARP replies %d, IP rx %d, data rx %d / tx %d, "
                "not decrypted %d\n", t + 10, net_pings, net_arp_replies, net_ip_rx,
                wlan_rx_data, wlan_tx_data, wlan_rx_undecrypted);
        if (tstc ()) {
            getc ();
            break;
        }
    }
    if (state != S_DONE) {
        printf ("lost the connection\n");
        return 1;
    }
    return 0;
}
