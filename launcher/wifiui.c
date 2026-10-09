/*
 * Launcher: the WiFi page (SETTINGS > WiFi) and an on-screen keyboard for
 * the passphrase. The network itself is the system's service
 * (net/netsvc.c); this page shows its state, scans, and saves the chosen
 * network to /NCAPPS/WIFI.TXT.
 *
 * Remote: UP/DOWN pick a network, OK connect (keyboard for the
 * passphrase), GREEN scan, YELLOW WiFi on / off, BACK close (serial
 * console: w/s, Enter, g scan, y on / off, Esc).
 * Keyboard: arrows + OK type, 0-9 type digits, RED delete, YELLOW
 * shift, BLUE show / hide, GREEN done, BACK cancel. On the serial console
 * just type (Enter = done, Esc = cancel).
 */
#include "sdk.h"
#include "system.h"
#include "netsvc.h"
#include "wifiui.h"

#define BG              RGB(12, 18, 40)         /* as launcher.c */
#define PANEL           RGB(24, 34, 72)
#define HILITE          RGB(60, 110, 200)
#define X0              60
#define NETS_Y          330
#define NET_ROW         36
#define NET_ROWS        8
#define MAX_APS         32

static struct net_ap aps[MAX_APS];
static int naps = -1;                           /* -1 = not scanned yet */

static const char *state_name(int s) {
    static const char *n[] = {
        "Off", "Starting", "No network chosen", "Connecting", "Getting an address",
        "Connected", "Not connected", "Scanning",
    };

    return s >= 0 && s <= SDK_NET_SCANNING ? n[s] : "?";
}

static const char *sec_name(int s) {
    return s == NET_SEC_OPEN ? "open" : s == NET_SEC_WEP_WPA1 ? "WEP/WPA1" :
           s == NET_SEC_WPA2_PSK ? "WPA2" : "WPA3/802.1X";
}

static const char *signal_word(int rssi) {
    return rssi >= -55 ? "excellent" : rssi >= -67 ? "good" : rssi >= -75 ? "fair" : "weak";
}

/* One line for the launcher header / settings page; 1 = it changed */
int wifi_status_line(char *buf, int n) {
    static char last[80];
    struct sdk_net_info i;
    char a[16];

    if (!system_running() || sdk_net_info(&i) < 0 || i.state == SDK_NET_OFF) {
        buf[0] = 0;
    } else if (i.state == SDK_NET_UP) {
        snprintf(buf, n, "WiFi %s", sdk_ip_str(i.ip, a));
    } else {
        snprintf(buf, n, "WiFi: %s", state_name(i.state));
    }
    if (strcmp(buf, last)) {
        snprintf(last, sizeof(last), "%s", buf);
        return 1;
    }
    return 0;
}

/* ---- on-screen keyboard ---- */

#define KEY_W           64
#define KEY_H           56
#define KEYS_X          ((1280 - 14 * KEY_W) / 2)
#define KEYS_Y          300

static const char *const pages[3][3] = {
    { "abcdefghijklm", "nopqrstuvwxyz", "0123456789.-_" },
    { "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789.-_" },
    { "!@#$%^&*()-_=+", "[]{}<>/\\|~`;:?", "'\",.0123456789" },
};
/* bottom row: shift (cols 0-2), symbols (3-5), space (6-9), delete (10-11), done (12-13) */
static const char *const bottom[5] = { "Shift", "#+=", "Space", "Del", "Done" };
static const int bottom_x[6] = { 0, 3, 6, 10, 12, 14 };

static int bottom_index(int col) {
    int b;

    for (b = 0; b < 4 && col >= bottom_x[b + 1]; b++) {
    }
    return b;
}

static void osk_draw(struct fb *fb, const char *title, const char *buf, int page, int row,
                     int col, int hide) {
    char shown[72];
    int r, c, n = strlen(buf);

    fb_clear(fb, BG);
    fb_rect(fb, 0, 0, fb->w, 100, PANEL);
    fb_text(fb, X0, 30, "PASSPHRASE", 3, WHITE, TRANSPARENT);
    fb_text(fb, X0, 130, title, 2, GREY, TRANSPARENT);
    fb_rect(fb, X0 - 10, 180, fb->w - 2 * X0 + 20, 60, PANEL);
    for (c = 0; c < n && c < (int) sizeof(shown) - 2; c++) {
        shown[c] = hide ? '*' : buf[c];
    }
    shown[c++] = '_';
    shown[c] = 0;
    fb_text(fb, X0, 194, shown, 2, WHITE, TRANSPARENT);
    {
        char cnt[32];

        snprintf(cnt, sizeof(cnt), "%d characters%s", n, n && n < 8 ? " (8-63 needed)" : "");
        fb_text(fb, X0, 250, cnt, 2, n && n < 8 ? YELLOW : GREY, TRANSPARENT);
    }
    for (r = 0; r < 3; r++) {
        const char *keys = pages[page][r];

        for (c = 0; keys[c]; c++) {
            char k[2] = { keys[c], 0 };
            int x = KEYS_X + c * KEY_W, y = KEYS_Y + r * KEY_H;

            fb_rect(fb, x + 2, y + 2, KEY_W - 4, KEY_H - 4, r == row && c == col ? HILITE : PANEL);
            fb_text(fb, x + 24, y + 12, k, 2, WHITE, TRANSPARENT);
        }
    }
    for (c = 0; c < 5; c++) {
        int x = KEYS_X + bottom_x[c] * KEY_W, w = (bottom_x[c + 1] - bottom_x[c]) * KEY_W;
        int y = KEYS_Y + 3 * KEY_H, on = row == 3 && bottom_index(col) == c;

        fb_rect(fb, x + 2, y + 2, w - 4, KEY_H - 4, on ? HILITE : PANEL);
        fb_text(fb, x + (w - (int) strlen(bottom[c]) * 16) / 2, y + 12,
                c == 0 && page == 1 ? "shift" : c == 1 && page == 2 ? "abc" : bottom[c], 2,
                c == 0 && page == 1 ? YELLOW : WHITE, TRANSPARENT);
    }
    fb_rect(fb, 0, 660, fb->w, 60, PANEL);
    fb_text(fb, X0, 670, "OK type  0-9 digits  RED delete  YELLOW shift  BLUE show/hide", 2, GREY,
            TRANSPARENT);
    fb_text(fb, X0, 692, "GREEN done  BACK cancel  (serial: just type, Enter done)", 2, GREY,
            TRANSPARENT);
}

/* Edit buf (max chars, NUL included); 1 = done, 0 = cancelled */
static int osk_input(struct fb *fb, const char *title, char *buf, int max) {
    struct sdk_key k;
    int page = 0, row = 0, col = 0, hide = 0, redraw = 1, n = strlen(buf);

    for (;;) {
        int type = 0;

        if (redraw) {
            osk_draw(fb, title, buf, page, row, col, hide);
            redraw = 0;
        }
        if (!sdk_key_poll(&k)) {
            sdk_idle(2000);
            continue;
        }
        redraw = 1;
        if (!k.remote && k.ch) {                /* serial console: plain typing */
            if (k.ch == '\r' || k.ch == '\n') {
                return 1;
            } else if (k.ch == 8 || k.ch == 127) {
                if (n) {
                    buf[--n] = 0;
                }
            } else if (k.ch >= 32 && k.ch < 127) {
                type = k.ch;
            } else if (k.btn == BTN_BACK) {
                return 0;
            }
        } else if (k.btn == BTN_UP) {
            row = (row + 3) % 4;
        } else if (k.btn == BTN_DOWN) {
            row = (row + 1) % 4;
        } else if (k.btn == BTN_LEFT) {
            col = row == 3 ? bottom_x[(bottom_index(col) + 4) % 5] : col - 1;
        } else if (k.btn == BTN_RIGHT) {
            col = row == 3 ? bottom_x[(bottom_index(col) + 1) % 5] : col + 1;
        } else if (k.btn >= BTN_0 && k.btn <= BTN_9 && !k.repeat) {
            type = '0' + (k.btn - BTN_0);
        } else if (k.btn == BTN_RED) {
            if (n) {
                buf[--n] = 0;
            }
        } else if (k.btn == BTN_YELLOW && !k.repeat) {
            page = page == 1 ? 0 : 1;
        } else if (k.btn == BTN_BLUE && !k.repeat) {
            hide = !hide;
        } else if (k.btn == BTN_GREEN && !k.repeat) {
            return 1;
        } else if (k.btn == BTN_BACK && !k.repeat) {
            return 0;
        } else if (k.btn == BTN_OK) {
            if (row < 3) {
                type = pages[page][row][col];
            } else {
                switch (bottom_index(col)) {
                case 0:
                    page = page == 1 ? 0 : 1;
                    break;
                case 1:
                    page = page == 2 ? 0 : 2;
                    break;
                case 2:
                    type = ' ';
                    break;
                case 3:
                    if (n) {
                        buf[--n] = 0;
                    }
                    break;
                default:
                    if (!k.repeat) {
                        return 1;
                    }
                }
            }
        } else {
            redraw = 0;
        }
        if (row < 3) {
            int len = strlen(pages[page][row]);

            col = col < 0 ? len - 1 : col >= len ? 0 : col;
        } else {
            col = col < 0 ? 0 : col > 13 ? 13 : col;
        }
        if (type && n < max - 1) {
            buf[n++] = type;
            buf[n] = 0;
            if (page == 1 && row < 2 && k.remote) {
                page = 0;                       /* shift: one capital */
            }
        }
    }
}

/* ---- the page ---- */

static void draw_status(struct fb *fb) {
    struct sdk_net_info i;
    char line[128], a[16], b[16], c[16];
    u16 col;

    fb_rect(fb, 0, 110, fb->w, 200, BG);
    sdk_net_info(&i);
    col = i.state == SDK_NET_UP ? GREEN : i.state == SDK_NET_FAILED ? RED :
          i.state == SDK_NET_OFF || i.state == SDK_NET_NO_CONFIG ? GREY : YELLOW;
    if (i.ssid[0] && i.state != SDK_NET_OFF && i.state != SDK_NET_NO_CONFIG) {
        snprintf(line, sizeof(line), "%s  \"%s\"", state_name(i.state), i.ssid);
    } else {
        snprintf(line, sizeof(line), "%s", state_name(i.state));
    }
    fb_text(fb, X0, 124, line, 2, col, TRANSPARENT);
    if (i.state == SDK_NET_UP) {
        snprintf(line, sizeof(line), "Address %s   router %s   DNS %s", sdk_ip_str(i.ip, a),
                 sdk_ip_str(i.gateway, b), sdk_ip_str(i.dns, c));
        fb_text(fb, X0, 160, line, 2, WHITE, TRANSPARENT);
    }
    if (i.state == SDK_NET_UP || i.state == SDK_NET_DHCP) {
        snprintf(line, sizeof(line), "Signal %d dBm (%s)   channel %d   host name ncwifi", i.rssi,
                 signal_word(i.rssi), i.channel);
        fb_text(fb, X0, 196, line, 2, WHITE, TRANSPARENT);
    }
    snprintf(line, sizeof(line), "MAC %02x:%02x:%02x:%02x:%02x:%02x   frames in %u out %u   %s",
             i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5], (unsigned) i.rx_frames,
             (unsigned) i.tx_frames, i.message);
    line[78] = 0;
    fb_text(fb, X0, 236, line, 2, GREY, TRANSPARENT);
}

static void draw_nets(struct fb *fb, int sel, int top) {
    char line[96], cur[33];
    int r, on, has;

    net_config(cur, sizeof(cur), &on, &has);
    fb_rect(fb, 0, NETS_Y - 40, fb->w, NET_ROWS * NET_ROW + 50, BG);
    fb_text(fb, X0, NETS_Y - 36, naps < 0 ? "Networks: GREEN to look for them" :
            naps == 0 ? "Networks: none found (GREEN to look again)" : "Networks", 2,
            GREY, TRANSPARENT);
    for (r = 0; r < NET_ROWS && top + r < naps; r++) {
        const struct net_ap *a = &aps[top + r];
        int y = NETS_Y + r * NET_ROW;

        if (top + r == sel) {
            fb_rect(fb, X0 - 10, y - 4, fb->w - 2 * X0 + 20, NET_ROW - 2, HILITE);
        }
        snprintf(line, sizeof(line), "%-32.32s %4d dBm  ch %-2d  %s%s", a->ssid, a->rssi,
                 a->channel, sec_name(a->security), strcmp(a->ssid, cur) ? "" : "  (saved)");
        fb_text(fb, X0, y, line, 2, a->security == NET_SEC_WPA2_PSK ? WHITE : GREY,
                TRANSPARENT);
    }
}

static void draw_page(struct fb *fb, int sel, int top) {
    int on, has;
    char cur[33];

    net_config(cur, sizeof(cur), &on, &has);
    fb_clear(fb, BG);
    fb_rect(fb, 0, 0, fb->w, 100, PANEL);
    fb_text(fb, X0, 30, "WIFI", 3, WHITE, TRANSPARENT);
    fb_text(fb, X0 + 120, 44, on ? "on" : "switched off", 2, on ? GREEN : GREY, TRANSPARENT);
    draw_status(fb);
    draw_nets(fb, sel, top);
    fb_rect(fb, 0, 660, fb->w, 60, PANEL);
    fb_text(fb, X0, 680, "UP/DOWN select  OK connect  GREEN scan  YELLOW on/off  BACK close", 2,
            GREY, TRANSPARENT);
}

static void note(struct fb *fb, const char *msg, u16 color) {
    fb_rect(fb, 0, 300, fb->w, 120, PANEL);
    fb_text(fb, (fb->w - (int) strlen(msg) * 16) / 2, 344, msg, 2, color, TRANSPARENT);
}

static void pause_ms(u32 ms) {
    u32 t = ub_get_timer(0);

    while (ub_get_timer(t) < ms) {
        sdk_idle(10000);
    }
}

static void do_scan(struct fb *fb) {
    note(fb, "Looking for networks (the connection pauses for a moment)...", WHITE);
    naps = net_scan(aps, MAX_APS);
    if (naps < 0) {
        naps = 0;
    }
}

static void connect_to(struct fb *fb, const struct net_ap *a) {
    static char pass[64];
    char cur[33], title[80];
    int on, has;

    if (a->security != NET_SEC_WPA2_PSK) {
        note(fb, a->security == NET_SEC_OPEN ? "Open networks are not supported yet" :
             "Only WPA2-PSK (password) networks are supported", YELLOW);
        pause_ms(2500);
        return;
    }
    net_config(cur, sizeof(cur), &on, &has);
    has = has && !strcmp(cur, a->ssid);         /* this one is saved with a passphrase */
    pass[0] = 0;
    snprintf(title, sizeof(title), "Network \"%.32s\"%s", a->ssid,
             has ? "  (empty: keep the saved one)" : "");
    for (;;) {
        if (!osk_input(fb, title, pass, sizeof(pass))) {
            return;
        }
        if (strlen(pass) >= 8 || (has && !pass[0])) {
            break;
        }
    }
    if (net_configure(a->ssid, pass[0] ? pass : 0, 1) == 0) {
        note(fb, "Saved. Connecting...", GREEN);
    } else {
        note(fb, "Connecting (NOT saved: /NCAPPS/WIFI.TXT missing, run stage.sh)", YELLOW);
    }
    memset(pass, 0, sizeof(pass));
    pause_ms(1500);
}

void wifi_page(struct fb *fb) {
    struct sdk_key k;
    struct sdk_net_info i;
    int sel = 0, top = 0, redraw = 1;
    u32 t = 0;

    if (!system_running() || sdk_net_info(&i) < 0) {
        fb_clear(fb, BG);
        note(fb, "WiFi needs Multitasking: SETTINGS > Multitasking ON, restart", YELLOW);
        while (!sdk_key_poll(&k) || k.repeat) {
            sdk_idle(10000);
        }
        return;
    }
    if (i.state == SDK_NET_OFF) {
        fb_clear(fb, BG);
        note(fb, i.message[0] ? i.message : "No WiFi on this box", YELLOW);
        while (!sdk_key_poll(&k) || k.repeat) {
            sdk_idle(10000);
        }
        return;
    }
    if (naps < 0 && i.state != SDK_NET_UP && i.state != SDK_NET_STARTING) {
        draw_page(fb, sel, top);
        do_scan(fb);
    }
    for (;;) {
        if (redraw) {
            draw_page(fb, sel, top);
            redraw = 0;
            t = ub_get_timer(0);
        } else if (ub_get_timer(t) >= 1000) {
            draw_status(fb);
            t = ub_get_timer(0);
        }
        if (!sdk_key_poll(&k)) {
            sdk_idle(5000);
            continue;
        }
        if (!k.remote && (k.ch == 'g' || k.ch == 'y')) {
            k.btn = k.ch == 'g' ? BTN_GREEN : BTN_YELLOW;     /* serial console */
        }
        if (k.btn == BTN_UP && naps > 0) {
            sel = (sel + naps - 1) % naps;
        } else if (k.btn == BTN_DOWN && naps > 0) {
            sel = (sel + 1) % naps;
        } else if (k.btn == BTN_GREEN && !k.repeat) {
            do_scan(fb);
            sel = top = 0;
        } else if (k.btn == BTN_YELLOW && !k.repeat) {
            char cur[33];
            int on, has;

            net_config(cur, sizeof(cur), &on, &has);
            net_configure(0, 0, !on);
        } else if (k.btn == BTN_OK && !k.repeat && naps > 0) {
            connect_to(fb, &aps[sel]);
        } else if (k.btn == BTN_BACK || k.btn == BTN_MENU || k.btn == BTN_HOME) {
            return;
        } else {
            continue;
        }
        if (sel < top) {
            top = sel;
        } else if (sel >= top + NET_ROWS) {
            top = sel - NET_ROWS + 1;
        }
        redraw = 1;
    }
}
