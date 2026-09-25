/*
 * rtlhttp: fetch a web page over the internal WiFi. Joins the network in
 * WIFI.TXT (wlan.h), DHCP as "ncwifi", DNS lookup, TCP connect to port 80,
 * HTTP/1.0 GET (netstack.h). Prints the status line and headers, the start
 * of the body, the size and the speed. Plain HTTP only (no TLS).
 *
 *   fatload usb 0 ${a} rtlhttp.bin
 *   mw.b 83d00000 0 400; fatload usb 0 83d00000 WIFI.TXT
 *   fatload usb 0 82000000 rtl8188fufw.bin     (last: ${filesize} = fw size)
 *   usb port 1
 *   usb reset
 *   go ${a} 82000000 ${filesize} 83d00000 [host [path]]
 *                                          (default example.com /)
 */
#include "wlan.h"
#include "netstack.h"

static u32 body_shown, in_body, crlf;

/* Append string s at p, return the new end (path + host are short) */
static char *cat (char *p, const char *s) {
    while (*s) {
        *p++ = *s++;
    }
    *p = 0;
    return p;
}

/* Print the headers and the first 600 bytes of the body */
static void page_data (const unsigned char *d, u32 len) {
    u32 i;

    for (i = 0; i < len; i++) {
        unsigned char c = d[i];

        if (!in_body) {
            crlf = (c == '\n') ? crlf + 1 : (c == '\r' ? crlf : 0);
            if (crlf == 2) {
                in_body = 1;
                printf ("\n----- body -----\n");
                continue;
            }
        } else if (body_shown++ >= 600) {
            continue;
        }
        if (c == '\n' || c == '\t' || (c >= ' ' && c < 127)) {
            printf ("%c", c);
        }
    }
}

int main (int argc, char *argv[]) {
    const char *host = argc > 4 ? argv[4] : "example.com";
    const char *path = argc > 5 ? argv[5] : "/";
    static char req[640];
    u32 ip, t0, ms, n;

    if (argc < 4) {
        printf ("usage: go ${a} <fw-addr> <fw-size> <WIFI.TXT addr> [host [path]]\n");
        return 1;
    }
    if (wlan_join ((const unsigned char *) parse_hex (argv[1]), parse_hex (argv[2]),
                   (const char *) parse_hex (argv[3])) < 0) {
        return 1;
    }
    wlan_install_keys ();
    if (dhcp_run (4) < 0) {
        printf ("no DHCP answer\n");
        return 1;
    }
    arp_send (1, 0, net_gw);                        /* router MAC for everything outside */
    wlan_poll (300, net_rx);

    t0 = get_timer (0);
    ip = dns_resolve (host);
    if (!ip) {
        printf ("DNS: \"%s\" not found\n", host);
        return 1;
    }
    printf ("DNS: %s = ", host);
    print_ip ("", ip);
    printf (" (%d ms)\n", (int) get_timer (t0));

    net_debug = 1;                                  /* every TCP segment (debugging) */
    t0 = get_timer (0);
    if (tcp_connect (ip, 80, page_data) < 0) {
        printf ("TCP: no connection to port 80\n");
        return 1;
    }
    printf ("TCP: connected in %d ms\n", (int) get_timer (t0));

    {
        char *e = cat (req, "GET ");

        e = cat (e, path);
        e = cat (e, " HTTP/1.0\r\nHost: ");
        e = cat (e, host);
        e = cat (e, "\r\nUser-Agent: ncwifi\r\nConnection: close\r\n\r\n");
        n = e - req;
    }
    t0 = get_timer (0);
    if (tcp_write ((const unsigned char *) req, n) < 0) {
        printf ("TCP: request not acknowledged (%d bytes). WiFi: frames %d, data rx %d / tx %d, "
                "not decrypted %d, biggest data frame %d, IP rx %d\n", n, wlan_rx_frames,
                wlan_rx_data, wlan_tx_data, wlan_rx_undecrypted, wlan_rx_max, net_ip_rx);
        return 1;
    }
    printf ("----- response -----\n");
    tcp_read_all (5000);
    ms = get_timer (t0);
    printf ("\n----- end -----\n%d bytes in %d ms (%d KB/s), %d out-of-order segments%s\n",
            tcp_rx_bytes, ms, ms ? (int) (tcp_rx_bytes / ms) : 0, tcp_dup,
            tcp_state == TCP_DONE ? ", closed by the server" : ", timed out");
    return 0;
}
