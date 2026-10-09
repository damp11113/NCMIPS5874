/*
 * NETTEST: network check for the NCAPPS system's WiFi (SDK app,
 * NCAPPS/APPS/NETTEST). Needs Multitasking on (the launcher's system
 * serves the network, sdk/net.h).
 *
 *   OK      ping the router, DNS lookup, HTTP GET http://example.com/
 *   GREEN   TCP receive speed test: listens on port 5001; send data from
 *           a PC, e.g.  python apps/nettest/tcpsend.py <box address>
 *   YELLOW  TCP send speed test: connects to <PC>:5002 (the address the
 *           last GREEN test came from) and sends 8 MB
 *   BACK    quit
 * Everything shown is also printed on the serial console.
 */
#include "sdk.h"

#define BG          RGB(12, 18, 40)
#define PANEL       RGB(24, 34, 72)
#define LINE_H      32
#define BODY_Y      260
#define MAX_LINES   12

static struct fb fb;
static char lines[MAX_LINES][82];
static u16 line_color[MAX_LINES];
static int nlines;
static u32 peer_ip;                         /* the PC of the last receive test */
static char buf[16384];

static void add(u16 color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void add(u16 color, const char *fmt, ...) {
    va_list ap;

    if (nlines == MAX_LINES) {
        memmove(lines[0], lines[1], sizeof(lines[0]) * (MAX_LINES - 1));
        memmove(line_color, line_color + 1, sizeof(line_color[0]) * (MAX_LINES - 1));
        nlines--;
    }
    va_start(ap, fmt);
    vsnprintf(lines[nlines], sizeof(lines[0]), fmt, ap);
    va_end(ap);
    line_color[nlines] = color;
    printf("nettest: %s\n", lines[nlines]);
    nlines++;
}

static void draw_status(void) {
    struct sdk_net_info i;
    char l[100], a[16], b[16], c[16];

    fb_rect(&fb, 0, 110, fb.w, 130, BG);
    if (sdk_net_info(&i) < 0) {
        fb_text(&fb, 40, 124, "No network: needs Multitasking ON (launcher SETTINGS)", 2, YELLOW,
                TRANSPARENT);
        return;
    }
    if (i.state == SDK_NET_UP) {
        snprintf(l, sizeof(l), "Connected to \"%s\"  %d dBm  channel %d", i.ssid, i.rssi,
                 i.channel);
        fb_text(&fb, 40, 124, l, 2, GREEN, TRANSPARENT);
        snprintf(l, sizeof(l), "Address %s  router %s  DNS %s", sdk_ip_str(i.ip, a),
                 sdk_ip_str(i.gateway, b), sdk_ip_str(i.dns, c));
        fb_text(&fb, 40, 160, l, 2, WHITE, TRANSPARENT);
    } else {
        snprintf(l, sizeof(l), "Network state %d: %s", i.state, i.message);
        fb_text(&fb, 40, 124, l, 2, YELLOW, TRANSPARENT);
    }
    snprintf(l, sizeof(l), "frames in %u out %u, reconnects %u", (unsigned) i.rx_frames,
             (unsigned) i.tx_frames, (unsigned) i.reconnects);
    fb_text(&fb, 40, 196, l, 2, GREY, TRANSPARENT);
}

static void draw(void) {
    int i;

    fb_clear(&fb, BG);
    fb_rect(&fb, 0, 0, fb.w, 100, PANEL);
    fb_text(&fb, 40, 30, "NETTEST", 3, WHITE, TRANSPARENT);
    draw_status();
    for (i = 0; i < nlines; i++) {
        fb_text(&fb, 40, BODY_Y + i * LINE_H, lines[i], 2, line_color[i], TRANSPARENT);
    }
    fb_rect(&fb, 0, 660, fb.w, 60, PANEL);
    fb_text(&fb, 40, 680, "OK ping/DNS/HTTP  GREEN receive test :5001  YELLOW send test  BACK quit",
            2, GREY, TRANSPARENT);
}

static const char *err_name(int e) {
    return e == SDK_NET_TIMEOUT ? "timeout" : e == SDK_NET_DOWN ? "network down" :
           e == SDK_NET_NOTFOUND ? "not found" : e == SDK_NET_REFUSED ? "refused" : "error";
}

static void basic_tests(void) {
    struct sdk_net_info i;
    char a[16], line[82];
    u32 ip, t0;
    int r, n, s, total = 0, k;

    nlines = 0;
    if (sdk_net_info(&i) < 0 || i.state != SDK_NET_UP) {
        add(YELLOW, "not connected");
        return;
    }
    for (k = 0; k < 4; k++) {
        r = sdk_net_ping(i.gateway, 1000);
        if (r >= 0) {
            add(WHITE, "ping router %s: %d ms", sdk_ip_str(i.gateway, a), r);
        } else {
            add(RED, "ping router %s: %s", sdk_ip_str(i.gateway, a), err_name(r));
        }
    }
    draw();
    t0 = ub_get_timer(0);
    r = sdk_net_resolve("example.com", &ip);
    if (r < 0) {
        add(RED, "DNS example.com: %s", err_name(r));
        return;
    }
    add(WHITE, "DNS example.com = %s (%u ms)", sdk_ip_str(ip, a), (unsigned) ub_get_timer(t0));
    draw();
    t0 = ub_get_timer(0);
    s = sdk_tcp_connect(ip, 80, 5000);
    if (s < 0) {
        add(RED, "TCP connect: %s", err_name(s));
        return;
    }
    add(WHITE, "TCP connected in %u ms", (unsigned) ub_get_timer(t0));
    snprintf(buf, sizeof(buf), "GET / HTTP/1.0\r\nHost: example.com\r\nUser-Agent: ncapps\r\n\r\n");
    sdk_net_send(s, buf, strlen(buf));
    line[0] = 0;
    while ((n = sdk_net_recv(s, buf, sizeof(buf) - 1, 5000)) > 0) {
        if (!total) {
            char *e;

            buf[n] = 0;
            e = strchr(buf, '\r');
            if (e) {
                *e = 0;
            }
            snprintf(line, sizeof(line), "%s", buf);
        }
        total += n;
    }
    sdk_net_close(s);
    add(line[0] ? GREEN : RED, "HTTP: %s", line[0] ? line : "no answer");
    add(WHITE, "%d bytes in %u ms%s", total, (unsigned) ub_get_timer(t0),
        n < 0 ? " (ended by timeout)" : "");
}

static void receive_test(void) {
    struct sdk_key k;
    char a[16];
    int ls, s, n, port;
    u32 t0, total = 0, last = 0, tlast;

    nlines = 0;
    ls = sdk_tcp_listen(5001, 1);
    if (ls < 0) {
        add(RED, "listen :5001: %s", err_name(ls));
        return;
    }
    add(WHITE, "listening on port 5001: send data from the PC (BACK stops)");
    draw();
    for (;;) {
        s = sdk_tcp_accept(ls, &peer_ip, &port, 500);
        if (s >= 0) {
            break;
        }
        if (s != SDK_NET_TIMEOUT || (sdk_key_poll(&k) && k.btn == BTN_BACK)) {
            sdk_net_close(ls);
            add(YELLOW, "stopped");
            return;
        }
    }
    sdk_net_close(ls);
    add(WHITE, "connection from %s:%d", sdk_ip_str(peer_ip, a), port);
    draw();
    t0 = tlast = ub_get_timer(0);
    while ((n = sdk_net_recv(s, buf, sizeof(buf), 5000)) > 0) {
        total += n;
        if (ub_get_timer(tlast) >= 1000) {
            u32 ms = ub_get_timer(tlast);

            add(WHITE, "%u KB  %u KB/s", (unsigned) (total >> 10),
                (unsigned) ((total - last) / ms * 1000 / 1024));
            draw();
            last = total;
            tlast = ub_get_timer(0);
        }
    }
    sdk_net_close(s);
    {
        u32 ms = ub_get_timer(t0);

        add(GREEN, "received %u KB in %u ms = %u KB/s", (unsigned) (total >> 10), (unsigned) ms,
            ms ? (unsigned) ((u32) ((unsigned long long) total * 1000 / ms) >> 10) : 0);
    }
}

static void send_test(void) {
    char a[16];
    u32 t0, total = 0;
    int s, i;

    nlines = 0;
    if (!peer_ip) {
        add(YELLOW, "run the receive test first (it learns the PC's address)");
        return;
    }
    s = sdk_tcp_connect(peer_ip, 5002, 3000);
    if (s < 0) {
        add(RED, "connect %s:5002: %s (run tcprecv.py on the PC)", sdk_ip_str(peer_ip, a),
            err_name(s));
        return;
    }
    for (i = 0; i < (int) sizeof(buf); i++) {
        buf[i] = 'A' + i % 26;
    }
    t0 = ub_get_timer(0);
    while (total < 8u << 20) {
        int n = sdk_net_send(s, buf, sizeof(buf));

        if (n < 0) {
            add(RED, "send: %s", err_name(n));
            break;
        }
        total += n;
    }
    sdk_net_close(s);
    {
        u32 ms = ub_get_timer(t0);

        add(GREEN, "sent %u KB in %u ms = %u KB/s", (unsigned) (total >> 10), (unsigned) ms,
            ms ? (unsigned) ((u32) ((unsigned long long) total * 1000 / ms) >> 10) : 0);
    }
}

int main(int argc, char *argv[]) {
    struct sdk_key k;
    u32 t = 0;

    (void) argc;
    (void) argv;
    if (osd_setup(&fb) < 0) {
        printf("nettest: display not running\n");
        return 1;
    }
    draw();
    for (;;) {
        if (!sdk_key_poll(&k) || k.btn == BTN_NONE) {
            if (ub_get_timer(t) >= 1000) {
                draw_status();
                t = ub_get_timer(0);
            }
            sdk_idle(5000);
            continue;
        }
        if (k.repeat) {
            continue;
        }
        if (k.btn == BTN_OK) {
            nlines = 0;
            add(YELLOW, "running...");
            draw();
            basic_tests();
            draw();
        } else if (k.btn == BTN_GREEN) {
            receive_test();
            draw();
        } else if (k.btn == BTN_YELLOW) {
            nlines = 0;
            add(YELLOW, "sending 8 MB...");
            draw();
            send_test();
            draw();
        } else if (k.btn == BTN_BACK || k.btn == BTN_POWER || k.btn == BTN_HOME) {
            break;
        }
    }
    fb_clear(&fb, TRANSPARENT);
    return 0;
}
