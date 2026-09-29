/*
 * BriMod driver: the ESP32-C3 bridge on the satellite box's front-panel
 * I2C bus (SoC channel 2). Interface, protocol and when it is used:
 * brimod.h. The FD650 code in runtime.c / fd650.h is not touched by this;
 * the runtime only calls in here when brimod_detect () found the bridge.
 */
#include "sdk.h"

int sdk_brimod;
int brimod_fw;

#define BRIMOD_PRESCALE         0x35    /* 100 kHz = i2c_prescale (100) */
#define BRIMOD_PREP_US          300     /* the bridge prepares a read reply */
#define BRIMOD_REPEAT_DELAY_MS  400     /* as the remote: repeats count after this */

void sdk_i2c_use_prescale(u32 pre);     /* runtime.c: the bus speed is shared with apps */

static unsigned char panel_seg[4];      /* the bridge's digit registers */
static int bell;                        /* events rung by IR frames, for brimod_doorbell */
static u32 front_press_ms;

static int le16(const unsigned char *b) {
    return b[0] | (b[1] << 8);
}

static int les16(const unsigned char *b) {
    return (short) (b[0] | (b[1] << 8));
}

static u32 le32(const unsigned char *b) {
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((u32) b[3] << 24);
}

/* Read request, then the reply. A failed read leaves the reply in the
 * bridge's TX FIFO, where it would come before the next one: a finished
 * read of one byte empties it. */
static int raw_read(int reg, void *buf, int n) {
    unsigned char req[3], dummy;
    int r;

    if (n < 1 || n > BRIMOD_READ_MAX) {
        return BRIMOD_EPROTO;
    }
    req[0] = 0xff;
    req[1] = (unsigned char) reg;
    req[2] = (unsigned char) n;
    sdk_i2c_use_prescale(BRIMOD_PRESCALE);
    r = i2c_write(I2C_CH_FP, BRIMOD_ADDR, req, 3);
    if (r) {
        return r;
    }
    udelay(BRIMOD_PREP_US);
    r = i2c_read(I2C_CH_FP, BRIMOD_ADDR, buf, n);
    if (r) {
        i2c_read(I2C_CH_FP, BRIMOD_ADDR, &dummy, 1);
    }
    return r;
}

int brimod_read(int reg, void *buf, int n) {
    if (!sdk_brimod) {
        return BRIMOD_ENODEV;
    }
    return raw_read(reg, buf, n);
}

int brimod_write(int reg, const void *buf, int n) {
    unsigned char b[1 + BRIMOD_WRITE_MAX];

    if (!sdk_brimod) {
        return BRIMOD_ENODEV;
    }
    if (n < 0 || n > BRIMOD_WRITE_MAX) {
        return BRIMOD_EPROTO;
    }
    b[0] = (unsigned char) reg;
    memcpy(b + 1, buf, n);
    sdk_i2c_use_prescale(BRIMOD_PRESCALE);
    return i2c_write(I2C_CH_FP, BRIMOD_ADDR, b, n + 1);
}

/* Called once at start, only when no FD650 answered on the bus */
int brimod_detect(void) {
    unsigned char b[5];

    sdk_brimod = 0;
    if (raw_read(BRIMOD_REG_MAGIC, b, 5) != 0 || memcmp(b, BRIMOD_MAGIC, 4) != 0) {
        return 0;
    }
    brimod_fw = b[4];
    raw_read(BRIMOD_REG_PANEL, panel_seg, 4);   /* keeps the LED as it is */
    sdk_brimod = 1;
    return 1;
}

static int read8(int reg) {
    unsigned char v;
    int r = brimod_read(reg, &v, 1);

    return r < 0 ? r : v;
}

static int write8(int reg, int v) {
    unsigned char b = (unsigned char) v;

    return brimod_write(reg, &b, 1);
}

int brimod_present(void) {
    return read8(BRIMOD_REG_PRESENT);
}

int brimod_config(int set, int clear) {
    int v = read8(BRIMOD_REG_CONFIG);
    int r;

    if (v < 0) {
        return v;
    }
    v = (v | set) & ~clear & 0xff;
    r = write8(BRIMOD_REG_CONFIG, v);
    return r < 0 ? r : v;
}

int brimod_restart(void) {
    return write8(BRIMOD_REG_RESTART, 0xa5);
}

/* ---- events ---- */

int brimod_events(void) {
    unsigned char b[2];
    int r = brimod_read(BRIMOD_REG_EVENTS, b, 2);

    if (r < 0) {
        return r;
    }
    if (b[0] | b[1]) {
        r = brimod_write(BRIMOD_REG_EVENTS, b, 2);      /* write 1 = clear */
        if (r < 0) {
            return r;
        }
    }
    return le16(b);
}

int brimod_doorbell(void) {
    int b = bell;

    bell = 0;
    return sdk_brimod ? b : BRIMOD_ENODEV;
}

int brimod_set_bell_mask(int mask) {
    unsigned char b[2] = { (unsigned char) mask, (unsigned char) (mask >> 8) };

    return brimod_write(BRIMOD_REG_BELLMASK, b, 2);
}

/* ---- clock ---- */

void brimod_split_time(u32 utc, int tz_min, struct brimod_time *t) {
    u32 shift = (u32) (tz_min < 0 ? -tz_min : tz_min) * 60;
    u32 local = tz_min >= 0 ? utc + shift : utc > shift ? utc - shift : 0;
    u32 days = local / 86400, secs = local % 86400;
    u32 z, era, doe, yoe, doy, mp;

    t->hour = secs / 3600;
    t->min = secs / 60 % 60;
    t->sec = secs % 60;
    t->wday = (days + 4) % 7;                           /* 1970-01-01 was a Thursday */
    /* days -> civil date (H. Hinnant's algorithm, years from 0000-03-01) */
    z = days + 719468;
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    t->day = doy - (153 * mp + 2) / 5 + 1;
    t->month = mp < 10 ? mp + 3 : mp - 9;
    t->year = yoe + era * 400 + (t->month <= 2);
}

u32 brimod_make_time(int year, int month, int day, int hour, int min, int sec, int tz_min) {
    /* civil date -> days since 1970-01-01 (H. Hinnant's algorithm) */
    int y = year - (month <= 2);
    int era = y / 400;
    u32 yoe = (u32) (y - era * 400);
    u32 doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    u32 days = (u32) era * 146097 + doe - 719468;
    u32 local = days * 86400 + hour * 3600 + min * 60 + sec;

    return tz_min >= 0 ? local - (u32) tz_min * 60 : local + (u32) (-tz_min) * 60;
}

int brimod_ntp_sync(char *msg, int max) {
    int r = brimod_cmd("NTP", msg, max, 12000);

    return r < 0 ? r : 0;
}

int brimod_time(struct brimod_time *t) {
    unsigned char b[7];
    int r = brimod_read(BRIMOD_REG_TIME, b, 7);

    if (r < 0) {
        return r;
    }
    t->utc = le32(b);
    t->tz_min = les16(b + 4);
    t->source = b[6];
    brimod_split_time(t->utc, t->tz_min, t);
    return 0;
}

int brimod_set_time(u32 utc) {
    unsigned char b[4] = {
        (unsigned char) utc, (unsigned char) (utc >> 8), (unsigned char) (utc >> 16),
        (unsigned char) (utc >> 24)
    };

    return brimod_write(BRIMOD_REG_TIME, b, 4);
}

int brimod_set_tz(int minutes) {
    unsigned char b[2] = { (unsigned char) minutes, (unsigned char) (minutes >> 8) };

    return brimod_write(BRIMOD_REG_TZ, b, 2);
}

/* ---- sensors ---- */

int brimod_climate(struct brimod_climate *c) {
    unsigned char b[10];
    int r = brimod_read(BRIMOD_REG_CLIMATE, b, 10);

    if (r < 0) {
        return r;
    }
    c->temp = les16(b);
    c->humidity = le16(b + 2);
    c->pressure = le32(b + 4);
    c->bmp_temp = les16(b + 8);
    return 0;
}

int brimod_adc(struct brimod_adc *a) {
    unsigned char b[6];
    int r = brimod_read(BRIMOD_REG_ADC, b, 6);

    if (r < 0) {
        return r;
    }
    a->mv[0] = le16(b);
    a->mv[1] = le16(b + 2);
    a->chip_temp = les16(b + 4);
    return 0;
}

/* ---- FM transmitter ---- */

int brimod_fm_get(struct brimod_fm *fm) {
    unsigned char b[6], s[4];
    int r = brimod_read(BRIMOD_REG_FM, b, 6);

    if (r == 0) {
        r = brimod_read(BRIMOD_REG_FM_STATUS, s, 4);
    }
    if (r < 0) {
        return r;
    }
    fm->freq = le16(b);
    fm->power = b[2];
    fm->flags = b[3];
    fm->antcap = b[4];
    fm->rds_opts = b[5];
    fm->noise = s[0];
    fm->antcap_used = s[1];
    fm->in_flags = s[2];
    fm->in_level = (signed char) s[3];
    return 0;
}

int brimod_fm_write(const struct brimod_fm *fm) {
    unsigned char b[6] = {
        (unsigned char) fm->freq, (unsigned char) (fm->freq >> 8), (unsigned char) fm->power,
        (unsigned char) fm->flags, (unsigned char) fm->antcap, (unsigned char) fm->rds_opts
    };

    return brimod_write(BRIMOD_REG_FM, b, 6);
}

int brimod_fm_set(int freq, int power, int flags) {
    unsigned char b[4] = {
        (unsigned char) freq, (unsigned char) (freq >> 8), (unsigned char) power,
        (unsigned char) flags
    };

    return brimod_write(BRIMOD_REG_FM, b, 4);
}

/* Space padded, like the bridge keeps them */
static int write_text(int reg, const char *s, int len) {
    char b[64];
    int i;

    for (i = 0; i < len; i++) {
        b[i] = *s ? *s++ : ' ';
    }
    return brimod_write(reg, b, len);
}

int brimod_rds_set(int pi, const char *ps, const char *text) {
    unsigned char b[2] = { (unsigned char) pi, (unsigned char) (pi >> 8) };
    int r = brimod_write(BRIMOD_REG_RDS_PI, b, 2);

    if (r == 0 && ps) {
        r = write_text(BRIMOD_REG_RDS_PS, ps, 8);
    }
    if (r == 0 && text) {
        r = write_text(BRIMOD_REG_RDS_RT, text, 64);
    }
    return r;
}

/* n bytes of text (n <= 64), trailing spaces / zeros cut */
static int read_text(int reg, char *out, int n) {
    int r = 0, i;

    for (i = 0; r == 0 && i < n; i += BRIMOD_READ_MAX) {
        r = brimod_read(reg + i, out + i, n - i > BRIMOD_READ_MAX ? BRIMOD_READ_MAX : n - i);
    }
    out[n] = 0;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) {
        out[--n] = 0;
    }
    return r;
}

int brimod_rds_get(int *pi, char *ps, char *text) {
    unsigned char b[2];
    int r = brimod_read(BRIMOD_REG_RDS_PI, b, 2);

    if (r == 0 && pi) {
        *pi = le16(b);
    }
    if (r == 0 && ps) {
        r = read_text(BRIMOD_REG_RDS_PS, ps, 8);
    }
    if (r == 0 && text) {
        r = read_text(BRIMOD_REG_RDS_RT, text, 64);
    }
    return r;
}

int brimod_si_get(int prop) {
    unsigned char sel[2] = { (unsigned char) prop, (unsigned char) (prop >> 8) }, b[4];
    int r = brimod_write(BRIMOD_REG_SIPROP, sel, 2);

    if (r == 0) {
        r = brimod_read(BRIMOD_REG_SIPROP, b, 4);
    }
    if (r < 0) {
        return r;
    }
    if (le16(b) != (prop & 0xffff) || le16(b + 2) == 0xffff) {
        return BRIMOD_EPROTO;               /* not a property the bridge lets us set */
    }
    return le16(b + 2);
}

int brimod_si_set(int prop, int value) {
    unsigned char b[4] = {
        (unsigned char) prop, (unsigned char) (prop >> 8), (unsigned char) value,
        (unsigned char) (value >> 8)
    };

    return brimod_write(BRIMOD_REG_SIPROP, b, 4);
}

int brimod_fm_defaults(void) {
    char msg[32];
    int r = brimod_cmd("FMDEFAULTS", msg, sizeof(msg), 2000);

    return r < 0 ? r : 0;
}

int brimod_fm_scan(int *freq, int *noise, int max) {
    char reply[128];
    const char *p = reply;
    int n = 0, r = brimod_cmd("FMSCAN", reply, sizeof(reply), 30000);

    if (r < 0) {
        return r;
    }
    while (*p && n < max) {                 /* "freq noise" lines */
        char *end;
        long f = strtol(p, &end, 10);

        if (end == p) {
            break;
        }
        freq[n] = (int) f;
        noise[n] = (int) strtol(end, &end, 10);
        n++;
        p = strchr(end, '\n');
        if (!p) {
            break;
        }
        p++;
    }
    return n;
}

/* ---- front panel ---- */

int brimod_panel_raw(const unsigned char seg[4]) {
    int r = brimod_write(BRIMOD_REG_PANEL, seg, 4);

    if (r == 0) {
        memcpy(panel_seg, seg, 4);
    }
    return r;
}

int brimod_panel_control(int v) {
    return write8(BRIMOD_REG_PANEL_CTRL, v);
}

/* Same characters and layout as fd650_show, written in one transfer */
void brimod_panel_show(const char *s) {
    int i;

    for (i = 0; i < 3; i++) {
        char c = *s ? *s++ : ' ';
        int reg = fd650_pos_reg[i];
        unsigned char seg = fd650_char(c);

        if (reg == FD650_LED_REG) {
            seg = (seg & ~FD650_LED_BIT) | (panel_seg[reg] & FD650_LED_BIT);
        }
        panel_seg[reg] = seg;
    }
    brimod_write(BRIMOD_REG_PANEL, panel_seg, 4);
}

void brimod_panel_led(int on) {
    panel_seg[FD650_LED_REG] = (panel_seg[FD650_LED_REG] & ~FD650_LED_BIT) |
                               (on ? FD650_LED_BIT : 0);
    brimod_write(BRIMOD_REG_PANEL + FD650_LED_REG, &panel_seg[FD650_LED_REG], 1);
}

/* ---- IR frames from the bridge ---- */

static const struct { unsigned char code; short btn; } front_btn[] = {
    { FD650_KEY_MENU, BTN_MENU }, { FD650_KEY_OK, BTN_OK },
    { FD650_KEY_VOLDOWN, BTN_LEFT }, { FD650_KEY_VOLUP, BTN_RIGHT },
    { FD650_KEY_CHDOWN, BTN_DOWN }, { FD650_KEY_CHUP, BTN_UP },
};

int brimod_ir_key(const struct ir_event *ev, int *btn, int *repeat) {
    unsigned int i;

    if (ev->user == BRIMOD_IR_EVENT) {
        bell |= ev->key;
        return 0;
    }
    if (ev->user != BRIMOD_IR_FRONT) {
        return 0;
    }
    /* held: the bridge resends the frame every 110 ms (ev->repeat = 1) */
    if (!ev->repeat) {
        front_press_ms = get_timer(0);
    } else if (get_timer(0) - front_press_ms < BRIMOD_REPEAT_DELAY_MS) {
        return 0;
    }
    for (i = 0; i < sizeof(front_btn) / sizeof(front_btn[0]); i++) {
        if (front_btn[i].code == ev->key) {
            *btn = front_btn[i].btn;
            *repeat = ev->repeat;
            return 1;
        }
    }
    return 0;
}

/* ---- mailbox ---- */

int brimod_cmd_start(const char *cmd) {
    unsigned char st;
    int n = (int) strlen(cmd), r;

    if (!sdk_brimod) {
        return BRIMOD_ENODEV;
    }
    if (n < 1 || n > BRIMOD_CMD_MAX) {
        return BRIMOD_EPROTO;
    }
    r = brimod_read(BRIMOD_REG_MAIL, &st, 1);
    if (r < 0) {
        return r;
    }
    if (st == BRIMOD_MAIL_BUSY) {
        return BRIMOD_EBUSY;
    }
    return brimod_write(BRIMOD_REG_MAIL, cmd, n);
}

int brimod_cmd_status(int *len) {
    unsigned char b[3];
    int r = brimod_read(BRIMOD_REG_MAIL, b, 3);

    if (r < 0) {
        return r;
    }
    if (len) {
        *len = le16(b + 1);
    }
    return b[0];
}

int brimod_cmd_reply(char *buf, int max) {
    unsigned char off[2] = { 0, 0 };
    int len, got = 0, r;

    r = brimod_cmd_status(&len);
    if (r < 0) {
        return r;
    }
    if (!buf || max < 1) {
        return len;
    }
    r = brimod_write(BRIMOD_REG_REPLY, off, 2);
    /* each read moves the bridge's reply offset on */
    while (r == 0 && got < len && got < max - 1) {
        int n = len - got;

        n = n > BRIMOD_READ_MAX ? BRIMOD_READ_MAX : n;
        n = n > max - 1 - got ? max - 1 - got : n;
        r = brimod_read(BRIMOD_REG_REPLY, buf + got, n);
        if (r == 0) {
            got += n;
        }
    }
    buf[got] = 0;
    return r < 0 ? r : len;
}

int brimod_cmd(const char *cmd, char *reply, int max, u32 timeout_ms) {
    u32 t0 = get_timer(0);
    int st, r = brimod_cmd_start(cmd);

    if (r < 0) {
        return r;
    }
    for (;;) {
        st = brimod_cmd_status(0);
        if (st < 0) {
            return st;
        }
        if (st == BRIMOD_MAIL_DONE || st == BRIMOD_MAIL_ERROR) {
            break;
        }
        if (get_timer(t0) >= timeout_ms) {
            return BRIMOD_ETIMEOUT;
        }
        sdk_idle(20000);
    }
    r = brimod_cmd_reply(reply, max);
    if (r < 0) {
        return r;
    }
    return st == BRIMOD_MAIL_ERROR ? BRIMOD_EFAIL : r;
}

/* ---- WiFi ---- */

/* Copy one space-separated word; returns the rest after it */
static const char *word(const char *s, char *out, int max) {
    int n = 0;

    while (*s == ' ') {
        s++;
    }
    while (*s && *s != ' ' && *s != '\n') {
        if (n < max - 1) {
            out[n++] = *s;
        }
        s++;
    }
    out[n] = 0;
    return s;
}

static void copy_line(char *out, const char *s, int max) {
    int n = 0;

    while (*s == ' ') {
        s++;
    }
    while (*s && *s != '\n' && n < max - 1) {
        out[n++] = *s++;
    }
    out[n] = 0;
}

int brimod_wifi_status(struct brimod_wifi *w) {
    char reply[96], tmp[16];
    const char *p;
    int r = brimod_cmd("WIFI?", reply, sizeof(reply), 2000);

    memset(w, 0, sizeof(*w));
    if (r < 0) {
        return r;
    }
    p = word(reply, tmp, sizeof(tmp));
    if (!strcmp(tmp, "CONNECTED")) {        /* CONNECTED ip rssi dBm ssid */
        w->connected = 1;
        p = word(p, w->ip, sizeof(w->ip));
        p = word(p, tmp, sizeof(tmp));
        w->rssi = atoi(tmp);
        p = word(p, tmp, sizeof(tmp));      /* "dBm" */
    }                                       /* DISCONNECTED saved-ssid */
    copy_line(w->ssid, p, sizeof(w->ssid));
    return 0;
}

int brimod_wifi_scan(struct brimod_net *nets, int max) {
    static char reply[3072];
    const char *p = reply;
    int n = 0, r = brimod_cmd("SCAN", reply, sizeof(reply), 15000);

    if (r < 0) {
        return r;
    }
    while (*p && n < max) {                 /* "rssi ch secure ssid" lines */
        struct brimod_net *e = &nets[n];
        char tmp[8];
        int i, dup = 0;

        p = word(p, tmp, sizeof(tmp));
        e->rssi = atoi(tmp);
        p = word(p, tmp, sizeof(tmp));
        e->channel = atoi(tmp);
        p = word(p, tmp, sizeof(tmp));
        e->secure = atoi(tmp);
        if (*p == ' ') {
            p++;                            /* the name keeps its own spaces */
        }
        copy_line(e->ssid, p, sizeof(e->ssid));
        for (i = 0; i < n; i++) {
            dup |= !strcmp(nets[i].ssid, e->ssid);
        }
        if (e->ssid[0] && !dup) {
            n++;
        }
        p = strchr(p, '\n');
        if (!p) {
            break;
        }
        p++;
    }
    return n;
}

int brimod_wifi_connect(const char *ssid, const char *pass, char *msg, int max) {
    char cmd[BRIMOD_CMD_MAX + 1];
    volatile char *wipe = cmd;
    int len = (int) strlen(ssid) + (int) strlen(pass) + 6, r, i;

    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 63 || len > BRIMOD_CMD_MAX) {
        return BRIMOD_EPROTO;
    }
    snprintf(cmd, sizeof(cmd), "WIFI\t%s\t%s", ssid, pass);
    r = brimod_cmd(cmd, msg, max, 20000);
    for (i = 0; i < (int) sizeof(cmd); i++) {
        wipe[i] = 0;                        /* no password left on the stack */
    }
    return r < 0 ? r : 0;
}

int brimod_wifi_forget(void) {
    char msg[16];
    int r = brimod_cmd("WIFIOFF", msg, sizeof(msg), 3000);

    return r < 0 ? r : 0;
}
