/*
 * BriMod: settings and status for the ESP32-C3 bridge on the satellite box
 * (sdk/brimod.h, esp32c3/bridge/bridge.ino).
 *
 *   Status          what the bridge found, clock, climate, FM, events
 *   WiFi            scan + connect (on-screen keyboard for the password),
 *                   type a hidden network's name, forget
 *   FM transmitter  every SI4713 setting: frequency, power, antenna,
 *                   stereo / mono, pre-emphasis, deviations, line input,
 *                   limiter, compressor, RDS (name, RadioText, PI, PTY, TP,
 *                   TA, music / speech, AF, clock time, share), noise scan,
 *                   defaults; live input level meter. The bridge keeps
 *                   everything over power-off.
 *   Clock           time zone, NTP sync, set by hand (also sets the DS1302)
 *
 * Remote, front buttons or serial: UP / DOWN select, LEFT / RIGHT change,
 * OK open / edit, BACK (EXIT, MENU, Esc) back. The front panel shows the
 * temperature. Passwords are never printed on the serial console.
 *
 *   sh apps/brimod/build.sh -> apps/brimod/BRIMOD.BIN
 */
#include "sdk.h"

#define BG          RGB(12, 18, 40)
#define PANEL       RGB(24, 34, 72)
#define HILITE      RGB(60, 110, 200)
#define HEAD        RGB(120, 200, 255)
#define VALUE       RGB(255, 225, 140)
#define DIM         RGB(70, 80, 110)
#define LX          60
#define VX          580
#define ROW_Y       116
#define ROW_H       36
#define ROWS        12
#define HELP_Y      560
#define LIVE_Y      592
#define STAT_Y      624

static struct fb fb;
static int have_screen;
static char msg[112];
static u16 msg_col = GREY;

/* ---- drawing ---- */

static void text(int x, int y, const char *s, int scale, u16 col) {
    fb_text(&fb, x, y, s, scale, col, TRANSPARENT);
}

static void frame(const char *title, const char *keys) {
    fb_clear(&fb, BG);
    fb_rect(&fb, 0, 0, fb.w, 96, PANEL);
    text(LX, 30, title, 3, WHITE);
    fb_rect(&fb, 0, 660, fb.w, 60, PANEL);
    text(LX, 680, keys, 2, GREY);
}

static void line_at(int y, const char *s, u16 col) {
    fb_rect(&fb, 0, y - 4, fb.w, 32, BG);
    text(LX, y, s, 2, col);
}

static void show_msg(void) {
    line_at(STAT_Y, msg, msg_col);
}

/* A box in the middle while a slow bridge command runs */
static void busy(const char *s) {
    int w = (int) strlen(s) * 16 + 80;

    fb_rect(&fb, (fb.w - w) / 2, 300, w, 100, PANEL);
    fb_rect(&fb, (fb.w - w) / 2, 300, w, 4, HILITE);
    text((fb.w - w) / 2 + 40, 334, s, 2, WHITE);
}

static const char *err_text(int r) {
    return r == BRIMOD_ETIMEOUT ? "no answer in time" : r == BRIMOD_EBUSY ? "bridge busy" :
           r == BRIMOD_ENODEV ? "no bridge" : r == BRIMOD_EPROTO ? "not accepted" :
           r == BRIMOD_EFAIL ? "failed" : "I2C error";
}

/* 0.01 units -> "-1.05" */
static char *c100(char *buf, int v) {
    sprintf(buf, "%s%d.%02d", v < 0 ? "-" : "", (v < 0 ? -v : v) / 100, (v < 0 ? -v : v) % 100);
    return buf;
}

/* Front panel: temperature every 5 s */
static void panel_tick(int force) {
    static u32 last;
    struct brimod_climate c;
    char s[8];

    if (!force && get_timer(last) < 5000) {
        return;
    }
    last = get_timer(0);
    if ((brimod_present() & BRIMOD_HAS_AHT20) && brimod_climate(&c) == 0) {
        int t = (c.temp + (c.temp < 0 ? -50 : 50)) / 100;

        t = t < -99 ? -99 : t > 99 ? 99 : t;
        snprintf(s, sizeof(s), t < -9 ? "%d" : "%2dC", t);
        sdk_panel_show(s);
    } else {
        sdk_panel_show("br");
    }
}

/* ---- generic menu ---- */

struct menu {
    const char *title;
    int n;
    void (*row)(int i, char *label, char *value);   /* label "#..." = heading */
    int (*change)(int i, int dir, int held);        /* LEFT / RIGHT: 1 = changed */
    int (*press)(int i);                            /* OK: 1 = leave the menu */
    const char *(*help)(int i);
    void (*live)(void);                             /* every 300 ms, draws at LIVE_Y */
    void (*enter)(void);                            /* (re)load the values */
};

static int is_heading(const struct menu *m, int i) {
    char label[64] = "", value[96] = "";

    m->row(i, label, value);
    return label[0] == '#';
}

static void menu_row(const struct menu *m, int i, int sel, int top) {
    char label[64] = "", value[96] = "";
    int y = ROW_Y + (i - top) * ROW_H;

    m->row(i, label, value);
    fb_rect(&fb, LX - 14, y - 6, fb.w - 2 * (LX - 14), ROW_H, i == sel ? HILITE : BG);
    if (label[0] == '#') {
        text(LX, y, label + 1, 2, HEAD);
        return;
    }
    text(LX + 16, y, label, 2, WHITE);
    text(VX, y, value, 2, i == sel ? WHITE : VALUE);
}

static void menu_rows(const struct menu *m, int sel, int top) {
    int i;

    fb_rect(&fb, 0, ROW_Y - 8, fb.w, ROWS * ROW_H + 4, BG);
    for (i = top; i < m->n && i < top + ROWS; i++) {
        menu_row(m, i, sel, top);
    }
    if (top > 0) {
        text(fb.w - 90, ROW_Y, "more", 1, GREY);
    }
    if (top + ROWS < m->n) {
        text(fb.w - 90, ROW_Y + (ROWS - 1) * ROW_H + 8, "more", 1, GREY);
    }
}

static void menu_help(const struct menu *m, int sel) {
    line_at(HELP_Y, m->help ? m->help(sel) : "", GREY);
}

static void menu_draw(const struct menu *m, int sel, int top) {
    char title[64];

    snprintf(title, sizeof(title), "BriMod  %s", m->title);
    frame(title, "UP/DOWN select  LEFT/RIGHT change  OK open/edit  BACK back");
    menu_rows(m, sel, top);
    menu_help(m, sel);
    show_msg();
}

static int next_row(const struct menu *m, int i, int dir) {
    int k;

    for (k = 0; k < m->n; k++) {
        i = (i + dir + m->n) % m->n;
        if (!is_heading(m, i)) {
            return i;
        }
    }
    return i;
}

static void run_menu(const struct menu *m) {
    struct sdk_key k;
    int sel = next_row(m, -1, 1), top = 0, old;
    u32 live_ms = 0;

    msg[0] = 0;
    if (m->enter) {
        m->enter();
    }
    menu_draw(m, sel, top);
    for (;;) {
        if (m->live && get_timer(live_ms) >= 300) {
            live_ms = get_timer(0);
            if (!sdk_screen_off) {
                m->live();
            }
        }
        panel_tick(0);
        if (!sdk_key_poll(&k)) {
            sdk_idle(5000);
            continue;
        }
        old = sel;
        if (k.btn == BTN_UP || k.btn == BTN_DOWN) {
            sel = next_row(m, sel, k.btn == BTN_UP ? -1 : 1);
        } else if ((k.btn == BTN_LEFT || k.btn == BTN_RIGHT) && m->change) {
            if (m->change(sel, k.btn == BTN_LEFT ? -1 : 1, k.repeat)) {
                menu_row(m, sel, sel, top);
                menu_help(m, sel);
            }
            continue;
        } else if (k.btn == BTN_OK && !k.repeat && m->press) {
            if (m->press(sel)) {
                return;
            }
            if (m->enter) {
                m->enter();
            }
            menu_draw(m, sel, top);
            continue;
        } else if ((k.btn == BTN_BACK || k.btn == BTN_MENU || k.btn == BTN_HOME ||
                    k.btn == BTN_POWER) && !k.repeat) {
            return;
        } else {
            continue;
        }
        if (sel < top || sel >= top + ROWS) {
            top = sel < top ? sel : sel - ROWS + 1;
            if (top > 0 && is_heading(m, top - 1) && sel - (top - 1) < ROWS) {
                top--;                              /* keep the heading in view */
            }
            menu_rows(m, sel, top);
        } else {
            menu_row(m, old, sel, top);
            menu_row(m, sel, sel, top);
        }
        menu_help(m, sel);
    }
}

/* Pick one of n lines; returns the index or -1 */
static int choose(const char *title, char lines[][80], int n) {
    struct sdk_key k;
    int sel = 0, top = 0, i, redraw = 1;
    char t[80];

    snprintf(t, sizeof(t), "BriMod  %s", title);
    frame(t, "UP/DOWN select  OK choose  BACK cancel");
    for (;;) {
        if (redraw) {
            fb_rect(&fb, 0, ROW_Y - 8, fb.w, ROWS * ROW_H + 4, BG);
            for (i = top; i < n && i < top + ROWS; i++) {
                int y = ROW_Y + (i - top) * ROW_H;

                fb_rect(&fb, LX - 14, y - 6, fb.w - 2 * (LX - 14), ROW_H, i == sel ? HILITE : BG);
                text(LX + 16, y, lines[i], 2, WHITE);
            }
            redraw = 0;
        }
        if (!sdk_key_poll(&k)) {
            sdk_idle(5000);
            continue;
        }
        if (k.btn == BTN_UP || k.btn == BTN_DOWN) {
            sel = (sel + (k.btn == BTN_UP ? n - 1 : 1)) % n;
            top = sel < top ? sel : sel >= top + ROWS ? sel - ROWS + 1 : top;
            redraw = 1;
        } else if (k.btn == BTN_OK && !k.repeat) {
            return sel;
        } else if ((k.btn == BTN_BACK || k.btn == BTN_MENU) && !k.repeat) {
            return -1;
        }
    }
}

/* ---- on-screen keyboard ---- */

enum { KB_TEXT, KB_SECRET, KB_HEX, KB_NUMBER };

#define KB_X        128
#define KB_Y        250
#define KB_CELL     64
#define KB_COLS     16

static const char *kb_rows[4] = {
    "abcdefghijklmnop",
    "qrstuvwxyz.-_@!?",
    "0123456789#$%&*+",
    "=/:;,'\"()<>[]{}~",
};

/* bottom row: 4 symbols, then wide keys */
enum { KEY_SPACE = 256, KEY_SHIFT, KEY_DEL, KEY_DONE };
static const struct { int col, w, key; const char *name; } kb_last[] = {
    { 0, 1, '\\', "\\" }, { 1, 1, '|', "|" }, { 2, 1, '^', "^" }, { 3, 1, '`', "`" },
    { 4, 4, KEY_SPACE, "space" }, { 8, 3, KEY_SHIFT, "shift" }, { 11, 2, KEY_DEL, "del" },
    { 13, 3, KEY_DONE, "DONE" },
};
#define KB_LAST_N   ((int) (sizeof(kb_last) / sizeof(kb_last[0])))

static int kb_last_index(int col) {
    int i;

    for (i = KB_LAST_N - 1; i > 0 && kb_last[i].col > col; i--) {
    }
    return i;
}

static int kb_key(int r, int c, int shift) {
    if (r == 4) {
        return kb_last[kb_last_index(c)].key;
    }
    if (shift && kb_rows[r][c] >= 'a' && kb_rows[r][c] <= 'z') {
        return kb_rows[r][c] - 32;
    }
    return kb_rows[r][c];
}

static int kb_allowed(int key, int mode) {
    if (key >= 256) {
        return key != KEY_SPACE || mode == KB_TEXT || mode == KB_SECRET;
    }
    if (mode == KB_HEX) {
        return (key >= '0' && key <= '9') || (key >= 'a' && key <= 'f') ||
               (key >= 'A' && key <= 'F');
    }
    if (mode == KB_NUMBER) {
        return (key >= '0' && key <= '9') || key == '.';
    }
    return 1;
}

static void kb_cell(int r, int c, int sel, int shift, int mode) {
    int x = KB_X + c * KB_CELL, y = KB_Y + r * KB_CELL, w = KB_CELL;
    int key = kb_key(r, c, shift);
    u16 col = kb_allowed(key, mode) ? WHITE : DIM;
    char s[2] = { (char) key, 0 };

    if (r == 4) {
        int i = kb_last_index(c);

        x = KB_X + kb_last[i].col * KB_CELL;
        w = kb_last[i].w * KB_CELL;
        fb_rect(&fb, x + 2, y + 2, w - 4, KB_CELL - 4, sel ? HILITE : PANEL);
        if (key == KEY_SHIFT && shift) {
            col = YELLOW;
        }
        if (key >= 256) {
            text(x + (w - (int) strlen(kb_last[i].name) * 16) / 2, y + 16, kb_last[i].name, 2, col);
            return;
        }
    } else {
        fb_rect(&fb, x + 2, y + 2, w - 4, KB_CELL - 4, sel ? HILITE : PANEL);
    }
    text(x + 20, y + 8, s, 3, col);
}

static void kb_all(int sr, int sc, int shift, int mode) {
    int r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < KB_COLS; c++) {
            kb_cell(r, c, r == sr && c == sc, shift, mode);
        }
    }
    for (c = 0; c < KB_LAST_N; c++) {
        kb_cell(4, kb_last[c].col, sr == 4 && kb_last_index(sc) == c, shift, mode);
    }
}

static void kb_field(const char *buf, int max, int mode, int reveal, u32 typed_ms) {
    char show[72], count[16];
    int n = (int) strlen(buf), i;

    for (i = 0; i < n && i < 64; i++) {
        show[i] = buf[i];
        if (mode == KB_SECRET && !reveal && !(i == n - 1 && get_timer(typed_ms) < 1500)) {
            show[i] = '*';
        }
    }
    show[i++] = '_';
    show[i] = 0;
    fb_rect(&fb, LX - 14, 160, fb.w - 2 * (LX - 14), 56, PANEL);
    text(LX, 176, show, 2, WHITE);
    snprintf(count, sizeof(count), "%d/%d", n, max);
    fb_rect(&fb, fb.w - 160, 222, 150, 20, BG);
    text(fb.w - 150, 222, count, 1, GREY);
}

/* Edit buf (up to max characters) in place: 1 = done, 0 = cancelled */
static int keyboard(const char *title, char *buf, int max, int mode) {
    struct sdk_key k;
    int r = 0, c = 0, shift = mode == KB_HEX, reveal = 0, n;
    u32 typed_ms = 0;
    char t[96];

    snprintf(t, sizeof(t), "BriMod  %s", title);
    frame(t, "OK type  RED del  GREEN done  YELLOW shift  BLUE space  BACK cancel");
    text(LX, 128, mode == KB_SECRET ? "INFO shows / hides the text. Digits type directly." :
         "Digits on the remote type directly; a PC keyboard on serial works too.", 2, GREY);
    kb_all(r, c, shift, mode);
    kb_field(buf, max, mode, reveal, typed_ms);
    for (;;) {
        int key = 0, or_ = r, oc = c;

        if (!sdk_key_poll(&k)) {
            if (mode == KB_SECRET && typed_ms && get_timer(typed_ms) >= 1500) {
                typed_ms = 0;
                kb_field(buf, max, mode, reveal, typed_ms);    /* hide the last character */
            }
            sdk_idle(5000);
            continue;
        }
        n = (int) strlen(buf);
        if (!k.remote && k.ch) {                /* serial keyboard */
            if (k.ch == '\r' || k.ch == '\n') {
                return 1;
            } else if (k.ch == 27) {
                return 0;
            } else if (k.ch == 8 || k.ch == 127) {
                key = KEY_DEL;
            } else if (k.ch >= 32 && k.ch < 127) {
                key = k.ch;
            }
        } else if (k.btn == BTN_UP || k.btn == BTN_DOWN) {
            r = (r + (k.btn == BTN_UP ? 4 : 1)) % 5;
            if (r == 4) {
                c = kb_last[kb_last_index(c)].col;
            }
        } else if (k.btn == BTN_LEFT || k.btn == BTN_RIGHT) {
            if (r == 4) {
                int i = kb_last_index(c) + (k.btn == BTN_LEFT ? KB_LAST_N - 1 : 1);

                c = kb_last[i % KB_LAST_N].col;
            } else {
                c = (c + (k.btn == BTN_LEFT ? KB_COLS - 1 : 1)) % KB_COLS;
            }
        } else if (k.repeat && k.btn != BTN_RED) {
            continue;
        } else if (k.btn == BTN_OK) {
            key = kb_key(r, c, shift);
        } else if (k.btn >= BTN_0 && k.btn <= BTN_9) {
            key = '0' + (k.btn - BTN_0);
        } else if (k.btn == BTN_RED) {
            key = KEY_DEL;
        } else if (k.btn == BTN_GREEN) {
            key = KEY_DONE;
        } else if (k.btn == BTN_YELLOW) {
            key = KEY_SHIFT;
        } else if (k.btn == BTN_BLUE) {
            key = KEY_SPACE;
        } else if (k.btn == BTN_INFO) {
            reveal = !reveal;
            kb_field(buf, max, mode, reveal, typed_ms);
            continue;
        } else if (k.btn == BTN_BACK || k.btn == BTN_MENU) {
            return 0;
        }
        if (r != or_ || c != oc) {
            kb_cell(or_, oc, 0, shift, mode);
            kb_cell(r, c, 1, shift, mode);
            continue;
        }
        if (!key || !kb_allowed(key, mode)) {
            continue;
        }
        if (key == KEY_DONE) {
            return 1;
        } else if (key == KEY_SHIFT) {
            shift = !shift;
            kb_all(r, c, shift, mode);
            continue;
        } else if (key == KEY_DEL) {
            if (n > 0) {
                buf[n - 1] = 0;
            }
        } else if (n < max) {
            if (mode == KB_HEX && key >= 'a' && key <= 'f') {
                key -= 32;
            }
            buf[n] = (char) (key == KEY_SPACE ? ' ' : key);
            buf[n + 1] = 0;
            typed_ms = get_timer(0) | 1;
        }
        kb_field(buf, max, mode, reveal, typed_ms);
    }
}

/* "102.3" / "102.30" -> 10230 (10 kHz units), -1 = not a number */
static int parse_mhz(const char *s) {
    int ip = 0, frac = 0, digits = 0;

    if (!*s) {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        ip = ip * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (digits < 2) {
                frac = frac * 10 + (*s - '0');
                digits++;
            }
            s++;
        }
        if (digits == 1) {
            frac *= 10;
        }
    }
    return *s ? -1 : ip * 100 + frac;
}

/* ---- status page ---- */

static const char *wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *tsrc[4] = { "not set", "DS1302", "NTP", "set here" };

static void status_page(void) {
    static const char *parts[8] = { "AHT20", "BMP280", "?", "SI4713", "DS1302", "PANEL", "WIFI",
                                    "TIME" };
    struct sdk_key k;
    int events = 0, bell = 0, last_key = 0;
    u32 last = 0;

    frame("BriMod  Status", "BACK back   (front buttons and the remote show up below)");
    for (;;) {
        int b = brimod_doorbell();

        if (b > 0) {
            bell |= b;
        }
        if (get_timer(last) >= 1000 && !sdk_screen_off) {
            struct brimod_time t;
            struct brimod_climate c;
            struct brimod_adc a;
            struct brimod_fm fm;
            unsigned char up[4];
            char s[8][100], b1[16], b2[16], b3[16];
            int n = 0, p = brimod_present(), i, len, ev = brimod_events();

            last = get_timer(0);
            events |= ev > 0 ? ev : 0;
            if (p < 0) {
                snprintf(s[n++], 100, "Bridge: no answer (%s)", err_text(p));
            } else {
                len = sprintf(s[n], "Bridge firmware %d, has:", brimod_fw);
                for (i = 0; i < 8; i++) {
                    if (p & (1 << i)) {
                        len += sprintf(s[n] + len, " %s", parts[i]);
                    }
                }
                n++;
            }
            if (brimod_time(&t) == 0) {
                int tz = t.tz_min < 0 ? -t.tz_min : t.tz_min;

                snprintf(s[n++], 100, "Clock: %04d-%02d-%02d %s %02d:%02d:%02d  UTC%c%d:%02d  (%s)",
                         t.year, t.month, t.day, wdays[t.wday], t.hour, t.min, t.sec,
                         t.tz_min < 0 ? '-' : '+', tz / 60, tz % 60, tsrc[t.source & 3]);
            }
            if (brimod_climate(&c) == 0) {
                snprintf(s[n++], 100, "Climate: %s C  %s %%RH  %u.%u hPa  (BMP280 %s C)",
                         c100(b1, c.temp), c100(b2, c.humidity), c.pressure / 100,
                         c.pressure % 100 / 10, c100(b3, c.bmp_temp));
            }
            if (brimod_adc(&a) == 0 && brimod_read(BRIMOD_REG_UPTIME, up, 4) == 0) {
                snprintf(s[n++], 100, "ESP32: GPIO0 %d mV  GPIO1 %d mV  chip %s C  up %u s",
                         a.mv[0], a.mv[1], c100(b1, a.chip_temp),
                         up[0] | up[1] << 8 | up[2] << 16 | (u32) up[3] << 24);
            }
            if (brimod_fm_get(&fm) == 0) {
                snprintf(s[n++], 100, "FM: %d.%02d MHz  %d dBuV  %s  %s  RDS %s  input %d dBFS",
                         fm.freq / 100, fm.freq % 100, fm.power,
                         fm.flags & BRIMOD_FM_TX ? "ON AIR" : "off",
                         fm.flags & BRIMOD_FM_MONO ? "mono" : "stereo",
                         fm.flags & BRIMOD_FM_RDS ? "on" : "off", fm.in_level);
            }
            snprintf(s[n++], 100, "Events 0x%04x  doorbell 0x%02x  last button %s", events, bell,
                     last_key ? sdk_btn_name(last_key) : "-");
            fb_rect(&fb, 0, 110, fb.w, 540, BG);
            for (i = 0; i < n; i++) {
                text(LX, 130 + i * 44, s[i], 2, i == 0 ? HEAD : WHITE);
            }
        }
        panel_tick(0);
        if (!sdk_key_poll(&k)) {
            sdk_idle(5000);
            continue;
        }
        if (k.remote) {
            last_key = k.btn;
            last = 0;                               /* show it now */
        }
        if ((k.btn == BTN_BACK || k.btn == BTN_MENU || k.btn == BTN_POWER) && !k.repeat) {
            return;
        }
    }
}

/* ---- WiFi ---- */

static struct brimod_wifi wifi;
static u32 wifi_ms;

static void wifi_refresh(void) {
    if (brimod_wifi_status(&wifi) < 0) {
        memset(&wifi, 0, sizeof(wifi));
    }
    wifi_ms = get_timer(0);
}

static void wifi_row(int i, char *label, char *value) {
    static const char *names[] = {
        "#Network", "Scan and connect", "Other network (type its name)", "Refresh",
        "Disconnect and forget",
    };

    strcpy(label, names[i]);
    if (i == 1) {
        strcpy(value, "OK");
    }
}

static const char *wifi_help(int i) {
    static const char *h[] = {
        "", "Lists the networks the bridge sees; pick one, type the password.",
        "For a hidden network: type its name, then the password.",
        "Asks the bridge again.",
        "The bridge disconnects and forgets the network (and password).",
    };

    return h[i];
}

static void wifi_live(void) {
    char s[112];
    u16 col;

    if (get_timer(wifi_ms) >= 5000) {
        wifi_refresh();
    }
    if (wifi.connected) {
        snprintf(s, sizeof(s), "Connected to %s, IP %s, %d dBm", wifi.ssid, wifi.ip, wifi.rssi);
        col = GREEN;
    } else if (wifi.ssid[0]) {
        snprintf(s, sizeof(s), "Not connected (trying \"%s\")", wifi.ssid);
        col = YELLOW;
    } else {
        snprintf(s, sizeof(s), "Not connected, no network saved");
        col = GREY;
    }
    line_at(LIVE_Y, s, col);
}

static void wifi_join(const char *ssid, int secure) {
    char pass[64] = "", title[80], reply[64];
    volatile char *wipe = pass;
    int r, i;

    snprintf(title, sizeof(title), "Password for %s", ssid);
    if (secure && !keyboard(title, pass, 63, KB_SECRET)) {
        return;
    }
    snprintf(title, sizeof(title), "Connecting to %s ...", ssid);
    busy(title);
    r = brimod_wifi_connect(ssid, pass, reply, sizeof(reply));
    for (i = 0; i < (int) sizeof(pass); i++) {
        wipe[i] = 0;
    }
    if (r == 0) {
        snprintf(msg, sizeof(msg), "Connected: %s", reply);
        msg_col = GREEN;
        printf("brimod: WiFi connected to %s\n", ssid);
    } else {
        snprintf(msg, sizeof(msg), "Not connected: %s", r == BRIMOD_EFAIL ? reply : err_text(r));
        msg_col = RED;
        printf("brimod: WiFi connect to %s failed\n", ssid);
    }
    wifi_refresh();
}

static int wifi_press(int i) {
    if (i == 1) {
        static struct brimod_net nets[20];
        static char lines[20][80];
        int n, k;

        busy("Scanning for networks ...");
        n = brimod_wifi_scan(nets, 20);
        if (n <= 0) {
            snprintf(msg, sizeof(msg), n == 0 ? "No networks found" : "Scan failed: %s",
                     err_text(n));
            msg_col = RED;
            return 0;
        }
        for (k = 0; k < n; k++) {
            snprintf(lines[k], 80, "%-32.32s %4d dBm  ch %2d  %s", nets[k].ssid, nets[k].rssi,
                     nets[k].channel, nets[k].secure ? "secured" : "open");
        }
        k = choose("Choose a network", lines, n);
        if (k >= 0) {
            wifi_join(nets[k].ssid, nets[k].secure);
        }
    } else if (i == 2) {
        char ssid[33] = "";

        if (keyboard("Network name (SSID)", ssid, 32, KB_TEXT) && ssid[0]) {
            wifi_join(ssid, 1);
        }
    } else if (i == 3) {
        busy("Asking the bridge ...");
        wifi_refresh();
    } else if (i == 4) {
        int r = brimod_wifi_forget();

        snprintf(msg, sizeof(msg), r == 0 ? "Disconnected, network forgotten" : "Failed: %s",
                 err_text(r));
        msg_col = r == 0 ? GREEN : RED;
        wifi_refresh();
    }
    return 0;
}

static const struct menu wifi_menu = {
    "WiFi", 5, wifi_row, 0, wifi_press, wifi_help, wifi_live, wifi_refresh,
};

/* ---- FM transmitter ---- */

enum {
    H_TX, R_TX, R_FREQ, R_POWER, R_ANT, R_SCAN,
    H_AUDIO, R_STEREO, R_PREEMPH, R_ADEV, R_PDEV, R_INRANGE, R_INLEVEL, R_MUTE,
    H_DYN, R_LIMIT, R_LIMREL, R_COMP, R_CTHR, R_CGAIN, R_CATT, R_CREL,
    H_RDS, R_RDS, R_PS, R_RT, R_PI, R_PTY, R_MS, R_TP, R_TA, R_AF, R_CT, R_MIX, R_RDSDEV,
    H_MORE, R_PILOTF, R_DEFAULTS,
    FM_ROWS
};

enum {
    P_ADEV, P_PDEV, P_RDSDEV, P_LINE, P_MUTE, P_PRE, P_PILOTF, P_DYN, P_CTHR, P_CATT, P_CREL,
    P_CGAIN, P_LIMREL, P_MIX, P_MISC, P_AF, P_N
};

static const unsigned short si_props[P_N] = {
    BRIMOD_SI_AUDIO_DEV, BRIMOD_SI_PILOT_DEV, BRIMOD_SI_RDS_DEV, BRIMOD_SI_LINE_INPUT,
    BRIMOD_SI_LINE_MUTE, BRIMOD_SI_PREEMPHASIS, BRIMOD_SI_PILOT_FREQ, BRIMOD_SI_DYNAMICS,
    BRIMOD_SI_COMP_THRESH, BRIMOD_SI_COMP_ATTACK, BRIMOD_SI_COMP_RELEASE, BRIMOD_SI_COMP_GAIN,
    BRIMOD_SI_LIMIT_RELEASE, BRIMOD_SI_PS_MIX, BRIMOD_SI_PS_MISC, BRIMOD_SI_AF,
};

static const char *pty_names[32] = {
    "None", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture",
    "Science", "Varied", "Pop music", "Rock music", "Easy listening", "Light classical",
    "Serious classical", "Other music", "Weather", "Finance", "Children's", "Social affairs",
    "Religion", "Phone-in", "Travel", "Leisure", "Jazz music", "Country music",
    "National music", "Oldies music", "Folk music", "Documentary", "Alarm test", "Alarm",
};
static const char *preemph_names[3] = { "75 us (Americas)", "50 us (Europe, Asia)", "Off" };
static const char *range_names[4] = { "190 mVpk", "301 mVpk", "416 mVpk", "636 mVpk" };
static const char *mute_names[4] = { "Off", "Right", "Left", "Both" };
static const char *mix_names[7] = {
    "only between texts", "12.5 %", "25 %", "50 %", "75 %", "87.5 %", "100 %"
};
static const int crel_ms[5] = { 100, 200, 350, 525, 1000 };
static const int limrel_steps[9] = { 2000, 1000, 512, 256, 102, 51, 26, 10, 5 };

static struct brimod_fm fm;
static int si[P_N];                         /* -1 = the bridge did not answer */
static int rds_pi;
static char rds_ps[9], rds_rt[65];
static int si_found, defaults_armed;

static void fm_load(void) {
    int i;

    si_found = (brimod_present() & BRIMOD_HAS_SI4713) != 0;
    if (brimod_fm_get(&fm) < 0) {
        memset(&fm, 0, sizeof(fm));
    }
    for (i = 0; i < P_N; i++) {
        si[i] = brimod_si_get(si_props[i]);
    }
    if (brimod_rds_get(&rds_pi, rds_ps, rds_rt) < 0) {
        rds_pi = 0;
        rds_ps[0] = rds_rt[0] = 0;
    }
    defaults_armed = 0;
}

static void prop_set(int p, int v) {
    si[p] = v & 0xffff;
    brimod_si_set(si_props[p], si[p]);
}

static void fm_store(void) {
    brimod_fm_write(&fm);
}

static int misc_bit(int bit) {
    return si[P_MISC] >= 0 && (si[P_MISC] & bit) != 0;
}

/* The property a row shows, -1 = none */
static int row_prop(int i) {
    switch (i) {
    case R_PREEMPH:
        return P_PRE;
    case R_ADEV:
        return P_ADEV;
    case R_PDEV:
        return P_PDEV;
    case R_RDSDEV:
        return P_RDSDEV;
    case R_INRANGE:
    case R_INLEVEL:
        return P_LINE;
    case R_MUTE:
        return P_MUTE;
    case R_LIMIT:
    case R_COMP:
        return P_DYN;
    case R_LIMREL:
        return P_LIMREL;
    case R_CTHR:
        return P_CTHR;
    case R_CGAIN:
        return P_CGAIN;
    case R_CATT:
        return P_CATT;
    case R_CREL:
        return P_CREL;
    case R_PTY:
    case R_MS:
    case R_TP:
    case R_TA:
        return P_MISC;
    case R_AF:
        return P_AF;
    case R_MIX:
        return P_MIX;
    case R_PILOTF:
        return P_PILOTF;
    }
    return -1;
}

static char *khz(char *out, int v10hz) {
    sprintf(out, "%d.%02d kHz", v10hz / 100, v10hz % 100);
    return out;
}

static void fm_row(int i, char *label, char *value) {
    static const char *names[FM_ROWS] = {
        "#Transmitter", "Transmitter", "Frequency", "Power", "Antenna tuning",
        "Find a quiet frequency",
        "#Audio", "Sound", "Pre-emphasis", "Audio deviation", "Pilot deviation", "Input range",
        "Input full scale", "Input mute",
        "#Dynamics", "Limiter", "Limiter release", "Compressor", "Compressor threshold",
        "Compressor gain", "Compressor attack", "Compressor release",
        "#RDS", "RDS", "Station name (PS)", "RadioText", "PI code", "Programme type",
        "Content", "Traffic programme (TP)", "Traffic announcement", "Alternative frequency",
        "Send clock time (CT)", "Station name share", "RDS deviation",
        "#More", "Pilot frequency", "Restore defaults",
    };
    int line = si[P_LINE], v;

    strcpy(label, names[i]);
    if (row_prop(i) >= 0 && si[row_prop(i)] < 0) {
        strcpy(value, "? (no answer)");
        return;
    }
    switch (i) {
    case R_TX:
        strcpy(value, fm.flags & BRIMOD_FM_TX ? "ON AIR" : "off");
        break;
    case R_FREQ:
        sprintf(value, "%d.%02d MHz", fm.freq / 100, fm.freq % 100);
        break;
    case R_POWER:
        sprintf(value, "%d dBuV", fm.power);
        break;
    case R_ANT:
        if (fm.antcap) {
            sprintf(value, "%d.%02d pF", fm.antcap / 4, fm.antcap % 4 * 25);
        } else {
            strcpy(value, "automatic");
        }
        break;
    case R_SCAN:
    case R_DEFAULTS:
        strcpy(value, i == R_DEFAULTS && defaults_armed ? "press OK again" : "OK");
        break;
    case R_STEREO:
        strcpy(value, fm.flags & BRIMOD_FM_MONO ? "Mono" : "Stereo");
        break;
    case R_PREEMPH:
        strcpy(value, si[P_PRE] >= 0 && si[P_PRE] <= 2 ? preemph_names[si[P_PRE]] : "?");
        break;
    case R_ADEV:
    case R_PDEV:
    case R_RDSDEV:
        v = si[i == R_ADEV ? P_ADEV : i == R_PDEV ? P_PDEV : P_RDSDEV];
        if (v < 0) {
            strcpy(value, "?");
        } else {
            khz(value, v);
        }
        break;
    case R_INRANGE:
        strcpy(value, line < 0 ? "?" : range_names[(line >> 12) & 3]);
        break;
    case R_INLEVEL:
        if (line < 0) {
            strcpy(value, "?");
        } else {
            sprintf(value, "%d mVpk", line & 0x3ff);
        }
        break;
    case R_MUTE:
        strcpy(value, si[P_MUTE] < 0 ? "?" : mute_names[si[P_MUTE] & 3]);
        break;
    case R_LIMIT:
    case R_COMP:
        strcpy(value, si[P_DYN] < 0 ? "?" : (si[P_DYN] & (i == R_LIMIT ? 2 : 1)) ? "ON" : "off");
        break;
    case R_LIMREL:
        v = si[P_LIMREL] > 0 ? 5120 / si[P_LIMREL] : 0;
        sprintf(value, "%d.%d ms", v / 10, v % 10);
        break;
    case R_CTHR:
        sprintf(value, "%d dBFS", (short) si[P_CTHR]);
        break;
    case R_CGAIN:
        sprintf(value, "%d dB", si[P_CGAIN]);
        break;
    case R_CATT:
        v = (si[P_CATT] + 1) * 5;
        sprintf(value, "%d.%d ms", v / 10, v % 10);
        break;
    case R_CREL:
        sprintf(value, "%d ms", si[P_CREL] >= 0 && si[P_CREL] <= 4 ? crel_ms[si[P_CREL]] : 0);
        break;
    case R_RDS:
        strcpy(value, fm.flags & BRIMOD_FM_RDS ? "ON" : "off");
        break;
    case R_PS:
        sprintf(value, "\"%s\"", rds_ps);
        break;
    case R_RT:
        snprintf(value, 96, "%.34s%s", rds_rt, strlen(rds_rt) > 34 ? "..." : "");
        break;
    case R_PI:
        sprintf(value, "%04X", rds_pi);
        break;
    case R_PTY:
        v = si[P_MISC] < 0 ? 0 : (si[P_MISC] >> 5) & 31;
        sprintf(value, "%d %s", v, pty_names[v]);
        break;
    case R_MS:
        strcpy(value, misc_bit(0x0008) ? "Music" : "Speech");
        break;
    case R_TP:
    case R_TA:
        strcpy(value, misc_bit(i == R_TP ? 0x0400 : 0x0010) ? "ON" : "off");
        break;
    case R_AF:
        v = si[P_AF];
        if (v >= 0xE101 && v <= 0xE1CC) {
            v = 8750 + (v & 0xff) * 10;
            sprintf(value, "%d.%02d MHz", v / 100, v % 100);
        } else {
            strcpy(value, "none");
        }
        break;
    case R_CT:
        strcpy(value, fm.rds_opts & BRIMOD_RDS_CT ? "ON" : "off");
        break;
    case R_MIX:
        strcpy(value, si[P_MIX] >= 0 && si[P_MIX] <= 6 ? mix_names[si[P_MIX]] : "?");
        break;
    case R_PILOTF:
        sprintf(value, "%d Hz", si[P_PILOTF]);
        break;
    }
}

static const char *fm_help(int i) {
    static char buf[100];
    static const char *h[FM_ROWS] = {
        "", "Carrier on / off. The bridge keeps all settings over power-off.",
        "LEFT/RIGHT 0.05 MHz (hold: faster), OK types it. Use a free frequency.",
        "88-115 dBuV: use the lowest that reaches your radio (licence rules).",
        "Automatic tunes the antenna itself; by hand only to experiment.",
        "Measures the noise 87.5-108 MHz and lists the 5 quietest frequencies.",
        "", "Mono sends no 19 kHz pilot: less hiss when reception is weak.",
        "Must match the radios: 50 us in Thailand / Europe / Asia, 75 us in the Americas.",
        0, 0,
        "Input attenuator: the range just above the source's peak level.",
        "Source peak level that means full scale (0 dBFS): watch the meter below.",
        "Silences the left and / or right input.",
        "", "Holds peaks below full deviation (recommended on).",
        "How fast the limiter lets go after a peak.",
        "Evens out quiet and loud parts (radio-style sound).",
        "Level above which the compressor works.", "Make-up gain after the compressor.",
        "How fast the compressor reacts.", "How fast the compressor lets go.",
        "", "Station name and text on RDS radios.", "Up to 8 characters, on every RDS radio.",
        "Up to 64 characters of text (radios with RadioText).",
        "4 hex digits identifying the station; any code not used nearby.",
        "Programme type shown by some radios (European RDS list).",
        "Music / speech flag.", "Traffic flags: leave off unless you send traffic news.",
        "Traffic flags: leave off unless you send traffic news.",
        "Another frequency with the same programme; OK types it, LEFT to none.",
        "Sends the bridge's clock every minute: radios can set their clocks.",
        "How much of RDS time goes to the station name instead of the RadioText.",
        0,
        "", "19000 Hz is the standard; other values only for tests (stereo stops).",
        "Audio, RDS flags, antenna back to defaults (frequency, texts kept).",
    };

    if (h[i]) {
        return h[i];
    }
    /* deviations: show the sum, which should stay at or below 75 kHz */
    {
        int sum = (si[P_ADEV] > 0 ? si[P_ADEV] : 0) + (si[P_RDSDEV] > 0 ? si[P_RDSDEV] : 0) +
                  (fm.flags & BRIMOD_FM_MONO ? 0 : (si[P_PDEV] > 0 ? si[P_PDEV] : 0));

        snprintf(buf, sizeof(buf), "Audio + pilot + RDS = %d.%02d kHz (keep at or below 75)%s",
                 sum / 100, sum % 100, sum > 7500 ? "  TOO MUCH" : "");
    }
    return buf;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int fm_change(int i, int dir, int held) {
    int v, k = row_prop(i);

    defaults_armed = 0;
    if (k >= 0 && si[k] < 0) {
        return 0;                                   /* the bridge did not report it */
    }
    switch (i) {
    case R_TX:
        fm.flags ^= BRIMOD_FM_TX;
        break;
    case R_FREQ:
        fm.freq = clampi(fm.freq - fm.freq % 5 + dir * (held ? 20 : 5), 7600, 10800);
        break;
    case R_POWER:
        fm.power = clampi(fm.power + dir, 88, 115);
        break;
    case R_ANT:
        fm.antcap = clampi(fm.antcap + dir * (held ? 4 : 1), 0, 191);
        break;
    case R_STEREO:
        fm.flags ^= BRIMOD_FM_MONO;
        break;
    case R_RDS:
        fm.flags ^= BRIMOD_FM_RDS;
        break;
    case R_CT:
        fm.rds_opts ^= BRIMOD_RDS_CT;
        break;
    case R_PREEMPH:
        prop_set(P_PRE, (si[P_PRE] + 3 + dir) % 3);
        return 1;
    case R_ADEV:
    case R_PDEV:
    case R_RDSDEV:
        k = i == R_ADEV ? P_ADEV : i == R_PDEV ? P_PDEV : P_RDSDEV;
        prop_set(k, clampi(si[k] + dir * (held ? 100 : 25), 0, k == P_RDSDEV ? 7500 : 9000));
        return 1;
    case R_INRANGE:
        v = si[P_LINE];
        prop_set(P_LINE, (v & ~0x3000) | ((((v >> 12) + dir + 4) & 3) << 12));
        return 1;
    case R_INLEVEL:
        v = si[P_LINE];
        prop_set(P_LINE, (v & ~0x3ff) | clampi((v & 0x3ff) + dir * (held ? 50 : 10), 0, 1023));
        return 1;
    case R_MUTE:
        prop_set(P_MUTE, (si[P_MUTE] + 4 + dir) & 3);
        return 1;
    case R_LIMIT:
        prop_set(P_DYN, si[P_DYN] ^ 2);
        return 1;
    case R_COMP:
        prop_set(P_DYN, si[P_DYN] ^ 1);
        return 1;
    case R_LIMREL:
        for (k = 0; k < 8 && limrel_steps[k] > si[P_LIMREL]; k++) {
        }
        prop_set(P_LIMREL, limrel_steps[clampi(k + dir, 0, 8)]);
        return 1;
    case R_CTHR:
        prop_set(P_CTHR, clampi((short) si[P_CTHR] + dir * (held ? 5 : 1), -40, 0));
        return 1;
    case R_CGAIN:
        prop_set(P_CGAIN, clampi(si[P_CGAIN] + dir, 0, 20));
        return 1;
    case R_CATT:
        prop_set(P_CATT, clampi(si[P_CATT] + dir, 0, 9));
        return 1;
    case R_CREL:
        prop_set(P_CREL, clampi(si[P_CREL] + dir, 0, 4));
        return 1;
    case R_PTY:
        v = ((si[P_MISC] >> 5) + 32 + dir) & 31;
        prop_set(P_MISC, (si[P_MISC] & ~(31 << 5)) | (v << 5));
        return 1;
    case R_MS:
        prop_set(P_MISC, si[P_MISC] ^ 0x0008);
        return 1;
    case R_TP:
        prop_set(P_MISC, si[P_MISC] ^ 0x0400);
        return 1;
    case R_TA:
        prop_set(P_MISC, si[P_MISC] ^ 0x0010);
        return 1;
    case R_AF:
        v = si[P_AF] >= 0xE101 && si[P_AF] <= 0xE1CC ? si[P_AF] & 0xff : 0;
        if (v == 0) {                               /* from "none": start at our frequency */
            v = clampi((fm.freq - 8750) / 10, 1, 204);
        } else {
            v += dir * (held ? 10 : 1);
        }
        prop_set(P_AF, v < 1 || v > 204 ? 0xE0E0 : 0xE100 | v);
        return 1;
    case R_MIX:
        prop_set(P_MIX, clampi(si[P_MIX] + dir, 0, 6));
        return 1;
    case R_PILOTF:
        prop_set(P_PILOTF, clampi(si[P_PILOTF] + dir * (held ? 1000 : 100), 0, 19000));
        return 1;
    default:
        return 0;
    }
    fm_store();
    return 1;
}

static void rds_store(void) {
    brimod_rds_set(rds_pi, rds_ps, rds_rt);
}

static int fm_press(int i) {
    char buf[72];
    int v;

    switch (i) {
    case R_FREQ:
    case R_AF:
        v = i == R_FREQ ? fm.freq : si[P_AF] >= 0xE101 && si[P_AF] <= 0xE1CC ?
            8750 + (si[P_AF] & 0xff) * 10 : 0;
        if (v) {
            sprintf(buf, "%d.%02d", v / 100, v % 100);
        } else {
            buf[0] = 0;
        }
        if (!keyboard(i == R_FREQ ? "Frequency in MHz (76.00 - 108.00)" :
                      "Alternative frequency in MHz (empty = none)", buf, 6, KB_NUMBER)) {
            return 0;
        }
        v = parse_mhz(buf);
        if (i == R_AF && !buf[0]) {
            prop_set(P_AF, 0xE0E0);
        } else if (i == R_FREQ && v >= 7600 && v <= 10800) {
            fm.freq = v - v % 5;
            fm_store();
        } else if (i == R_AF && v >= 8760 && v <= 10790) {
            prop_set(P_AF, 0xE100 | ((v - 8750) / 10));
        } else {
            snprintf(msg, sizeof(msg), "\"%s\" is not a frequency in range", buf);
            msg_col = RED;
        }
        return 0;
    case R_SCAN: {
        static char lines[5][80];
        int f[5], nz[5], n, k;

        busy("Measuring 87.5 - 108 MHz (about 10 s) ...");
        n = brimod_fm_scan(f, nz, 5);
        if (n <= 0) {
            snprintf(msg, sizeof(msg), "Scan failed: %s", n == 0 ? "no result" : err_text(n));
            msg_col = RED;
            return 0;
        }
        for (k = 0; k < n; k++) {
            snprintf(lines[k], 80, "%3d.%02d MHz   noise %d dBuV", f[k] / 100, f[k] % 100, nz[k]);
        }
        k = choose("Quietest frequencies", lines, n);
        if (k >= 0) {
            fm.freq = f[k];
            fm_store();
        }
        return 0;
    }
    case R_PS:
        strcpy(buf, rds_ps);
        if (keyboard("Station name (up to 8 characters)", buf, 8, KB_TEXT)) {
            strcpy(rds_ps, buf);
            rds_store();
        }
        return 0;
    case R_RT:
        strcpy(buf, rds_rt);
        if (keyboard("RadioText (up to 64 characters)", buf, 64, KB_TEXT)) {
            strcpy(rds_rt, buf);
            rds_store();
        }
        return 0;
    case R_PI:
        sprintf(buf, "%04X", rds_pi);
        if (keyboard("PI code (4 hex digits)", buf, 4, KB_HEX) && buf[0]) {
            rds_pi = (int) strtoul(buf, 0, 16);
            rds_store();
        }
        return 0;
    case R_DEFAULTS:
        if (!defaults_armed) {
            defaults_armed = 1;
            snprintf(msg, sizeof(msg), "Press OK again to restore the defaults");
            msg_col = YELLOW;
            return 0;
        }
        v = brimod_fm_defaults();
        snprintf(msg, sizeof(msg), v == 0 ? "Defaults restored" : "Failed: %s", err_text(v));
        msg_col = v == 0 ? GREEN : RED;
        return 0;
    case R_TX:
    case R_STEREO:
    case R_PREEMPH:
    case R_INRANGE:
    case R_MUTE:
    case R_LIMIT:
    case R_COMP:
    case R_RDS:
    case R_PTY:
    case R_MS:
    case R_TP:
    case R_TA:
    case R_CT:
        fm_change(i, 1, 0);                         /* switches: OK works like RIGHT */
        return 0;
    default:
        snprintf(msg, sizeof(msg), "LEFT / RIGHT change this value (hold for bigger steps)");
        msg_col = GREY;
        return 0;
    }
}

static void fm_live(void) {
    struct brimod_fm now;
    char s[112];
    int lv, w, x = 900;

    if (!si_found) {
        line_at(LIVE_Y, "The bridge found no SI4713 (check wiring, RST pull-up)", RED);
        return;
    }
    if (brimod_fm_get(&now) < 0) {
        return;
    }
    if (!(now.flags & BRIMOD_FM_TX)) {
        snprintf(s, sizeof(s), "Off air. Noise at %d.%02d MHz: %d dBuV (last measured)",
                 now.freq / 100, now.freq % 100, now.noise);
        line_at(LIVE_Y, s, GREY);
        return;
    }
    snprintf(s, sizeof(s), "ON AIR  noise %d dBuV  ant %d.%02d pF  in %3d dBFS",
             now.noise, now.antcap_used / 4, now.antcap_used % 4 * 25, now.in_level);
    line_at(LIVE_Y, s, GREEN);
    /* input level meter -40..0 dBFS */
    lv = clampi(now.in_level, -40, 0);
    w = (lv + 40) * 300 / 40;
    fb_rect(&fb, x + 40, LIVE_Y, 300, 18, DIM);
    fb_rect(&fb, x + 40, LIVE_Y, w, 18, (now.in_flags & BRIMOD_IN_OVERMOD) ? RED :
            lv > -3 ? YELLOW : GREEN);
    if (now.in_flags & BRIMOD_IN_OVERMOD) {
        text(x + 40 + 8, LIVE_Y + 2, "OVERMODULATION", 1, WHITE);
    }
}

static const struct menu fm_menu = {
    "FM transmitter", FM_ROWS, fm_row, fm_change, fm_press, fm_help, fm_live, fm_load,
};

/* ---- clock ---- */

static struct brimod_time now_t;
static int ds_found;

static void clock_load(void) {
    if (brimod_time(&now_t) < 0) {
        memset(&now_t, 0, sizeof(now_t));
    }
    ds_found = (brimod_present() & BRIMOD_HAS_DS1302) != 0;
}

static void clock_row(int i, char *label, char *value) {
    static const char *names[] = {
        "#Clock", "Time zone", "Sync with internet time (NTP)", "Set date and time by hand",
    };
    int tz = now_t.tz_min < 0 ? -now_t.tz_min : now_t.tz_min;

    strcpy(label, names[i]);
    if (i == 1) {
        sprintf(value, "UTC%c%d:%02d", now_t.tz_min < 0 ? '-' : '+', tz / 60, tz % 60);
    } else if (i > 1) {
        strcpy(value, "OK");
    }
}

static const char *clock_help(int i) {
    static const char *h[] = {
        "", "LEFT/RIGHT 15 minutes. Thailand = UTC+7:00.",
        "Needs the bridge's WiFi. It also syncs by itself when WiFi connects.",
        "Sets the bridge clock and the DS1302 (which keeps time without power).",
    };

    return h[i];
}

static void clock_live(void) {
    struct brimod_time t;
    char s[112];

    if (brimod_time(&t) < 0) {
        return;
    }
    if (t.source == BRIMOD_TIME_NONE) {
        snprintf(s, sizeof(s), "Clock not set   DS1302 %s", ds_found ? "found" : "not found");
    } else {
        snprintf(s, sizeof(s), "%04d-%02d-%02d %s %02d:%02d:%02d   from %s   DS1302 %s",
                 t.year, t.month, t.day, wdays[t.wday], t.hour, t.min, t.sec, tsrc[t.source & 3],
                 ds_found ? "found" : "not found");
    }
    line_at(LIVE_Y, s, t.source == BRIMOD_TIME_NONE ? YELLOW : WHITE);
}

static int clock_change(int i, int dir, int held) {
    if (i != 1) {
        return 0;
    }
    now_t.tz_min = clampi(now_t.tz_min + dir * (held ? 60 : 15), -720, 840);
    brimod_set_tz(now_t.tz_min);
    return 1;
}

static int month_days(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m - 1];
}

/* Year, month, day, hour, minute: LEFT / RIGHT pick, UP / DOWN change */
static void set_time_by_hand(void) {
    static const char *names[5] = { "year", "month", "day", "hour", "minute" };
    static const int xs[5] = { 0, 5, 8, 11, 14 }, ws[5] = { 4, 2, 2, 2, 2 };
    struct brimod_time t;
    struct sdk_key k;
    int v[5], f = 3, redraw = 1;

    if (brimod_time(&t) < 0 || t.source == BRIMOD_TIME_NONE) {
        t.year = 2026;
        t.month = t.day = 1;
        t.hour = t.min = 0;
    }
    v[0] = t.year;
    v[1] = t.month;
    v[2] = t.day;
    v[3] = t.hour;
    v[4] = t.min;
    frame("BriMod  Set date and time", "LEFT/RIGHT field  UP/DOWN change  OK save  BACK cancel");
    for (;;) {
        if (redraw) {
            char s[24];

            snprintf(s, sizeof(s), "%04d-%02d-%02d %02d:%02d", v[0], v[1], v[2], v[3], v[4]);
            fb_rect(&fb, 0, 200, fb.w, 200, BG);
            fb_rect(&fb, 200 + xs[f] * 48 - 6, 236, ws[f] * 48 + 12, 108, HILITE);
            text(200, 250, s, 6, WHITE);
            snprintf(s, sizeof(s), "changing: %s", names[f]);
            text(200, 370, s, 2, GREY);
            redraw = 0;
        }
        if (!sdk_key_poll(&k)) {
            sdk_idle(5000);
            continue;
        }
        if (k.btn == BTN_LEFT || k.btn == BTN_RIGHT) {
            f = (f + (k.btn == BTN_LEFT ? 4 : 1)) % 5;
        } else if (k.btn == BTN_UP || k.btn == BTN_DOWN) {
            int d = k.btn == BTN_UP ? 1 : -1;
            static const int lo[5] = { 2000, 1, 1, 0, 0 }, hi[5] = { 2099, 12, 31, 23, 59 };

            v[f] += d;
            v[f] = v[f] < lo[f] ? hi[f] : v[f] > hi[f] ? lo[f] : v[f];
        } else if (k.btn == BTN_OK && !k.repeat) {
            u32 utc;
            int r;

            v[2] = clampi(v[2], 1, month_days(v[0], v[1]));
            utc = brimod_make_time(v[0], v[1], v[2], v[3], v[4], 0, now_t.tz_min);
            r = brimod_set_time(utc);
            if (r == 0) {
                snprintf(msg, sizeof(msg), "Clock set to %04d-%02d-%02d %02d:%02d", v[0], v[1],
                         v[2], v[3], v[4]);
            } else {
                snprintf(msg, sizeof(msg), "Failed: %s", err_text(r));
            }
            msg_col = r == 0 ? GREEN : RED;
            return;
        } else if ((k.btn == BTN_BACK || k.btn == BTN_MENU) && !k.repeat) {
            return;
        } else {
            continue;
        }
        redraw = 1;
    }
}

static int clock_press(int i) {
    char reply[64];
    int r;

    if (i == 1) {
        clock_change(1, 1, 0);
    } else if (i == 2) {
        busy("Asking the internet time ...");
        r = brimod_ntp_sync(reply, sizeof(reply));
        snprintf(msg, sizeof(msg), r == 0 ? "Clock synced" : "NTP failed: %s",
                 r == BRIMOD_EFAIL ? reply : err_text(r));
        msg_col = r == 0 ? GREEN : RED;
    } else if (i == 3) {
        set_time_by_hand();
    }
    return 0;
}

static const struct menu clock_menu = {
    "Clock", 4, clock_row, clock_change, clock_press, clock_help, clock_live, clock_load,
};

/* ---- main page ---- */

static int restart_armed;

static void main_row(int i, char *label, char *value) {
    static const char *names[] = {
        "#BriMod bridge", "Status", "WiFi", "FM transmitter (SI4713)", "Clock",
        "Restart the bridge",
    };
    struct brimod_fm f;
    struct brimod_time t;

    strcpy(label, names[i]);
    switch (i) {
    case 2:
        if (wifi.connected) {
            snprintf(value, 96, "%.30s", wifi.ssid);
        } else {
            strcpy(value, "not connected");
        }
        break;
    case 3:
        if (!(brimod_present() & BRIMOD_HAS_SI4713)) {
            strcpy(value, "not found");
        } else if (brimod_fm_get(&f) == 0) {
            sprintf(value, "%d.%02d MHz %s", f.freq / 100, f.freq % 100,
                    f.flags & BRIMOD_FM_TX ? "ON AIR" : "off");
        }
        break;
    case 4:
        if (brimod_time(&t) == 0 && t.source != BRIMOD_TIME_NONE) {
            sprintf(value, "%02d:%02d  (%s)", t.hour, t.min, tsrc[t.source & 3]);
        } else {
            strcpy(value, "not set");
        }
        break;
    case 5:
        strcpy(value, restart_armed ? "press OK again" : "OK");
        break;
    }
}

static const char *main_help(int i) {
    static const char *h[] = {
        "", "Everything the bridge measures, live.",
        "Connect the bridge to a WiFi network (for NTP and the mailbox commands).",
        "Frequency, power, audio, RDS: every SI4713 setting.",
        "Time zone, internet time, or set it by hand.",
        "Restarts the ESP32 (about 2 s); its settings stay.",
    };

    return h[i];
}

static void main_live(void) {
    static u32 last;
    char s[112];

    if (get_timer(last) < 2000) {
        return;
    }
    last = get_timer(0);
    snprintf(s, sizeof(s), "Bridge firmware %d at I2C 0x%02x", brimod_fw, BRIMOD_ADDR);
    line_at(LIVE_Y, s, GREY);
}

static void main_enter(void) {
    wifi_refresh();
}

static int main_press(int i) {
    if (i != 5) {
        restart_armed = 0;
    }
    switch (i) {
    case 1:
        status_page();
        break;
    case 2:
        run_menu(&wifi_menu);
        break;
    case 3:
        run_menu(&fm_menu);
        break;
    case 4:
        run_menu(&clock_menu);
        break;
    case 5:
        if (!restart_armed) {
            restart_armed = 1;
            snprintf(msg, sizeof(msg), "Press OK again to restart the bridge");
            msg_col = YELLOW;
            return 0;
        }
        restart_armed = 0;
        brimod_restart();
        busy("Restarting the bridge ...");
        sdk_idle(2500000);
        snprintf(msg, sizeof(msg), "Bridge restarted");
        msg_col = GREEN;
        return 0;
    }
    msg[0] = 0;                                     /* the page's messages stay there */
    return 0;
}

static const struct menu main_menu = {
    "", 6, main_row, 0, main_press, main_help, main_live, main_enter,
};

int main(int argc, char *argv[]) {
    struct sdk_key k;

    (void) argc;
    (void) argv;
    have_screen = osd_setup(&fb) == 0;
    if (!sdk_brimod) {
        const char *s = sdk_box_sat ? "No BriMod bridge: the FD650 panel is on the box bus, "
                        "or nothing answers at 0x42." : "BriMod is for the satellite box.";

        printf("brimod: %s\n", s);
        if (!have_screen) {
            return 0;
        }
        frame("BriMod", "BACK back");
        text(LX, 140, s, 2, YELLOW);
        for (;;) {
            if (!sdk_key_poll(&k)) {
                sdk_idle(5000);
            } else if (k.btn == BTN_BACK || k.btn == BTN_MENU || k.btn == BTN_OK ||
                       k.btn == BTN_POWER) {
                return 0;
            }
        }
    }
    printf("brimod: bridge firmware %d\n", brimod_fw);
    if (!have_screen) {
        return 0;
    }
    panel_tick(1);
    run_menu(&main_menu);
    sdk_panel_show("---");
    return 0;
}
