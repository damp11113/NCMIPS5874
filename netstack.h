/*
 * netstack.h: minimal IPv4 on top of wlan.h (Ethernet-style frames).
 *   ARP      answers requests for our address, learns MACs (small cache)
 *   IPv4     no fragments, no options
 *   ICMP     echo reply (ping)
 *   UDP      send + one receive hook
 *   DHCP     client: DISCOVER -> OFFER -> REQUEST -> ACK, hostname "ncwifi"
 * Needs before including: wlan_send (dst, type, data, len), mac[6].
 */
#ifndef NETSTACK_H
#define NETSTACK_H

#define NET_HOSTNAME    "ncwifi"

static u32 net_ip, net_mask, net_gw, net_dns, net_dhcp_server, net_lease;
static u32 net_pings, net_arp_replies, net_ip_rx;

/* ---- helpers ---- */

static u32 get_be32 (const unsigned char *p) {
    return ((u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void put_be32 (unsigned char *p, u32 v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* Internet checksum over n bytes, starting from sum */
static u32 net_csum (const unsigned char *p, u32 n, u32 sum) {
    while (n > 1) {
        sum += (p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n) {
        sum += p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum;
}

static void print_ip (const char *tag, u32 ip) {
    printf ("%s%d.%d.%d.%d", tag, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

/* ---- ARP ---- */

#define ARP_CACHE 8
static struct { u32 ip; unsigned char mac[6]; } arp_cache[ARP_CACHE];
static u32 arp_next;

static void arp_learn (u32 ip, const unsigned char *m) {
    u32 i;

    for (i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy (arp_cache[i].mac, m, 6);
            return;
        }
    }
    arp_cache[arp_next].ip = ip;
    memcpy (arp_cache[arp_next].mac, m, 6);
    arp_next = (arp_next + 1) % ARP_CACHE;
}

static const unsigned char *arp_lookup (u32 ip) {
    u32 i;

    for (i = 0; i < ARP_CACHE; i++) {
        if (ip && arp_cache[i].ip == ip) {
            return arp_cache[i].mac;
        }
    }
    return 0;
}

static void arp_send (u32 op, const unsigned char *tmac, u32 tip) {
    unsigned char a[28];

    put_be16 (a, 1);            /* Ethernet */
    put_be16 (a + 2, 0x0800);   /* IPv4 */
    a[4] = 6;
    a[5] = 4;
    put_be16 (a + 6, op);
    memcpy (a + 8, mac, 6);
    put_be32 (a + 14, net_ip);
    memcpy (a + 18, op == 1 ? (const unsigned char *) "\0\0\0\0\0\0" : tmac, 6);
    put_be32 (a + 24, tip);
    wlan_send (op == 1 ? bcast : tmac, 0x0806, a, 28);
}

static void arp_rx (const unsigned char *a, u32 len) {
    u32 op, sip, tip;

    if (len < 28 || be16 (a) != 1 || be16 (a + 2) != 0x0800) {
        return;
    }
    op = be16 (a + 6);
    sip = get_be32 (a + 14);
    tip = get_be32 (a + 24);
    if (sip) {
        arp_learn (sip, a + 8);
    }
    if (op == 1 && net_ip && tip == net_ip) {
        arp_send (2, a + 8, sip);
        net_arp_replies++;
    }
}

/* ---- IPv4 ---- */

static u32 ip_id;

/* Send an IPv4 packet (payload already built). 0 ok, -1 no MAC yet. */
static int ip_send (u32 dst, u32 proto, const unsigned char *payload, u32 len) {
    static unsigned char pkt[1500];
    const unsigned char *dmac;
    u32 via = dst, sum;

    if (len > 1480) {
        return -1;
    }
    if (dst == 0xffffffffu) {
        dmac = bcast;
    } else {
        if (net_mask && (dst & net_mask) != (net_ip & net_mask)) {
            via = net_gw;
        }
        dmac = arp_lookup (via);
        if (!dmac) {
            arp_send (1, 0, via);
            return -1;
        }
    }
    pkt[0] = 0x45;
    pkt[1] = 0;
    put_be16 (pkt + 2, 20 + len);
    put_be16 (pkt + 4, ip_id++);
    put_be16 (pkt + 6, 0);
    pkt[8] = 64;
    pkt[9] = proto;
    pkt[10] = pkt[11] = 0;
    put_be32 (pkt + 12, net_ip);
    put_be32 (pkt + 16, dst);
    sum = net_csum (pkt, 20, 0);
    put_be16 (pkt + 10, ~sum & 0xffff);
    memcpy (pkt + 20, payload, len);
    return wlan_send (dmac, 0x0800, pkt, 20 + len);
}

/* ---- UDP ---- */

static void (*udp_hook) (u32 src, u32 sport, u32 dport, const unsigned char *d, u32 len);

static int udp_send (u32 dst, u32 sport, u32 dport, const unsigned char *d, u32 len) {
    static unsigned char u[1480];

    if (len > 1472) {
        return -1;
    }
    put_be16 (u, sport);
    put_be16 (u + 2, dport);
    put_be16 (u + 4, 8 + len);
    put_be16 (u + 6, 0);        /* no checksum (allowed for IPv4) */
    memcpy (u + 8, d, len);
    return ip_send (dst, 17, u, 8 + len);
}

/* ---- DHCP client ---- */

enum { DHCP_OFF, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };
static u32 dhcp_state, dhcp_xid, dhcp_offer_ip;

static void dhcp_send (u32 type) {
    static unsigned char d[300];
    unsigned char *o;
    static const unsigned char params[] = { 1, 3, 6, 15, 51, 54 };

    memset (d, 0, sizeof (d));
    d[0] = 1;                   /* BOOTREQUEST */
    d[1] = 1;                   /* Ethernet */
    d[2] = 6;
    put_be32 (d + 4, dhcp_xid);
    put_be16 (d + 10, 0x8000);  /* broadcast reply: we have no address yet */
    memcpy (d + 28, mac, 6);    /* chaddr */
    put_be32 (d + 236, 0x63825363);
    o = d + 240;
    *o++ = 53;                  /* message type */
    *o++ = 1;
    *o++ = type;
    *o++ = 61;                  /* client id: Ethernet + MAC */
    *o++ = 7;
    *o++ = 1;
    memcpy (o, mac, 6);
    o += 6;
    *o++ = 12;                  /* host name */
    *o++ = sizeof (NET_HOSTNAME) - 1;
    memcpy (o, NET_HOSTNAME, sizeof (NET_HOSTNAME) - 1);
    o += sizeof (NET_HOSTNAME) - 1;
    if (type == 3) {
        *o++ = 50;              /* requested address */
        *o++ = 4;
        put_be32 (o, dhcp_offer_ip);
        o += 4;
        *o++ = 54;              /* server id */
        *o++ = 4;
        put_be32 (o, net_dhcp_server);
        o += 4;
    }
    *o++ = 55;                  /* parameter request list */
    *o++ = sizeof (params);
    memcpy (o, params, sizeof (params));
    o += sizeof (params);
    *o++ = 255;
    udp_send (0xffffffffu, 68, 67, d, (o - d) < 300 ? 300 : (o - d));
}

static void dhcp_rx (const unsigned char *d, u32 len) {
    const unsigned char *o = d + 240, *end = d + len;
    u32 type = 0, mask = 0, gw = 0, dns = 0, server = 0, lease = 0;

    if (len < 244 || d[0] != 2 || get_be32 (d + 4) != dhcp_xid ||
        get_be32 (d + 236) != 0x63825363 || memcmp (d + 28, mac, 6)) {
        return;
    }
    while (o + 2 <= end && o[0] != 255) {
        if (o[0] == 0) {
            o++;
            continue;
        }
        if (o + 2 + o[1] > end) {
            break;
        }
        if (o[0] == 53 && o[1] >= 1) {
            type = o[2];
        } else if (o[0] == 1 && o[1] >= 4) {
            mask = get_be32 (o + 2);
        } else if (o[0] == 3 && o[1] >= 4) {
            gw = get_be32 (o + 2);
        } else if (o[0] == 6 && o[1] >= 4) {
            dns = get_be32 (o + 2);
        } else if (o[0] == 54 && o[1] >= 4) {
            server = get_be32 (o + 2);
        } else if (o[0] == 51 && o[1] >= 4) {
            lease = get_be32 (o + 2);
        }
        o += 2 + o[1];
    }
    if (type == 2 && dhcp_state == DHCP_SELECTING) {        /* OFFER */
        dhcp_offer_ip = get_be32 (d + 16);
        net_dhcp_server = server;
        print_ip ("DHCP offer: ", dhcp_offer_ip);
        print_ip (" from ", server);
        printf ("\n");
        dhcp_state = DHCP_REQUESTING;
        dhcp_send (3);
    } else if (type == 5 && dhcp_state == DHCP_REQUESTING) {  /* ACK */
        net_ip = get_be32 (d + 16);
        net_mask = mask;
        net_gw = gw;
        net_dns = dns;
        net_lease = lease;
        dhcp_state = DHCP_BOUND;
        print_ip ("DHCP ack: address ", net_ip);
        print_ip (", mask ", net_mask);
        print_ip (", router ", net_gw);
        print_ip (", DNS ", net_dns);
        printf (", lease %d s\n", lease);
    } else if (type == 6) {                                 /* NAK */
        printf ("DHCP NAK - starting over\n");
        dhcp_state = DHCP_SELECTING;
        dhcp_send (1);
    }
}

/* ---- ICMP ---- */

static void icmp_rx (u32 src, const unsigned char *p, u32 len) {
    static unsigned char r[1480];
    u32 sum;

    if (len < 8 || p[0] != 8 || len > sizeof (r)) {
        return;                 /* echo request only */
    }
    memcpy (r, p, len);
    r[0] = 0;                   /* echo reply */
    r[2] = r[3] = 0;
    sum = net_csum (r, len, 0);
    put_be16 (r + 2, ~sum & 0xffff);
    if (ip_send (src, 1, r, len) == 0) {
        net_pings++;
    }
}

/* ---- receive: Ethernet-style frames from wlan_poll ---- */

static void net_rx (const unsigned char *dst, const unsigned char *src, u32 type,
                    const unsigned char *p, u32 len) {
    u32 ihl, tot, proto, sip, dip;

    (void) dst;
    if (type == 0x0806) {
        arp_rx (p, len);
        return;
    }
    if (type != 0x0800 || len < 20 || (p[0] >> 4) != 4) {
        return;
    }
    ihl = (p[0] & 15) * 4;
    tot = be16 (p + 2);
    if (ihl < 20 || tot < ihl || tot > len || (net_csum (p, ihl, 0) & 0xffff) != 0xffff) {
        return;
    }
    if (be16 (p + 6) & 0x3fff) {
        return;                 /* fragments not handled */
    }
    proto = p[9];
    sip = get_be32 (p + 12);
    dip = get_be32 (p + 16);
    if (dip != net_ip && dip != 0xffffffffu && !(net_mask && dip == (net_ip | ~net_mask))) {
        if (!(dhcp_state != DHCP_BOUND && proto == 17)) {
            return;             /* before DHCP: accept UDP to any address */
        }
    }
    net_ip_rx++;
    if (sip) {
        arp_learn (sip, src);
    }
    p += ihl;
    len = tot - ihl;
    if (proto == 1) {
        icmp_rx (sip, p, len);
    } else if (proto == 17 && len >= 8) {
        u32 sport = be16 (p), dport = be16 (p + 2), ulen = be16 (p + 4);

        if (ulen < 8 || ulen > len) {
            return;
        }
        if (dport == 68 && sport == 67) {
            dhcp_rx (p + 8, ulen - 8);
        } else if (udp_hook) {
            udp_hook (sip, sport, dport, p + 8, ulen - 8);
        }
    }
}

/* Get an address: up to tries DISCOVERs, 3 s each. 0 = bound. */
static int dhcp_run (u32 tries) {
    u32 c, t;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
    dhcp_xid = c ^ (mac[5] << 24) ^ (mac[4] << 16);
    for (t = 0; t < tries && dhcp_state != DHCP_BOUND; t++) {
        dhcp_state = DHCP_SELECTING;
        printf ("DHCP discover (%d)\n", t + 1);
        dhcp_send (1);
        wlan_poll (3000, net_rx);
        if (dhcp_state == DHCP_REQUESTING) {
            wlan_poll (3000, net_rx);
        }
    }
    return dhcp_state == DHCP_BOUND ? 0 : -1;
}

#endif
