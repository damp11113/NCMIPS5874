/*
 * BriMod: the satellite box with the ESP32-C3 bridge (esp32c3/bridge/
 * bridge.ino) on its front-panel I2C bus. The bridge drives the front
 * panel (the FD650 moves to the bridge's own bus), sends the front
 * buttons as IR frames and adds a clock (DS1302, NTP), a climate sensor
 * (AHT20 + BMP280), an FM transmitter with RDS (SI4713) and WiFi (text
 * commands through a mailbox: HTTP, NTP, network scan ...).
 *
 * The runtime looks for it once at start: only when no FD650 answers on
 * the box bus does it ask address 0x42 for its magic "NCB1". So an
 * unmodified box (IPTV, or satellite with its panel wired as shipped)
 * never sees a BriMod transfer, and the FD650 code runs there as before.
 * With the bridge (sdk_brimod = 1, sdk_box_sat stays 1):
 *   sdk_panel_show / sdk_panel_led / led_green   go to the bridge
 *   front buttons (IR user code BRIMOD_IR_FRONT)  BTN_* in sdk_key_poll
 *   doorbell frames (BRIMOD_IR_EVENT)             brimod_doorbell ()
 * so apps need no change; the functions below reach everything else.
 *
 * Link: I2C at 100 kHz, 7-bit address 0x42 (apps must not use it).
 *   write: {reg, data...}
 *   read:  write {0xff, reg, n}, STOP, wait 300 us, read n bytes (n <= 32)
 * Multi-byte values are little-endian. Register map: bridge.ino.
 *
 * Functions return 0 (or a value >= 0) or a negative error: SDK_I2C_E*
 * from the bus, or BRIMOD_E* below. All of them return BRIMOD_ENODEV
 * without a bridge.
 */
#ifndef SDK_BRIMOD_H
#define SDK_BRIMOD_H

#define BRIMOD_ADDR         0x42
#define BRIMOD_MAGIC        "NCB1"
#define BRIMOD_READ_MAX     32          /* bytes per brimod_read */
#define BRIMOD_WRITE_MAX    127         /* bytes per brimod_write (bridge buffer) */
#define BRIMOD_CMD_MAX      120         /* mailbox command length */

/* Registers */
#define BRIMOD_REG_MAGIC    0x00
#define BRIMOD_REG_VERSION  0x04
#define BRIMOD_REG_PRESENT  0x05
#define BRIMOD_REG_EVENTS   0x06        /* 16 bit, write 1 bits to clear */
#define BRIMOD_REG_BELLMASK 0x08        /* 16 bit */
#define BRIMOD_REG_CONFIG   0x0a
#define BRIMOD_REG_KEY      0x0b        /* FD650 key register as last read */
#define BRIMOD_REG_UPTIME   0x0c        /* 32 bit, seconds */
#define BRIMOD_REG_TIME     0x10        /* 32 bit, Unix seconds UTC */
#define BRIMOD_REG_TZ       0x14        /* 16 bit signed, minutes */
#define BRIMOD_REG_TSOURCE  0x16
#define BRIMOD_REG_CLIMATE  0x20        /* 0x20-0x29 */
#define BRIMOD_REG_FM_STATUS 0x30       /* 0x30-0x33: noise, antenna, input */
#define BRIMOD_REG_ADC      0x38        /* 0x38-0x3d */
#define BRIMOD_REG_FM       0x40        /* 0x40-0x45: freq, power, flags, antenna, RDS options */
#define BRIMOD_REG_RDS_PI   0x46
#define BRIMOD_REG_RDS_PS   0x48        /* 8 characters */
#define BRIMOD_REG_RDS_RT   0x50        /* 64 characters */
#define BRIMOD_REG_SIPROP   0x90        /* SI4713 property window */
#define BRIMOD_REG_PANEL    0xa0        /* FD650 digit registers 0-3 */
#define BRIMOD_REG_PANEL_CTRL 0xa4
#define BRIMOD_REG_MAIL     0xc0        /* W: command, R: status */
#define BRIMOD_REG_REPLY    0xc8        /* W: offset, R: reply bytes */
#define BRIMOD_REG_RESTART  0xf0

/* brimod_present () */
#define BRIMOD_HAS_AHT20    0x01
#define BRIMOD_HAS_BMP280   0x02
#define BRIMOD_HAS_SI4713   0x08
#define BRIMOD_HAS_DS1302   0x10
#define BRIMOD_HAS_PANEL    0x20        /* FD650 answers on the bridge's bus */
#define BRIMOD_HAS_WIFI     0x40        /* connected */
#define BRIMOD_HAS_TIME     0x80        /* the clock is set */

/* Events: brimod_events () and the doorbell */
#define BRIMOD_EV_KEY       0x01        /* front button */
#define BRIMOD_EV_CLIMATE   0x02        /* new sensor values */
#define BRIMOD_EV_ADC       0x04
#define BRIMOD_EV_TIME      0x08        /* clock set or synced */
#define BRIMOD_EV_WIFI      0x10        /* connected / disconnected */
#define BRIMOD_EV_MAIL      0x20        /* mailbox command finished */
#define BRIMOD_EV_FM        0x40

/* brimod_config () */
#define BRIMOD_CFG_FRONT_IR 0x01        /* front buttons as IR frames (default on) */
#define BRIMOD_CFG_PANEL    0x02        /* the bridge drives the panel (default on) */
#define BRIMOD_CFG_BELL     0x04        /* doorbell frames (default on) */

/* IR user codes of the bridge's frames (the stock remote is 0xfe01).
 * Front: key byte = FD650 key code (FD650_KEY_*); event: key byte = the
 * low byte of the events that rang (the doorbell mask picks them). */
#define BRIMOD_IR_FRONT     0xa55a
#define BRIMOD_IR_EVENT     0xa55b

/* Clock sources */
#define BRIMOD_TIME_NONE    0
#define BRIMOD_TIME_DS1302  1
#define BRIMOD_TIME_NTP     2
#define BRIMOD_TIME_BOX     3           /* set with brimod_set_time */

/* FM flags */
#define BRIMOD_FM_TX        0x01
#define BRIMOD_FM_RDS       0x02
#define BRIMOD_FM_MONO      0x04        /* no stereo pilot (RDS stereo flag follows) */
#define BRIMOD_RDS_CT       0x01        /* rds_opts: clock time every minute */
#define BRIMOD_IN_LOW       0x01        /* in_flags: input below the low level */
#define BRIMOD_IN_HIGH      0x02        /* above the high level */
#define BRIMOD_IN_OVERMOD   0x04        /* overmodulation in the last second */

/* SI4713 properties the bridge lets the box set (AN332 has the details) */
#define BRIMOD_SI_AUDIO_DEV     0x2101  /* 10 Hz units, 0..9000 */
#define BRIMOD_SI_PILOT_DEV     0x2102  /* 10 Hz units */
#define BRIMOD_SI_RDS_DEV       0x2103  /* 10 Hz units, 0..7500 */
#define BRIMOD_SI_LINE_INPUT    0x2104  /* bits 13-12 range (190/301/416/636 mVpk), 9-0 full scale mVpk */
#define BRIMOD_SI_LINE_MUTE     0x2105  /* bit 1 left, bit 0 right */
#define BRIMOD_SI_PREEMPHASIS   0x2106  /* 0 = 75 us, 1 = 50 us, 2 = off */
#define BRIMOD_SI_PILOT_FREQ    0x2107  /* Hz */
#define BRIMOD_SI_DYNAMICS      0x2200  /* bit 1 limiter, bit 0 compressor */
#define BRIMOD_SI_COMP_THRESH   0x2201  /* dBFS, -40..0 (16 bit signed) */
#define BRIMOD_SI_COMP_ATTACK   0x2202  /* 0..9 = 0.5..5 ms */
#define BRIMOD_SI_COMP_RELEASE  0x2203  /* 0..4 = 100, 200, 350, 525, 1000 ms */
#define BRIMOD_SI_COMP_GAIN     0x2204  /* dB, 0..20 */
#define BRIMOD_SI_LIMIT_RELEASE 0x2205  /* 512 / n ms, n 5..2000 */
#define BRIMOD_SI_PS_MIX        0x2C02  /* 0..6: station name share of the RDS groups */
#define BRIMOD_SI_PS_MISC       0x2C03  /* bit 11 force PTY / TP, 10 TP, 9-5 PTY, 4 TA, 3 music */
#define BRIMOD_SI_AF            0x2C06  /* 0xE0E0 none, 0xE101..0xE1CC = 87.6..107.9 MHz */

/* Mailbox status */
#define BRIMOD_MAIL_IDLE    0
#define BRIMOD_MAIL_BUSY    1
#define BRIMOD_MAIL_DONE    2
#define BRIMOD_MAIL_ERROR   3

#define BRIMOD_ENODEV       (-10)       /* no bridge on this box */
#define BRIMOD_EBUSY        (-11)       /* the mailbox still runs a command */
#define BRIMOD_ETIMEOUT     (-12)       /* the command did not finish in time */
#define BRIMOD_EFAIL        (-13)       /* the command failed; the reply says why */
#define BRIMOD_EPROTO       (-14)       /* bad answer (not a bridge?) */

extern int sdk_brimod;                  /* 1 = bridge found at start */
extern int brimod_fw;                   /* its firmware version */

/* Raw registers: read up to BRIMOD_READ_MAX bytes, write any number */
int brimod_read(int reg, void *buf, int n);
int brimod_write(int reg, const void *buf, int n);

int brimod_present(void);               /* BRIMOD_HAS_* bits */
int brimod_config(int set, int clear);  /* change BRIMOD_CFG_* bits, returns the new value */
int brimod_restart(void);               /* ESP32 restarts (~1 s); the link is gone meanwhile */

/* Events the bridge collected: returns them and clears them there */
int brimod_events(void);
/* Events whose doorbell IR frame arrived (seen by sdk_key_poll) since
 * the last call; no I2C transfer, so cheap to call every frame */
int brimod_doorbell(void);
int brimod_set_bell_mask(int mask);     /* events that ring; default BRIMOD_EV_MAIL */

/* ---- clock ---- */

struct brimod_time {
    u32 utc;                            /* Unix seconds */
    int tz_min;                         /* local = UTC + tz_min minutes */
    int source;                         /* BRIMOD_TIME_*; NONE = not set */
    int year, month, day;               /* local date: 2026, 1-12, 1-31 */
    int hour, min, sec;
    int wday;                           /* 0 = Sunday */
};

int brimod_time(struct brimod_time *t);
int brimod_set_time(u32 utc);           /* also writes the DS1302 */
int brimod_set_tz(int minutes);         /* e.g. 420 = UTC+7 */
/* Unix seconds + offset -> the date fields of t (no I2C) */
void brimod_split_time(u32 utc, int tz_min, struct brimod_time *t);
/* Local date and time + offset -> Unix seconds (no I2C) */
u32 brimod_make_time(int year, int month, int day, int hour, int min, int sec, int tz_min);
/* Sync the clock from the internet now (needs WiFi); msg gets the
 * bridge's answer. 0 = ok */
int brimod_ntp_sync(char *msg, int max);

/* ---- WiFi (the bridge's; mailbox commands underneath) ---- */

struct brimod_wifi {
    int connected;
    int rssi;                           /* dBm */
    char ip[16];
    char ssid[33];                      /* connected, else the saved network ("" = none) */
};

struct brimod_net {
    int rssi, channel, secure;
    char ssid[33];
};

int brimod_wifi_status(struct brimod_wifi *w);
/* Visible networks, strongest first, each name once (hidden ones left
 * out): returns how many (up to max), ~3 s */
int brimod_wifi_scan(struct brimod_net *nets, int max);
/* Connect and remember (password "" = open network); msg gets "OK ip"
 * or the reason. 0 = connected. Up to ~16 s. The password is never
 * printed by the SDK. */
int brimod_wifi_connect(const char *ssid, const char *pass, char *msg, int max);
int brimod_wifi_forget(void);           /* disconnect, forget the network */

/* ---- sensors ---- */

struct brimod_climate {
    int temp;                           /* AHT20, 0.01 degC */
    int humidity;                       /* AHT20, 0.01 % RH */
    u32 pressure;                       /* BMP280, Pa */
    int bmp_temp;                       /* BMP280, 0.01 degC */
};

struct brimod_adc {
    int mv[2];                          /* ESP32 GPIO0 / GPIO1, mV */
    int chip_temp;                      /* ESP32 die, 0.01 degC */
};

int brimod_climate(struct brimod_climate *c);   /* absent sensors read 0 */
int brimod_adc(struct brimod_adc *a);

/* ---- FM transmitter (SI4713) ---- */

struct brimod_fm {
    /* settings (brimod_fm_write; kept by the bridge over power-off) */
    int freq;                           /* 10 kHz units: 10230 = 102.30 MHz (50 kHz steps) */
    int power;                          /* dBuV 88..115 */
    int flags;                          /* BRIMOD_FM_TX | BRIMOD_FM_RDS | BRIMOD_FM_MONO */
    int antcap;                         /* 0 = automatic, 1..191 = n x 0.25 pF */
    int rds_opts;                       /* BRIMOD_RDS_CT */
    /* measured (read only) */
    int noise;                          /* dBuV at freq, from the last tune */
    int antcap_used;                    /* capacitor in use, 0.25 pF steps */
    int in_flags;                       /* BRIMOD_IN_* */
    int in_level;                       /* audio input, dBFS */
};

int brimod_fm_get(struct brimod_fm *fm);
int brimod_fm_write(const struct brimod_fm *fm);   /* the settings part */
int brimod_fm_set(int freq, int power, int flags);
/* PI code, station name (up to 8 characters), RadioText (up to 64);
 * NULL keeps the current text */
int brimod_rds_set(int pi, const char *ps, const char *text);
/* Current texts: ps 9 bytes, text 65 bytes (trailing spaces removed) */
int brimod_rds_get(int *pi, char *ps, char *text);

/* SI4713 properties (BRIMOD_SI_*, AN332): the value, or negative */
int brimod_si_get(int prop);
int brimod_si_set(int prop, int value);
/* Properties, antenna, mono, CT back to the bridge's defaults */
int brimod_fm_defaults(void);
/* Noise scan 87.5-108 MHz (~10 s): the quietest frequencies (10 kHz
 * units) and their noise (dBuV); returns how many (up to max) */
int brimod_fm_scan(int *freq, int *noise, int max);

/* ---- front panel ---- */

/* Raw FD650 digit registers 0-3 (panel bits; see fd650.h: left to right
 * = registers 2, 3, 1, register 1 bit 3 = green LED). sdk_panel_show is
 * the usual way. */
int brimod_panel_raw(const unsigned char seg[4]);
/* FD650 control: bit 0 = display on, bits 6..4 = brightness (0 =
 * brightest, 1..7 = 1/8..7/8); default 0x41 */
int brimod_panel_control(int v);

/* ---- mailbox (WiFi and other slow jobs, text in and out) ---- */

/* Start a command ("PING", "HTTP http://...", see bridge.ino) */
int brimod_cmd_start(const char *cmd);
/* BRIMOD_MAIL_*; *len = reply length when not NULL */
int brimod_cmd_status(int *len);
/* Copy the reply (NUL terminated, up to max - 1 bytes); returns its
 * full length */
int brimod_cmd_reply(char *buf, int max);
/* Start, wait up to timeout_ms (sdk_idle while waiting), copy the reply:
 * returns the reply length, BRIMOD_EFAIL (reply = the message),
 * BRIMOD_ETIMEOUT, ... */
int brimod_cmd(const char *cmd, char *reply, int max, u32 timeout_ms);

/* ---- used by the SDK runtime ---- */

int brimod_detect(void);                /* 1 = bridge found (sets sdk_brimod) */
void brimod_panel_show(const char *s);
void brimod_panel_led(int on);
/* An IR frame from the bridge: 1 = front button (*btn, *repeat),
 * 0 = consumed or not the bridge's */
int brimod_ir_key(const struct ir_event *ev, int *btn, int *repeat);

#endif
