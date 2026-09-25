/*
 * netstack.h: minimal IPv4 on top of wlan.h (Ethernet-style frames).
 *   ARP      answers requests for our address, learns MACs (small cache)
 *   IPv4     no fragments, no options
 *   ICMP     echo reply (ping)
 *   UDP      send + one receive hook
 *   DHCP     client: DISCOVER -> OFFER -> REQUEST -> ACK, hostname "ncwifi"
 *   DNS      A records through the DHCP-supplied server
 *   TCP      one client connection: handshake (MSS 1400), stop-and-wait
 *            sending with retransmit, in-order receive with an ACK for
 *            every segment, FIN / RST
 * Needs before including: wlan_send (dst, type, data, len), mac[6].
 */
#ifndef NETSTACK_H
#define NETSTACK_H

#define NET_HOSTNAME    "ncwifi"

static u32 net_ip, net_mask, net_gw, net_dns, net_dhcp_server, net_lease;
static u32 net_pings, net_arp_replies, net_ip_rx;
static int net_debug;                  /* print every TCP segment */

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

static void tcp_rx (u32 src, const unsigned char *p, u32 len);

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
    } else if (proto == 6) {
        tcp_rx (sip, p, len);
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

static u32 net_rand (void) {
    static u32 x = 0x9e3779b9;
    u32 c;

    __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
    x ^= c + (x << 6) + (x >> 2);
    return x;
}

/* ---- DNS (A records, via the DHCP-supplied server) ---- */

static u32 dns_id, dns_port, dns_result;
static int dns_done;

static void dns_rx (const unsigned char *d, u32 len) {
    u32 q, an, i, p = 12;

    if (len < 12 || be16 (d) != dns_id || !(d[2] & 0x80)) {
        return;
    }
    dns_done = 1;
    if ((d[3] & 15) != 0) {
        return;                                     /* rcode: no such name etc. */
    }
    q = be16 (d + 4);
    an = be16 (d + 6);
    for (i = 0; i < q; i++) {                       /* skip the questions */
        while (p < len && d[p] && (d[p] & 0xc0) != 0xc0) {
            p += 1 + d[p];
        }
        p += (p < len && (d[p] & 0xc0) == 0xc0) ? 2 : 1;
        p += 4;
    }
    for (i = 0; i < an && p + 12 <= len; i++) {
        u32 type, rdlen;

        while (p < len && d[p] && (d[p] & 0xc0) != 0xc0) {
            p += 1 + d[p];
        }
        p += (p < len && (d[p] & 0xc0) == 0xc0) ? 2 : 1;
        if (p + 10 > len) {
            return;
        }
        type = be16 (d + p);
        rdlen = be16 (d + p + 8);
        p += 10;
        if (type == 1 && rdlen == 4 && p + 4 <= len) {
            dns_result = get_be32 (d + p);
            return;
        }
        p += rdlen;                                 /* CNAME etc.: next answer */
    }
}

static void net_udp_rx (u32 src, u32 sport, u32 dport, const unsigned char *d, u32 len) {
    (void) src;
    if (sport == 53 && dport == dns_port) {
        dns_rx (d, len);
    }
}

/* Name -> address (0 = failed). 2 tries, 3 s each. */
__attribute__ ((unused))
static u32 dns_resolve (const char *name) {
    unsigned char q[300], *p;
    u32 t, w;

    udp_hook = net_udp_rx;
    dns_result = 0;
    for (t = 0; t < 2 && !dns_result; t++) {
        const char *s = name;

        dns_id = net_rand () & 0xffff;
        dns_port = 49152 + (net_rand () & 0x3fff);
        memset (q, 0, 12);
        put_be16 (q, dns_id);
        q[2] = 0x01;                                /* recursion desired */
        put_be16 (q + 4, 1);
        p = q + 12;
        while (*s && p < q + 280) {                 /* labels */
            unsigned char *lenp = p++;

            while (*s && *s != '.' && p < q + 280) {
                *p++ = *s++;
            }
            *lenp = p - lenp - 1;
            if (*s == '.') {
                s++;
            }
        }
        *p++ = 0;
        put_be16 (p, 1);                            /* type A */
        put_be16 (p + 2, 1);                        /* class IN */
        p += 4;
        dns_done = 0;
        if (udp_send (net_dns, dns_port, 53, q, p - q) < 0) {
            wlan_poll (300, net_rx);                /* ARP for the DNS server first */
            udp_send (net_dns, dns_port, 53, q, p - q);
        }
        for (w = 0; w < 30 && !dns_done; w++) {
            wlan_poll (100, net_rx);
        }
    }
    return dns_result;
}

/* ---- TCP: one client connection ---- */

enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_DONE };

#define TCP_MSS         1400
#define TCP_WINDOW      16384

static u32 tcp_state, tcp_rip, tcp_rport, tcp_lport;
static u32 tcp_snd_una, tcp_snd_nxt, tcp_rcv_nxt;
static u32 tcp_rx_bytes, tcp_dup;
static void (*tcp_data_fn) (const unsigned char *d, u32 len);

#define TF_FIN  0x01
#define TF_SYN  0x02
#define TF_RST  0x04
#define TF_PSH  0x08
#define TF_ACK  0x10

static int tcp_seg (u32 seq, u32 flags, const unsigned char *d, u32 len) {
    if (net_debug) {
        printf ("  tcp tx: flags %02x seq una+%d len %d\n", flags, seq - tcp_snd_una, len);
    }
    static unsigned char s[1480];
    unsigned char ph[12];
    u32 hl = (flags & TF_SYN) ? 24 : 20, sum;

    if (hl + len > sizeof (s)) {
        return -1;
    }
    put_be16 (s, tcp_lport);
    put_be16 (s + 2, tcp_rport);
    put_be32 (s + 4, seq);
    put_be32 (s + 8, (flags & TF_ACK) ? tcp_rcv_nxt : 0);
    s[12] = (hl / 4) << 4;
    s[13] = flags;
    put_be16 (s + 14, TCP_WINDOW);
    s[16] = s[17] = 0;
    s[18] = s[19] = 0;
    if (flags & TF_SYN) {
        s[20] = 2;                                  /* MSS option */
        s[21] = 4;
        put_be16 (s + 22, TCP_MSS);
    }
    if (len) {
        memcpy (s + hl, d, len);
    }
    put_be32 (ph, net_ip);                          /* pseudo header */
    put_be32 (ph + 4, tcp_rip);
    ph[8] = 0;
    ph[9] = 6;
    put_be16 (ph + 10, hl + len);
    sum = net_csum (ph, 12, 0);
    sum = net_csum (s, hl + len, sum);
    put_be16 (s + 16, ~sum & 0xffff);
    return ip_send (tcp_rip, 6, s, hl + len);
}

static void tcp_rx (u32 src, const unsigned char *p, u32 len) {
    u32 hl, seq, ack, flags, dlen;

    if (len < 20 || src != tcp_rip || be16 (p) != tcp_rport || be16 (p + 2) != tcp_lport ||
        tcp_state == TCP_CLOSED) {
        return;
    }
    hl = (p[12] >> 4) * 4;
    if (hl < 20 || hl > len) {
        return;
    }
    seq = get_be32 (p + 4);
    ack = get_be32 (p + 8);
    flags = p[13];
    dlen = len - hl;
    if (net_debug) {
        printf ("  tcp rx: flags %02x seq rcv+%d ack una+%d len %d (our nxt una+%d)\n", flags,
                (int) (seq - tcp_rcv_nxt), (int) (ack - tcp_snd_una), dlen,
                (int) (tcp_snd_nxt - tcp_snd_una));
    }
    if (flags & TF_RST) {
        printf ("TCP: connection reset by the server\n");
        tcp_state = TCP_DONE;
        return;
    }
    if (tcp_state == TCP_SYN_SENT) {
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK) && ack == tcp_snd_nxt) {
            tcp_rcv_nxt = seq + 1;
            tcp_snd_una = ack;
            tcp_state = TCP_ESTABLISHED;
            tcp_seg (tcp_snd_nxt, TF_ACK, 0, 0);
        }
        return;
    }
    if ((flags & TF_ACK) && (int) (ack - tcp_snd_una) > 0 && (int) (ack - tcp_snd_nxt) <= 0) {
        tcp_snd_una = ack;
    }
    if (dlen) {
        if (seq == tcp_rcv_nxt) {
            if (tcp_data_fn) {
                tcp_data_fn (p + hl, dlen);
            }
            tcp_rcv_nxt += dlen;
            tcp_rx_bytes += dlen;
        } else {
            tcp_dup++;                              /* out of order / repeat: ACK tells */
        }
    }
    if ((flags & TF_FIN) && seq + dlen == tcp_rcv_nxt) {
        tcp_rcv_nxt++;
        if (tcp_state == TCP_ESTABLISHED) {
            tcp_seg (tcp_snd_nxt, TF_FIN | TF_ACK, 0, 0);   /* close our side too */
            tcp_snd_nxt++;
        }
        tcp_state = TCP_DONE;
    }
    if (dlen || (flags & TF_FIN)) {
        tcp_seg (tcp_snd_nxt, TF_ACK, 0, 0);
    }
}

/* 0 = connected. Resends the SYN every second, 5 tries. fn gets every
 * received data piece from now on (the answer may arrive while tcp_write
 * still waits for its ACK). */
__attribute__ ((unused))
static int tcp_connect (u32 ip, u32 port, void (*fn) (const unsigned char *d, u32 len)) {
    u32 t, w;

    tcp_data_fn = fn;

    tcp_rip = ip;
    tcp_rport = port;
    tcp_lport = 49152 + (net_rand () & 0x3fff);
    tcp_snd_una = tcp_snd_nxt = net_rand ();
    tcp_rcv_nxt = 0;
    tcp_rx_bytes = tcp_dup = 0;
    tcp_state = TCP_SYN_SENT;
    for (t = 0; t < 5 && tcp_state == TCP_SYN_SENT; t++) {
        if (tcp_seg (tcp_snd_nxt, TF_SYN, 0, 0) < 0) {
            wlan_poll (300, net_rx);                /* no MAC yet: ARP went out */
            continue;
        }
        if (t == 0) {
            tcp_snd_nxt++;
        }
        for (w = 0; w < 10 && tcp_state == TCP_SYN_SENT; w++) {
            wlan_poll (100, net_rx);
        }
    }
    if (t && tcp_snd_nxt == tcp_snd_una) {
        tcp_snd_nxt++;                              /* SYN never went out */
    }
    return tcp_state == TCP_ESTABLISHED ? 0 : -1;
}

/* Send data (up to one MSS at a time), wait for each piece to be ACKed */
__attribute__ ((unused))
static int tcp_write (const unsigned char *d, u32 len) {
    while (len && tcp_state == TCP_ESTABLISHED) {
        u32 n = len > TCP_MSS ? TCP_MSS : len, seq = tcp_snd_nxt, t, w;

        /* snd_nxt moves at once: a FIN from the server in the same burst
         * as its ACK must get our FIN with the right sequence number */
        tcp_snd_nxt = seq + n;
        for (t = 0; t < 6 && tcp_snd_una != seq + n; t++) {
            if (tcp_state == TCP_ESTABLISHED) {
                tcp_seg (seq, TF_ACK | TF_PSH, d, n);
            }
            for (w = 0; w < 10 && tcp_snd_una != seq + n; w++) {
                wlan_poll (100, net_rx);
            }
        }
        if (tcp_snd_una != seq + n && (int) (tcp_snd_una - (seq + n)) < 0) {
            return -1;
        }
        d += n;
        len -= n;
    }
    return len ? -1 : 0;
}

/* Receive (into the tcp_connect callback) until the server closes, or
 * idle_ms without data. */
__attribute__ ((unused))
static void tcp_read_all (u32 idle_ms) {
    u32 last = tcp_rx_bytes, idle = 0;

    while (tcp_state == TCP_ESTABLISHED && idle < idle_ms) {
        wlan_poll (200, net_rx);
        if (tcp_rx_bytes != last) {
            last = tcp_rx_bytes;
            idle = 0;
        } else {
            idle += 200;
        }
    }
    tcp_data_fn = 0;
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
