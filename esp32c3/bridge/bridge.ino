/*
 * NCAPPS bridge: ESP32-C3 Super Mini between the satellite box and its
 * front panel, climate sensor, FM transmitter and clock (Arduino-ESP32 3.x).
 * Wiring: page "ESP32 Bridge Wiring" (continue.md / sat/NOTES.md).
 *
 * Arduino IDE: board "ESP32C3 Dev Module" (or "Nologo ESP32C3 Super Mini"),
 * Tools -> USB CDC On Boot: Enabled (log on the USB-C port; GPIO20/21 are
 * the DS1302 here, not a UART). No extra libraries needed.
 *
 * ---- Box link -------------------------------------------------------------
 * I2C slave at 7-bit address 0x42 on GPIO8 (SDA) / GPIO9 (SCL), 100 kHz.
 * GPIO9 is a strapping pin: the 4.7k pull-ups on this bus keep it high at
 * boot. If the ESP32 resets while the box is off (unpowered box pulls the
 * lines low), it stays in download mode until the next reset.
 *   write: {reg, data...}              data goes to reg, reg+1, ...
 *   read:  write {0xFF, reg, n}, STOP, wait >= 200 us, then read exactly
 *          n bytes (n <= 32). The reply is prepared when the request
 *          arrives, so always read what you asked for.
 * Multi-byte values are little-endian.
 *
 * ---- Register map -----------------------------------------------------------
 * 0x00-03 R   magic "NCB1"
 * 0x04    R   firmware version
 * 0x05    R   present: bit0 AHT20, 1 BMP280, 2 (unused), 3 SI4713, 4 DS1302,
 *             5 front panel, 6 WiFi connected, 7 time valid
 * 0x06-07 RW  events (write 1 bits to clear): bit0 front key, 1 climate,
 *             2 ADC, 3 time set/synced, 4 WiFi changed, 5 mailbox done,
 *             6 FM changed
 * 0x08-09 RW  doorbell mask: events that also send an IR frame (user code
 *             0xA55B, key byte = events low byte); default mailbox done
 * 0x0A    RW  config: bit0 front keys as IR frames (default 1),
 *             bit1 ESP32 drives the panel (default 1; with the optional
 *             bypass switch 0 = switch closed, box talks to the FD650),
 *             bit2 doorbell on (default 1)
 * 0x0B    R   front key register as last read (bit 6 = pressed)
 * 0x0C-0F R   uptime in seconds
 * 0x10-13 RW  time, Unix seconds UTC (write 4 bytes = set clock + DS1302)
 * 0x14-15 RW  time zone offset in minutes (default +420 = UTC+7)
 * 0x16    R   time source: 0 none, 1 DS1302, 2 NTP, 3 set by the box
 * 0x20-21 R   AHT20 temperature, 0.01 degC (signed)
 * 0x22-23 R   AHT20 humidity, 0.01 %
 * 0x24-27 R   BMP280 pressure, Pa
 * 0x28-29 R   BMP280 temperature, 0.01 degC (signed)
 * 0x30    R   FM: noise level at the frequency (last tune / measure), dBuV
 * 0x31    R   FM: antenna capacitor in use, 0.25 pF steps
 * 0x32    R   FM audio input: bit0 below the low level, bit1 above the high
 *             level, bit2 overmodulation (held 1 s)
 * 0x33    R   FM audio input level, dBFS (signed; 4x a second while on air)
 * 0x34-37     reserved
 * 0x38-3B R   ESP32 ADC GPIO0 / GPIO1, mV
 * 0x3C-3D R   ESP32 chip temperature, 0.01 degC (signed)
 * 0x40-41 RW  FM frequency in 10 kHz (10230 = 102.30 MHz)
 * 0x42    RW  FM power in dBuV (88..115), 0 = carrier off
 * 0x43    RW  FM flags: bit0 transmit, bit1 RDS, bit2 mono (no stereo pilot;
 *             also clears the RDS stereo flag)
 * 0x44    RW  antenna capacitor: 0 = automatic, 1..191 = n x 0.25 pF
 * 0x45    RW  RDS options: bit0 send the clock time (CT, group 4A) every minute
 * 0x46-47 RW  RDS PI code
 * 0x48-4F RW  RDS PS (station name, 8 characters)
 * 0x50-8F RW  RDS RadioText (64 characters, space padded)
 * 0x90-93 RW  SI4713 property window {prop lo, hi, value lo, hi}:
 *             write 4 bytes = set a property, write 2 = select one; a read
 *             of 4 bytes gives the selected property and its value (0xFFFF =
 *             not settable here). Settable: 0x2101-0x2107 (deviations, line
 *             input, mute, pre-emphasis, pilot), 0x2200-0x2205 (limiter,
 *             compressor), 0x2C02 (PS mix), 0x2C03 (PS misc: PTY, TP, TA,
 *             MS, DI), 0x2C06 (AF). FM / RDS registers 0x40-0x8F and the
 *             properties are kept in NVS (3 s after the last change) and
 *             applied at start, so the transmitter comes back after power-off
 * 0xA0-A3 RW  front panel FD650 digit registers 0-3, raw segment bytes
 *             (the box maps characters; reg 1 bit 3 = green LED)
 * 0xA4    RW  FD650 control byte (default 0x41 = display on)
 * 0xC0    W   mailbox: text command (up to 120 bytes) starts it
 *         R   mailbox status: 0 idle, 1 busy, 2 done, 3 error
 * 0xC1-C2 R   reply length
 * 0xC3    R   finished commands counter (8 bit)
 * 0xC8    W   {lo, hi} = reply offset;  R: reply bytes from that offset,
 *             the offset then moves past them (read the reply in pieces)
 * 0xF0    W   0xA5 = restart the ESP32
 *
 * ---- Mailbox commands (reply is text) --------------------------------------
 *   PING                        PONG
 *   VER                         firmware and chip info
 *   WIFI ssid password          connect, remember (NVS), reply "OK ip"
 *   WIFI<tab>ssid<tab>password  the same for names / passwords with spaces
 *   WIFI?                       "CONNECTED ip rssi dBm ssid" or
 *                               "DISCONNECTED saved-ssid"
 *   WIFIOFF                     disconnect
 *   SCAN                        visible networks: "rssi ch secure ssid" lines
 *   NTP [tz_minutes]            sync the clock now
 *   HTTP url                    GET (https not certificate-checked): status
 *                               line, then the body (up to ~8 KB)
 *   FMSCAN                      noise at 87.5-108.0 MHz, reply = 5 quietest
 *                               as "freq noise" lines
 *   FMDEFAULTS                  SI4713 properties, antenna, mono, RDS options
 *                               back to defaults (frequency, power, texts kept)
 *   I2CW addr hexbytes          write to the sensor bus (any extra I2C part)
 *   I2CR addr n                 read n bytes
 *   I2CWR addr hexbytes n       write, repeated START, read n
 *   PIN n 0|1  /  GET n         free pins GPIO0 / GPIO1
 *
 * ---- Front panel and IR -------------------------------------------------------
 * The FD650 sits on its own software I2C (GPIO6/7). Front buttons go to the
 * box as NEC frames on the IR line (GPIO3, Schottky diode, open drain) with
 * user code 0xA55A and the FD650 key code as key byte (0x05 MENU, 0x2d OK,
 * 0x15 VOL-, 0x1d VOL+, 0x25 CH-, 0x35 CH+); a held button sends its frame
 * again every 110 ms (the box's decoder takes the same key within 250 ms
 * as held). The remote keeps its own user code 0xFE01.
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"        // gpio_od_enable: open drain without re-routing the pad

// ---- pins --------------------------------------------------------------------
#define PIN_BOX_SDA     8           // strapping pin, onboard LED (flickers with traffic)
#define PIN_BOX_SCL     9           // strapping pin / BOOT button: pull-up keeps it high at boot
#define PIN_PANEL_SDA   6
#define PIN_PANEL_SCL   7
#define PIN_SENS_SDA    4
#define PIN_SENS_SCL    5
#define PIN_SI_RST      2           // strapping pin: 10k pull-up to 3.3 V fitted
#define PIN_DS_CE       21          // module pin "RST" (= CE in the datasheet)
#define PIN_DS_CLK      10
#define PIN_DS_DAT      20
#define PIN_IR          3
#define PIN_FREE0       0
#define PIN_FREE1       1
#define PIN_BYPASS      -1          // GPIO driving the optional bypass switch, -1 = none

#define BOX_ADDR        0x42
#define FW_VERSION      1
#define IR_USER_FRONT   0xA55A
#define IR_USER_EVENT   0xA55B

#define AHT_ADDR        0x38
#define SI_ADDR         0x63

#define REPLY_MAX       8192
#define CMD_MAX         121

enum {
    P_AHT = 1, P_BMP = 2, P_SI = 8, P_DS = 16, P_PANEL = 32, P_WIFI = 64, P_TIME = 128
};
enum {
    EV_KEY = 1, EV_CLIMATE = 2, EV_ADC = 4, EV_TIME = 8, EV_WIFI = 16, EV_MAIL = 32, EV_FM = 64
};
enum {
    CFG_FRONT_IR = 1, CFG_PANEL_OWN = 2, CFG_BELL = 4
};

// ---- registers shared with the I2C slave task ---------------------------------
static uint8_t regs[256];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static volatile bool panel_dirty, fm_dirty, rds_dirty, time_set_pending, cmd_pending, restart_pending;
static volatile uint32_t time_set_value;
static volatile uint32_t fm_save_ms;        // last FM / RDS write by the box, 0 = saved

// SI4713 properties the box may set (register window 0x90), with our defaults
struct si_setting {
    uint16_t prop, def;
};
static const si_setting si_table[] = {
    { 0x2101, 6625 },       // audio deviation 66.25 kHz (+ pilot 6.75 + RDS 2 = 75 kHz)
    { 0x2102, 675 },        // pilot deviation 6.75 kHz
    { 0x2103, 200 },        // RDS deviation 2 kHz
    { 0x2104, 0x327C },     // line input: 636 mVpk range, 636 mVpk full scale
    { 0x2105, 0 },          // line input mute: none
    { 0x2106, 1 },          // pre-emphasis: 0 = 75 us, 1 = 50 us, 2 = off
    { 0x2107, 19000 },      // pilot frequency, Hz
    { 0x2200, 0x0002 },     // audio dynamics: bit1 limiter, bit0 compressor
    { 0x2201, 0xFFD8 },     // compressor threshold -40 dBFS
    { 0x2202, 0 },          // compressor attack 0.5 ms (n + 1) x 0.5 ms
    { 0x2203, 4 },          // compressor release 1000 ms (100/200/350/525/1000)
    { 0x2204, 15 },         // compressor gain 15 dB
    { 0x2205, 102 },        // limiter release 512 / n ms = 5 ms
    { 0x2C02, 3 },          // PS share 50 %
    { 0x2C03, 0x1808 },     // PS misc: stereo, force PTY / TP, music
    { 0x2C06, 0xE0E0 },     // no alternative frequency
};
#define SI_N (sizeof(si_table) / sizeof(si_table[0]))
static uint16_t si_val[SI_N];
static volatile bool siprop_dirty;

static int si_index(uint16_t prop) {
    for (int i = 0; i < (int) SI_N; i++) {
        if (si_table[i].prop == prop) {
            return i;
        }
    }
    return -1;
}
static volatile uint16_t reply_off;
static char cmd_buf[CMD_MAX + 1];
static char reply[REPLY_MAX];
static size_t reply_len;
static uint16_t pending_bell;

static void put8(int r, uint8_t v) {
    portENTER_CRITICAL(&lock);
    regs[r] = v;
    portEXIT_CRITICAL(&lock);
}

static void put16(int r, uint16_t v) {
    portENTER_CRITICAL(&lock);
    regs[r] = v & 0xff;
    regs[r + 1] = v >> 8;
    portEXIT_CRITICAL(&lock);
}

static void put32(int r, uint32_t v) {
    portENTER_CRITICAL(&lock);
    for (int i = 0; i < 4; i++) {
        regs[r + i] = (v >> (8 * i)) & 0xff;
    }
    portEXIT_CRITICAL(&lock);
}

static uint16_t get16(int r) {
    uint16_t v;

    portENTER_CRITICAL(&lock);
    v = regs[r] | (regs[r + 1] << 8);
    portEXIT_CRITICAL(&lock);
    return v;
}

static void set_present(uint8_t bit, bool on) {
    portENTER_CRITICAL(&lock);
    regs[0x05] = on ? (regs[0x05] | bit) : (regs[0x05] & ~bit);
    portEXIT_CRITICAL(&lock);
}

static void raise_event(uint16_t ev) {
    uint16_t mask;

    portENTER_CRITICAL(&lock);
    regs[0x06] |= ev & 0xff;
    regs[0x07] |= ev >> 8;
    mask = regs[0x08] | (regs[0x09] << 8);
    portEXIT_CRITICAL(&lock);
    pending_bell |= ev & mask;
}

// ---- box link (I2C slave, runs in the Wire slave task) ---------------------------
static void prepare_read(uint8_t reg, uint8_t n) {
    uint8_t out[32];

    if (n > 32) {
        n = 32;
    }
    if (reg == 0xC8) {                          // reply bytes; the offset moves on
        for (int i = 0; i < n; i++) {
            size_t p = reply_off + i;
            out[i] = p < reply_len ? reply[p] : 0;
        }
        reply_off += n;
    } else {
        portENTER_CRITICAL(&lock);
        for (int i = 0; i < n; i++) {
            out[i] = regs[(uint8_t) (reg + i)];
        }
        portEXIT_CRITICAL(&lock);
    }
    Wire.slaveWrite(out, n);
}

static void handle_write(uint8_t reg, const uint8_t *d, int n) {
    if (reg == 0x90) {                          // SI4713 property window
        if (n >= 2) {
            uint16_t prop = d[0] | (d[1] << 8), v;
            int k = si_index(prop);

            portENTER_CRITICAL(&lock);
            if (k >= 0 && n >= 4) {
                si_val[k] = d[2] | (d[3] << 8);
                siprop_dirty = true;
                fm_save_ms = millis() | 1;
            }
            v = k >= 0 ? si_val[k] : 0xFFFF;
            regs[0x90] = d[0];
            regs[0x91] = d[1];
            regs[0x92] = v & 0xff;
            regs[0x93] = v >> 8;
            portEXIT_CRITICAL(&lock);
        }
        return;
    }
    if (reg == 0xC0) {                          // mailbox command
        if (regs[0xC0] != 1 && n > 0) {
            int len = n > CMD_MAX ? CMD_MAX : n;
            memcpy(cmd_buf, d, len);
            cmd_buf[len] = 0;
            regs[0xC0] = 1;
            cmd_pending = true;
        }
        return;
    }
    if (reg == 0xC8) {
        if (n >= 2) {
            reply_off = d[0] | (d[1] << 8);
        }
        return;
    }
    if (reg == 0xF0) {
        if (n >= 1 && d[0] == 0xA5) {
            restart_pending = true;
        }
        return;
    }
    if (reg == 0x06) {                          // write 1 to clear
        portENTER_CRITICAL(&lock);
        regs[0x06] &= ~d[0];
        if (n >= 2) {
            regs[0x07] &= ~d[1];
        }
        portEXIT_CRITICAL(&lock);
        return;
    }
    if (reg == 0x10 && n >= 4) {
        time_set_value = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t) d[3] << 24);
        time_set_pending = true;
        return;
    }
    portENTER_CRITICAL(&lock);
    for (int i = 0; i < n; i++) {
        int r = reg + i;

        if (r > 0xff) {
            break;
        }
        if ((r >= 0x08 && r <= 0x0A) || r == 0x14 || r == 0x15 || (r >= 0x40 && r <= 0x8F) ||
            (r >= 0xA0 && r <= 0xA4)) {
            regs[r] = d[i];
            if (r >= 0xA0 && r <= 0xA4) {
                panel_dirty = true;
            } else if (r >= 0x40 && r <= 0x45) {
                fm_dirty = true;
                fm_save_ms = millis() | 1;
            } else if (r >= 0x46 && r <= 0x8F) {
                rds_dirty = true;
                fm_save_ms = millis() | 1;
            } else if (r == 0x0A) {
                panel_dirty = true;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
}

static void on_box_receive(int n) {
    uint8_t buf[CMD_MAX + 8];
    int len = 0;

    while (Wire.available() && len < (int) sizeof(buf)) {
        buf[len++] = Wire.read();
    }
    while (Wire.available()) {
        Wire.read();
    }
    if (len == 0) {
        return;
    }
    if (buf[0] == 0xFF) {
        if (len >= 3) {
            prepare_read(buf[1], buf[2]);
        }
        return;
    }
    handle_write(buf[0], buf + 1, len - 1);
}

// ---- software I2C master (open drain, external pull-ups) ----------------------------
class SoftI2C {
public:
    SoftI2C(int sda, int scl) : sda_((gpio_num_t) sda), scl_((gpio_num_t) scl) {}

    void begin(uint32_t khz) {
        half_us_ = khz >= 100 ? 4 : 500 / (khz ? khz : 1);
        setup(sda_);
        setup(scl_);
        recover();
    }

    // Pins back to plain inputs (bus handed to somebody else)
    void release() {
        gpio_set_direction(sda_, GPIO_MODE_INPUT);
        gpio_set_direction(scl_, GPIO_MODE_INPUT);
    }

    bool write(uint8_t addr, const uint8_t *d, size_t n, bool stop_after = true) {
        bool ok;

        start();
        ok = write_byte(addr << 1);
        for (size_t i = 0; ok && i < n; i++) {
            ok = write_byte(d[i]);
        }
        if (stop_after || !ok) {
            stop();
        }
        return ok;
    }

    bool read(uint8_t addr, uint8_t *d, size_t n) {
        bool ok;

        start();
        ok = write_byte((addr << 1) | 1);
        for (size_t i = 0; ok && i < n; i++) {
            d[i] = read_byte(i + 1 < n);
        }
        stop();
        return ok;
    }

    // write, repeated START, read
    bool write_read(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) {
        return write(addr, w, wn, false) && read(addr, r, rn);
    }

    bool probe(uint8_t addr) {
        return write(addr, nullptr, 0);
    }

private:
    gpio_num_t sda_, scl_;
    uint32_t half_us_ = 4;

    static void setup(gpio_num_t p) {
        gpio_reset_pin(p);
        gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_pull_mode(p, GPIO_PULLUP_ONLY);
        gpio_set_level(p, 1);
    }

    void dly() {
        delayMicroseconds(half_us_);
    }

    void sda(int v) {
        gpio_set_level(sda_, v);
    }

    void scl_low() {
        gpio_set_level(scl_, 0);
    }

    // Release SCL and wait while a slave stretches the clock (max 2 ms)
    void scl_high() {
        uint32_t t0 = micros();

        gpio_set_level(scl_, 1);
        while (!gpio_get_level(scl_) && micros() - t0 < 2000) {
        }
    }

    void start() {                          // also a repeated START
        sda(1);
        dly();
        scl_high();
        dly();
        sda(0);
        dly();
        scl_low();
        dly();
    }

    void stop() {
        sda(0);
        dly();
        scl_high();
        dly();
        sda(1);
        dly();
    }

    bool write_byte(uint8_t b) {
        bool ack;

        for (int i = 0; i < 8; i++) {
            sda((b & 0x80) != 0);
            b <<= 1;
            dly();
            scl_high();
            dly();
            scl_low();
        }
        sda(1);
        dly();
        scl_high();
        dly();
        ack = gpio_get_level(sda_) == 0;
        scl_low();
        dly();
        return ack;
    }

    uint8_t read_byte(bool ack) {
        uint8_t b = 0;

        sda(1);
        for (int i = 0; i < 8; i++) {
            dly();
            scl_high();
            dly();
            b = (b << 1) | gpio_get_level(sda_);
            scl_low();
        }
        sda(ack ? 0 : 1);
        dly();
        scl_high();
        dly();
        scl_low();
        sda(1);
        dly();
        return b;
    }

    // A slave left holding SDA low (reset in the middle of a read): clock it free
    void recover() {
        sda(1);
        for (int i = 0; i < 9 && !gpio_get_level(sda_); i++) {
            scl_low();
            dly();
            scl_high();
            dly();
        }
        stop();
    }
};

static SoftI2C panel_bus(PIN_PANEL_SDA, PIN_PANEL_SCL);
static SoftI2C sens(PIN_SENS_SDA, PIN_SENS_SCL);

// ---- front panel (FD650) ---------------------------------------------------------------
#define FD650_CTRL  0x24
#define FD650_KEY   0x27
#define FD650_DIG0  0x34

static bool panel_owned;
static uint8_t key_held;                    // FD650 key code while pressed, 0 = none
static uint32_t key_press_ms, key_repeat_ms, key_poll_ms;

static void panel_apply() {
    uint8_t v[5];

    portENTER_CRITICAL(&lock);
    memcpy(v, regs + 0xA0, 5);
    panel_dirty = false;
    portEXIT_CRITICAL(&lock);
    panel_bus.write(FD650_CTRL, v + 4, 1);
    for (int i = 0; i < 4; i++) {
        panel_bus.write(FD650_DIG0 + i, v + i, 1);
    }
}

static void panel_take(bool own) {
    if (own == panel_owned) {
        return;
    }
    panel_owned = own;
    if (own) {
        if (PIN_BYPASS >= 0) {
            digitalWrite(PIN_BYPASS, LOW);  // open the switch: the box no longer reaches the FD650
            delay(2);
        }
        uint8_t k;

        panel_bus.begin(100);
        set_present(P_PANEL, panel_bus.read(FD650_KEY, &k, 1));   // key read: a real FD650 command
        panel_apply();
    } else {
        panel_bus.release();
        if (PIN_BYPASS >= 0) {
            digitalWrite(PIN_BYPASS, HIGH); // box talks to the FD650 directly
        }
        set_present(P_PANEL, false);
    }
}

// ---- IR line: NEC frames to the box ----------------------------------------------------
// The RMT peripheral times the pulses (1 us ticks): WiFi and the I2C slave
// task would stretch delayMicroseconds() pulses. The pad is open drain
// with pull-up and its input stays on, so the line can be read while idle.
static bool ir_rmt;

static void ir_init() {
    rmt_data_t idle = { .duration0 = 1, .level0 = 1, .duration1 = 1, .level1 = 1 };

    ir_rmt = rmtInit(PIN_IR, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000) && rmtSetEOT(PIN_IR, HIGH);
    gpio_od_enable((gpio_num_t) PIN_IR);
    gpio_input_enable((gpio_num_t) PIN_IR);
    gpio_pullup_en((gpio_num_t) PIN_IR);
    // the channel idles low after init: one high symbol leaves it high (released)
    if (ir_rmt) {
        rmtWrite(PIN_IR, &idle, 1, 10);
    }
    if (!ir_rmt) {
        Serial.println("bridge: RMT init failed, no IR frames");
    }
}

// Line must have been idle (high) for 12 ms: don't talk over the remote
static bool ir_line_free() {
    uint32_t t0 = millis(), idle = millis();

    while (millis() - t0 < 300) {
        if (!gpio_get_level((gpio_num_t) PIN_IR)) {
            idle = millis();
        } else if (millis() - idle >= 12) {
            return true;
        }
    }
    return false;
}

static bool ir_send_nec(uint16_t user, uint8_t key) {
    uint32_t data = user | ((uint32_t) key << 16) | ((uint32_t) (uint8_t) ~key << 24);
    rmt_data_t sym[34];

    if (!ir_rmt || !ir_line_free()) {
        return false;
    }
    // each symbol: line low (mark) for duration0, then high for duration1
    sym[0] = { .duration0 = 9000, .level0 = 0, .duration1 = 4500, .level1 = 1 };
    for (int i = 0; i < 32; i++) {
        sym[1 + i] = { .duration0 = 560, .level0 = 0,
                       .duration1 = (uint16_t) ((data >> i) & 1 ? 1690 : 560), .level1 = 1 };
    }
    sym[33] = { .duration0 = 560, .level0 = 0, .duration1 = 100, .level1 = 1 };
    return rmtWrite(PIN_IR, sym, 34, 200);
}

static void front_keys_poll() {
    uint8_t cfg = regs[0x0A], b = 0;
    uint32_t now = millis();

    if (!panel_owned || now - key_poll_ms < 30) {
        return;
    }
    key_poll_ms = now;
    if (!panel_bus.read(FD650_KEY, &b, 1)) {
        return;
    }
    put8(0x0B, b);
    if (!(b & 0x40)) {                      // released
        key_held = 0;
        return;
    }
    b &= ~0x40;
    if (b != key_held) {                    // new press
        key_held = b;
        key_press_ms = key_repeat_ms = now;
        raise_event(EV_KEY);
        if (cfg & CFG_FRONT_IR) {
            ir_send_nec(IR_USER_FRONT, b);
        }
    } else if ((cfg & CFG_FRONT_IR) && now - key_repeat_ms >= 110) {
        key_repeat_ms = now;
        ir_send_nec(IR_USER_FRONT, b);      // held: the full frame again, as the stock remote does
    }
}

static void doorbell_poll() {
    static uint32_t last;

    if (!pending_bell || !(regs[0x0A] & CFG_BELL) || millis() - last < 250) {
        return;
    }
    if (ir_send_nec(IR_USER_EVENT, pending_bell & 0xff)) {
        pending_bell = 0;
        last = millis();
    }
}

// ---- DS1302 clock (3-wire, BCD registers) -----------------------------------------------
static void ds_write_byte(uint8_t b) {
    pinMode(PIN_DS_DAT, OUTPUT);
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_DS_DAT, b & 1);
        b >>= 1;
        delayMicroseconds(1);
        digitalWrite(PIN_DS_CLK, HIGH);
        delayMicroseconds(1);
        digitalWrite(PIN_DS_CLK, LOW);
    }
}

static uint8_t ds_read_byte() {
    uint8_t b = 0;

    pinMode(PIN_DS_DAT, INPUT);
    for (int i = 0; i < 8; i++) {
        delayMicroseconds(1);
        b |= digitalRead(PIN_DS_DAT) << i;
        digitalWrite(PIN_DS_CLK, HIGH);
        delayMicroseconds(1);
        digitalWrite(PIN_DS_CLK, LOW);
    }
    return b;
}

static void ds_begin_xfer(uint8_t cmd) {
    digitalWrite(PIN_DS_CLK, LOW);
    digitalWrite(PIN_DS_CE, HIGH);
    delayMicroseconds(4);
    ds_write_byte(cmd);
}

static void ds_end_xfer() {
    digitalWrite(PIN_DS_CE, LOW);
    delayMicroseconds(4);
}

static void ds_write_reg(uint8_t reg, uint8_t v) {  // reg = write command (even)
    ds_begin_xfer(reg);
    ds_write_byte(v);
    ds_end_xfer();
}

static uint8_t ds_read_reg(uint8_t reg) {           // reg = read command (odd)
    uint8_t v;

    ds_begin_xfer(reg);
    v = ds_read_byte();
    ds_end_xfer();
    return v;
}

static uint8_t bcd2bin(uint8_t v) {
    return (v >> 4) * 10 + (v & 0x0f);
}

static uint8_t bin2bcd(uint8_t v) {
    return ((v / 10) << 4) | (v % 10);
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian)
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned) (y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097LL + (int64_t) doe - 719468;
}

static bool ds_present() {
    ds_write_reg(0x8E, 0x00);                       // write protect off
    ds_write_reg(0xC0, 0xA5);                       // RAM byte 0
    if (ds_read_reg(0xC1) != 0xA5) {
        return false;
    }
    ds_write_reg(0xC0, 0x5A);
    return ds_read_reg(0xC1) == 0x5A;
}

// Unix time from the DS1302 (stored as UTC), 0 = not set / halted
static uint32_t ds_get() {
    uint8_t b[8];

    ds_begin_xfer(0xBF);                            // clock burst read
    for (int i = 0; i < 8; i++) {
        b[i] = ds_read_byte();
    }
    ds_end_xfer();
    if (b[0] & 0x80) {                              // clock halt: never set
        return 0;
    }
    int sec = bcd2bin(b[0] & 0x7f), min = bcd2bin(b[1] & 0x7f), hour = bcd2bin(b[2] & 0x3f);
    int day = bcd2bin(b[3] & 0x3f), mon = bcd2bin(b[4] & 0x1f), year = 2000 + bcd2bin(b[6]);

    if (year < 2024 || mon < 1 || mon > 12 || day < 1 || day > 31) {
        return 0;
    }
    return (uint32_t) (days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec);
}

static void ds_set(uint32_t t) {
    time_t tt = t;
    struct tm tm;

    gmtime_r(&tt, &tm);
    ds_write_reg(0x8E, 0x00);
    ds_begin_xfer(0xBE);                            // clock burst write
    ds_write_byte(bin2bcd(tm.tm_sec));              // bit 7 = 0: clock runs
    ds_write_byte(bin2bcd(tm.tm_min));
    ds_write_byte(bin2bcd(tm.tm_hour));             // 24 h mode
    ds_write_byte(bin2bcd(tm.tm_mday));
    ds_write_byte(bin2bcd(tm.tm_mon + 1));
    ds_write_byte(bin2bcd(tm.tm_wday ? tm.tm_wday : 7));
    ds_write_byte(bin2bcd(tm.tm_year % 100));
    ds_write_byte(0x00);                            // control: write protect off
    ds_end_xfer();
}

// ---- clock ---------------------------------------------------------------------------------
static volatile bool ntp_synced;

static void on_ntp_sync(struct timeval *tv) {
    (void) tv;
    ntp_synced = true;
}

static void set_system_time(uint32_t t, uint8_t source) {
    struct timeval tv = { (time_t) t, 0 };

    settimeofday(&tv, nullptr);
    put8(0x16, source);
    set_present(P_TIME, true);
    raise_event(EV_TIME);
}

static void clock_poll() {
    static uint32_t last;
    uint32_t now = millis();

    if (time_set_pending) {
        time_set_pending = false;
        set_system_time(time_set_value, 3);
        if (regs[0x05] & P_DS) {
            ds_set(time_set_value);
        }
    }
    if (ntp_synced) {
        ntp_synced = false;
        put8(0x16, 2);
        set_present(P_TIME, true);
        raise_event(EV_TIME);
        if (regs[0x05] & P_DS) {
            ds_set((uint32_t) time(nullptr));
        }
    }
    if (now - last < 250) {
        return;
    }
    last = now;
    put32(0x0C, now / 1000);
    put32(0x10, (regs[0x05] & P_TIME) ? (uint32_t) time(nullptr) : 0);
}

// ---- sensors -------------------------------------------------------------------------------
// AHT20: trigger, read 80 ms later
static uint32_t aht_ms;
static bool aht_measuring;

static bool aht_init() {
    uint8_t st;

    if (!sens.read(AHT_ADDR, &st, 1)) {
        return false;
    }
    if (!(st & 0x08)) {                             // not calibrated: initialise
        uint8_t c[3] = { 0xBE, 0x08, 0x00 };
        sens.write(AHT_ADDR, c, 3);
        delay(10);
    }
    return true;
}

static void aht_poll() {
    uint32_t now = millis();

    if (!(regs[0x05] & P_AHT)) {
        return;
    }
    if (!aht_measuring && now - aht_ms >= 2000) {
        uint8_t c[3] = { 0xAC, 0x33, 0x00 };

        if (sens.write(AHT_ADDR, c, 3)) {
            aht_measuring = true;
            aht_ms = now;
        }
    } else if (aht_measuring && now - aht_ms >= 80) {
        uint8_t b[7];

        if (sens.read(AHT_ADDR, b, 7) && !(b[0] & 0x80)) {
            uint32_t rh = ((uint32_t) b[1] << 12) | (b[2] << 4) | (b[3] >> 4);
            uint32_t t = ((uint32_t) (b[3] & 0x0f) << 16) | (b[4] << 8) | b[5];

            put16(0x20, (int16_t) ((int32_t) ((t * 20000ULL) >> 20) - 5000));
            put16(0x22, (uint16_t) ((rh * 10000ULL) >> 20));
            raise_event(EV_CLIMATE);
            aht_measuring = false;
            aht_ms = now;
        } else if (now - aht_ms > 500) {
            aht_measuring = false;
        }
    }
}

// BMP280: normal mode, Bosch integer compensation
static uint8_t bmp_addr;
static uint16_t dig_T1, dig_P1;
static int16_t dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

static bool bmp_init() {
    static const uint8_t addrs[2] = { 0x77, 0x76 };

    for (int k = 0; k < 2; k++) {
        uint8_t reg = 0xD0, id = 0, c[24];

        if (!sens.write_read(addrs[k], &reg, 1, &id, 1) || (id != 0x58 && id != 0x60)) {
            continue;
        }
        bmp_addr = addrs[k];
        reg = 0x88;
        if (!sens.write_read(bmp_addr, &reg, 1, c, 24)) {
            return false;
        }
        dig_T1 = c[0] | (c[1] << 8);
        dig_T2 = c[2] | (c[3] << 8);
        dig_T3 = c[4] | (c[5] << 8);
        dig_P1 = c[6] | (c[7] << 8);
        dig_P2 = c[8] | (c[9] << 8);
        dig_P3 = c[10] | (c[11] << 8);
        dig_P4 = c[12] | (c[13] << 8);
        dig_P5 = c[14] | (c[15] << 8);
        dig_P6 = c[16] | (c[17] << 8);
        dig_P7 = c[18] | (c[19] << 8);
        dig_P8 = c[20] | (c[21] << 8);
        dig_P9 = c[22] | (c[23] << 8);
        uint8_t cfg[2] = { 0xF5, 0xA8 };            // standby 1 s, IIR filter 4
        uint8_t meas[2] = { 0xF4, 0x57 };           // temp x2, pressure x16, normal mode
        sens.write(bmp_addr, cfg, 2);
        sens.write(bmp_addr, meas, 2);
        return true;
    }
    return false;
}

static void bmp_poll() {
    static uint32_t last;
    uint8_t reg = 0xF7, b[6];

    if (!(regs[0x05] & P_BMP) || millis() - last < 1000) {
        return;
    }
    last = millis();
    if (!sens.write_read(bmp_addr, &reg, 1, b, 6)) {
        return;
    }
    int32_t adc_p = ((int32_t) b[0] << 12) | (b[1] << 4) | (b[2] >> 4);
    int32_t adc_t = ((int32_t) b[3] << 12) | (b[4] << 4) | (b[5] >> 4);
    int32_t v1 = ((((adc_t >> 3) - ((int32_t) dig_T1 << 1))) * dig_T2) >> 11;
    int32_t v2 = (((((adc_t >> 4) - (int32_t) dig_T1) * ((adc_t >> 4) - (int32_t) dig_T1)) >> 12) *
                  dig_T3) >> 14;
    int32_t t_fine = v1 + v2;
    int64_t p1 = (int64_t) t_fine - 128000, p2, p;

    put16(0x28, (int16_t) ((t_fine * 5 + 128) >> 8));
    p2 = p1 * p1 * dig_P6;
    p2 += (p1 * dig_P5) << 17;
    p2 += ((int64_t) dig_P4) << 35;
    p1 = ((p1 * p1 * dig_P3) >> 8) + ((p1 * dig_P2) << 12);
    p1 = ((((int64_t) 1) << 47) + p1) * dig_P1 >> 33;
    if (p1 == 0) {
        return;
    }
    p = 1048576 - adc_p;
    p = (((p << 31) - p2) * 3125) / p1;
    p1 = ((int64_t) dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    p2 = ((int64_t) dig_P8 * p) >> 19;
    p = ((p + p1 + p2) >> 8) + (((int64_t) dig_P7) << 4);
    put32(0x24, (uint32_t) (p / 256));
    raise_event(EV_CLIMATE);
}

static void local_adc_poll() {
    static uint32_t last;

    if (millis() - last < 100) {
        return;
    }
    last = millis();
    put16(0x38, analogReadMilliVolts(PIN_FREE0));
    put16(0x3A, analogReadMilliVolts(PIN_FREE1));
    put16(0x3C, (uint16_t) (int16_t) (temperatureRead() * 100));
    raise_event(EV_ADC);
}

// ---- SI4713 FM transmitter ------------------------------------------------------------------
static bool si_cmd(const uint8_t *cmd, size_t n, uint8_t *resp = nullptr, size_t rn = 0,
                   uint32_t timeout_ms = 500) {
    uint8_t st = 0;
    uint32_t t0 = millis();

    if (!sens.write(SI_ADDR, cmd, n)) {
        return false;
    }
    do {                                            // wait for CTS (bit 7)
        if (!sens.read(SI_ADDR, &st, 1)) {
            return false;
        }
        if (st & 0x80) {
            break;
        }
        delay(1);
    } while (millis() - t0 < timeout_ms);
    if (!(st & 0x80)) {
        return false;
    }
    return rn ? sens.read(SI_ADDR, resp, rn) : true;
}

static bool si_prop(uint16_t prop, uint16_t val) {
    uint8_t c[6] = { 0x12, 0x00, (uint8_t) (prop >> 8), (uint8_t) prop, (uint8_t) (val >> 8), (uint8_t) val };

    return si_cmd(c, 6);
}

// Wait for "seek/tune complete", then read TX_TUNE_STATUS (clears it)
static bool si_wait_tune(uint8_t *status8) {
    uint32_t t0 = millis();
    uint8_t c = 0x14, st[1];

    while (millis() - t0 < 500) {
        if (si_cmd(&c, 1, st, 1) && (st[0] & 0x01)) {
            uint8_t ts[2] = { 0x33, 0x01 };

            return si_cmd(ts, 2, status8, 8);
        }
        delay(5);
    }
    return false;
}

static bool si_init() {
    uint8_t up[3] = { 0x01, 0x12, 0x50 };   // power up: crystal, transmit, analog inputs
    uint8_t rev = 0x10, r[9];

    for (int i = 0; i < (int) SI_N; i++) {
        si_val[i] = si_table[i].def;        // NVS values come in setup()
    }
    pinMode(PIN_SI_RST, OUTPUT);
    digitalWrite(PIN_SI_RST, LOW);
    delay(10);
    digitalWrite(PIN_SI_RST, HIGH);
    delay(20);
    if (!si_cmd(up, 3, nullptr, 0, 1000)) {
        return false;
    }
    delay(200);
    if (!si_cmd(&rev, 1, r, 9) || r[1] != 13) {    // part number 13 = Si4713
        return false;
    }
    si_prop(0x0201, 32768);                 // REFCLK: 32.768 kHz crystal
    si_prop(0x2C04, 3);                     // PS repeat count
    si_prop(0x2C05, 1);                     // one PS message
    si_prop(0x2C07, 0);                     // no RDS FIFO (CT turns it on)
    siprop_dirty = true;
    return true;
}

// PS misc with the stereo flag (DI bit d0) following the mono setting
static uint16_t si_misc() {
    int k = si_index(0x2C03);
    uint16_t v = si_val[k];

    return (regs[0x43] & 4) ? (v & ~0x1000) : (v | 0x1000);
}

static void si_apply_props() {
    uint16_t v[SI_N];

    portENTER_CRITICAL(&lock);
    memcpy(v, si_val, sizeof(v));
    siprop_dirty = false;
    portEXIT_CRITICAL(&lock);
    if (!(regs[0x05] & P_SI)) {
        return;
    }
    for (int i = 0; i < (int) SI_N; i++) {
        si_prop(si_table[i].prop, si_table[i].prop == 0x2C03 ? si_misc() : v[i]);
    }
    raise_event(EV_FM);
}

static void si_update_status(const uint8_t *s) {
    put8(0x30, s[7]);                       // noise level dBuV
    put8(0x31, s[6]);                       // antenna capacitor
}

static bool ct_fifo;                        // RDS FIFO reserved for CT groups

static void fm_apply() {
    uint8_t v[6], s[8];
    uint16_t freq;

    portENTER_CRITICAL(&lock);
    memcpy(v, regs + 0x40, 6);
    fm_dirty = false;
    portEXIT_CRITICAL(&lock);
    if (!(regs[0x05] & P_SI)) {
        return;
    }
    if (((v[5] & 1) != 0) != ct_fifo) {
        ct_fifo = !ct_fifo;
        si_prop(0x2C07, ct_fifo ? 4 : 0);   // blocks for one 4A group (+1)
        rds_dirty = true;                   // the RadioText buffer is reloaded
    }
    freq = v[0] | (v[1] << 8);
    freq -= freq % 5;                       // the SI4713 tunes in 50 kHz steps
    if ((v[3] & 1) && freq >= 7600 && freq <= 10800) {
        uint8_t tune[4] = { 0x30, 0x00, (uint8_t) (freq >> 8), (uint8_t) freq };
        uint8_t pwr = v[2] < 88 ? 88 : v[2] > 115 ? 115 : v[2];
        uint8_t power[5] = { 0x31, 0x00, 0x00, pwr, (uint8_t) (v[4] > 191 ? 0 : v[4]) };

        // components: bit0 stereo pilot, bit1 L-R (stereo), bit2 RDS
        si_prop(0x2100, ((v[3] & 4) ? 0 : 0x0003) | ((v[3] & 2) ? 0x0004 : 0));
        si_prop(0x2C03, si_misc());
        if (si_cmd(tune, 4) && si_wait_tune(s)) {
            si_update_status(s);
        }
        if (si_cmd(power, 5) && si_wait_tune(s)) {
            si_update_status(s);
        }
    } else {
        uint8_t off[5] = { 0x31, 0x00, 0x00, 0x00, 0x00 };

        si_cmd(off, 5);
        si_wait_tune(s);
    }
    raise_event(EV_FM);
}

static void rds_apply() {
    static char last_rt[64];
    static uint8_t ab;                      // RadioText A/B flag: flips when the text changes
    char ps[8], rt[64];
    uint16_t pi;

    portENTER_CRITICAL(&lock);
    pi = regs[0x46] | (regs[0x47] << 8);
    memcpy(ps, regs + 0x48, 8);
    memcpy(rt, regs + 0x50, 64);
    rds_dirty = false;
    portEXIT_CRITICAL(&lock);
    if (!(regs[0x05] & P_SI)) {
        return;
    }
    for (int i = 0; i < 8; i++) {
        ps[i] = ps[i] ? ps[i] : ' ';
    }
    for (int i = 0; i < 64; i++) {
        rt[i] = rt[i] ? rt[i] : ' ';
    }
    if (memcmp(rt, last_rt, 64) != 0) {
        memcpy(last_rt, rt, 64);
        ab ^= 0x10;
    }
    si_prop(0x2C01, pi ? pi : 0x7A42);
    for (int i = 0; i < 2; i++) {
        uint8_t c[6] = { 0x36, (uint8_t) i, (uint8_t) ps[4 * i], (uint8_t) ps[4 * i + 1],
                         (uint8_t) ps[4 * i + 2], (uint8_t) ps[4 * i + 3] };
        si_cmd(c, 6);
    }
    // RadioText, group 2A, 4 characters per segment (block B low bits: A/B flag, segment);
    // PTY and TP in block B come from PS misc (force bit)
    for (int i = 0; i < 16; i++) {
        uint8_t c[8] = { 0x35, (uint8_t) (i == 0 ? 0x06 : 0x04), 0x20, (uint8_t) (ab | i),
                         (uint8_t) rt[4 * i], (uint8_t) rt[4 * i + 1], (uint8_t) rt[4 * i + 2],
                         (uint8_t) rt[4 * i + 3] };
        si_cmd(c, 8);
    }
    raise_event(EV_FM);
}

// Audio input level and overmodulation, 4x a second while on air
static void asq_poll() {
    static uint32_t last, overmod_ms;
    uint8_t c[2] = { 0x34, 0x01 }, r[5];    // TX_ASQ_STATUS, clear the flags

    if (!(regs[0x05] & P_SI) || !(regs[0x43] & 1) || millis() - last < 250) {
        return;
    }
    last = millis();
    if (!si_cmd(c, 2, r, 5)) {
        return;
    }
    if (r[1] & 0x04) {
        overmod_ms = millis() | 1;
    }
    put8(0x32, (r[1] & 0x03) | (overmod_ms && millis() - overmod_ms < 1000 ? 0x04 : 0));
    put8(0x33, r[4]);                       // dBFS, signed
}

// RDS clock time: group 4A through the FIFO at each new minute (UTC + offset
// in half hours, as the standard wants)
static void ct_poll() {
    static uint32_t last_min;
    time_t now = time(nullptr);
    uint32_t min = (uint32_t) now / 60, mjd, hour, minute;
    int off = (int16_t) get16(0x14);
    uint16_t b, c, d;

    if (!ct_fifo || !(regs[0x43] & 1) || !(regs[0x43] & 2) || !(regs[0x05] & P_TIME) ||
        min == last_min || now % 60 > 5) {
        return;
    }
    last_min = min;
    mjd = (uint32_t) now / 86400 + 40587;
    hour = (uint32_t) now / 3600 % 24;
    minute = min % 60;
    off = (off + (off < 0 ? -15 : 15)) / 30;
    b = 0x4000 | ((mjd >> 15) & 0x03);
    c = (uint16_t) (((mjd & 0x7FFF) << 1) | (hour >> 4));
    d = (uint16_t) (((hour & 0x0F) << 12) | (minute << 6) | (off < 0 ? 0x20 : 0) |
                    ((off < 0 ? -off : off) & 0x1F));
    uint8_t cmd[8] = { 0x35, 0x84, (uint8_t) (b >> 8), (uint8_t) b, (uint8_t) (c >> 8),
                       (uint8_t) c, (uint8_t) (d >> 8), (uint8_t) d };
    si_cmd(cmd, 8);
}

// ---- mailbox ---------------------------------------------------------------------------------
static void rep(const char *fmt, ...) {
    va_list ap;
    int n;

    if (reply_len >= REPLY_MAX - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(reply + reply_len, REPLY_MAX - reply_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        reply_len += n;
        if (reply_len > REPLY_MAX - 1) {
            reply_len = REPLY_MAX - 1;
        }
    }
}

// Word i of the command (0 = command name); rest = everything from word i on
static String arg(const String &line, int i, bool rest = false) {
    int pos = 0, n = line.length();

    for (int k = 0; k < i; k++) {
        while (pos < n && line[pos] != ' ') {
            pos++;
        }
        while (pos < n && line[pos] == ' ') {
            pos++;
        }
    }
    if (rest) {
        return line.substring(pos);
    }
    int end = line.indexOf(' ', pos);
    return line.substring(pos, end < 0 ? n : end);
}

static int parse_hex_bytes(const String &s, uint8_t *out, int max) {
    int n = 0;

    for (int i = 0; i + 1 < (int) s.length() && n < max; i += 2) {
        out[n++] = (uint8_t) strtoul(s.substring(i, i + 2).c_str(), nullptr, 16);
    }
    return n;
}

static Preferences prefs;

static void wifi_connect(const String &ssid, const String &pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    WiFi.setTxPower(WIFI_POWER_8_5dBm);     // Super Mini boards often fail at full power
}

static void ntp_start() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
}

static bool cmd_run(const String &line) {
    String c = arg(line, 0);

    if (c.indexOf('\t') >= 0) {                     // "WIFI<tab>..."
        c = c.substring(0, c.indexOf('\t'));
    }
    c.toUpperCase();
    if (c == "PING") {
        rep("PONG");
    } else if (c == "VER") {
        rep("NCAPPS bridge v%d, %s rev %d, %u KB free heap", FW_VERSION, ESP.getChipModel(),
            ESP.getChipRevision(), ESP.getFreeHeap() / 1024);
    } else if (c == "WIFI") {
        String ssid, pass;
        int t1 = line.indexOf('\t'), t2 = t1 < 0 ? -1 : line.indexOf('\t', t1 + 1);
        wl_status_t st;

        if (t1 >= 0) {                              // WIFI<tab>ssid<tab>password
            ssid = line.substring(t1 + 1, t2 < 0 ? line.length() : t2);
            pass = t2 < 0 ? String("") : line.substring(t2 + 1);
        } else {
            ssid = arg(line, 1);
            pass = arg(line, 2, true);
        }
        if (!ssid.length()) {
            rep("usage: WIFI ssid password");
            return false;
        }
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        WiFi.disconnect();
        wifi_connect(ssid, pass);
        for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
            delay(250);
        }
        st = WiFi.status();
        if (st != WL_CONNECTED) {
            rep("not connected: %s", st == WL_NO_SSID_AVAIL ? "network not found" :
                st == WL_CONNECT_FAILED || st == WL_DISCONNECTED ? "wrong password?" :
                "no answer");
            return false;
        }
        rep("OK %s", WiFi.localIP().toString().c_str());
    } else if (c == "WIFI?") {
        if (WiFi.status() == WL_CONNECTED) {
            rep("CONNECTED %s %d dBm %s", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                WiFi.SSID().c_str());
        } else {
            rep("DISCONNECTED %s", prefs.getString("ssid", "").c_str());
        }
    } else if (c == "WIFIOFF") {
        WiFi.disconnect(true);
        prefs.remove("ssid");
        rep("OK");
    } else if (c == "SCAN") {
        int n = WiFi.scanNetworks();

        for (int i = 0; i < n; i++) {
            rep("%d %d %d %s\n", WiFi.RSSI(i), WiFi.channel(i),
                WiFi.encryptionType(i) != WIFI_AUTH_OPEN, WiFi.SSID(i).c_str());
        }
        WiFi.scanDelete();
    } else if (c == "NTP") {
        String tz = arg(line, 1);
        uint32_t t0 = millis();

        if (tz.length()) {
            put16(0x14, (uint16_t) (int16_t) tz.toInt());
            prefs.putShort("tz", (int16_t) tz.toInt());
        }
        if (WiFi.status() != WL_CONNECTED) {
            rep("no WiFi");
            return false;
        }
        ntp_synced = false;
        ntp_start();
        while (!ntp_synced && millis() - t0 < 8000) {
            delay(50);
        }
        if (!ntp_synced) {
            rep("no answer from NTP");
            return false;
        }
        clock_poll();
        rep("OK %lu", (unsigned long) time(nullptr));
    } else if (c == "HTTP") {
        String url = arg(line, 1);
        HTTPClient http;
        WiFiClient plain;
        WiFiClientSecure tls;
        int code;

        if (WiFi.status() != WL_CONNECTED) {
            rep("no WiFi");
            return false;
        }
        tls.setInsecure();
        if (!(url.startsWith("https:") ? http.begin(tls, url) : http.begin(plain, url))) {
            rep("bad url");
            return false;
        }
        http.setTimeout(8000);
        code = http.GET();
        rep("%d %d\n", code, http.getSize());
        if (code > 0) {
            WiFiClient *s = http.getStreamPtr();
            uint32_t t0 = millis();
            int left = http.getSize();

            while (http.connected() && (left > 0 || left == -1) && reply_len < REPLY_MAX - 1 &&
                   millis() - t0 < 8000) {
                int avail = s->available();

                if (avail > 0) {
                    int n = s->readBytes(reply + reply_len,
                                         min((size_t) avail, (size_t) (REPLY_MAX - 1 - reply_len)));
                    reply_len += n;
                    if (left > 0) {
                        left -= n;
                    }
                } else {
                    delay(2);
                }
            }
        }
        http.end();
        return code > 0;
    } else if (c == "FMDEFAULTS") {
        portENTER_CRITICAL(&lock);
        for (int i = 0; i < (int) SI_N; i++) {
            si_val[i] = si_table[i].def;
        }
        regs[0x43] &= ~4;                           // stereo
        regs[0x44] = 0;                             // automatic antenna tuning
        regs[0x45] = 0;                             // no CT
        siprop_dirty = true;
        fm_dirty = true;
        fm_save_ms = millis() | 1;
        portEXIT_CRITICAL(&lock);
        rep("OK");
    } else if (c == "FMSCAN") {
        uint16_t best_f[5] = { 0 };
        uint8_t best_n[5] = { 255, 255, 255, 255, 255 }, s[8];

        if (!(regs[0x05] & P_SI)) {
            rep("no SI4713");
            return false;
        }
        for (uint16_t f = 8750; f <= 10800; f += 10) {
            uint8_t m[5] = { 0x32, 0x00, (uint8_t) (f >> 8), (uint8_t) f, 0x00 };

            if (!si_cmd(m, 5) || !si_wait_tune(s)) {
                continue;
            }
            for (int k = 0; k < 5; k++) {           // keep the 5 quietest
                if (s[7] < best_n[k]) {
                    for (int j = 4; j > k; j--) {
                        best_n[j] = best_n[j - 1];
                        best_f[j] = best_f[j - 1];
                    }
                    best_n[k] = s[7];
                    best_f[k] = f;
                    break;
                }
            }
        }
        for (int k = 0; k < 5 && best_f[k]; k++) {
            rep("%u %u\n", best_f[k], best_n[k]);
        }
        fm_dirty = true;                            // back to the chosen frequency
    } else if (c == "I2CW" || c == "I2CR" || c == "I2CWR") {
        uint8_t addr = (uint8_t) strtoul(arg(line, 1).c_str(), nullptr, 16), w[32], r[32];
        int wn = 0, rn = 0;
        bool ok;

        if (c != "I2CR") {
            wn = parse_hex_bytes(arg(line, 2), w, sizeof(w));
        }
        if (c == "I2CR") {
            rn = arg(line, 2).toInt();
        } else if (c == "I2CWR") {
            rn = arg(line, 3).toInt();
        }
        rn = rn > 32 ? 32 : rn;
        ok = c == "I2CW" ? sens.write(addr, w, wn) :
             c == "I2CR" ? sens.read(addr, r, rn) : sens.write_read(addr, w, wn, r, rn);
        rep("%s", ok ? "OK" : "NACK");
        for (int i = 0; ok && i < rn; i++) {
            rep(" %02x", r[i]);
        }
        return ok;
    } else if (c == "PIN" || c == "GET") {
        int pin = arg(line, 1).toInt();

        if (pin != PIN_FREE0 && pin != PIN_FREE1) {
            rep("only GPIO%d and GPIO%d are free", PIN_FREE0, PIN_FREE1);
            return false;
        }
        if (c == "PIN") {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, arg(line, 2).toInt() ? HIGH : LOW);
            rep("OK");
        } else {
            pinMode(pin, INPUT_PULLUP);
            rep("%d", digitalRead(pin));
        }
    } else {
        rep("unknown command: %s", c.c_str());
        return false;
    }
    return true;
}

static void mailbox_poll() {
    static uint8_t done_count;
    char cmd[CMD_MAX + 1];
    bool ok;

    if (!cmd_pending) {
        return;
    }
    portENTER_CRITICAL(&lock);
    memcpy(cmd, cmd_buf, sizeof(cmd));
    cmd_pending = false;
    portEXIT_CRITICAL(&lock);
    reply_len = 0;
    reply_off = 0;
    if (!strncasecmp(cmd, "WIFI", 4) && (cmd[4] == ' ' || cmd[4] == '\t')) {
        Serial.println("bridge: command WIFI (password not logged)");
    } else {
        Serial.printf("bridge: command \"%s\"\n", cmd);
    }
    ok = cmd_run(String(cmd));
    reply[reply_len] = 0;
    put16(0xC1, (uint16_t) reply_len);
    put8(0xC3, ++done_count);
    put8(0xC0, ok ? 2 : 3);                         // status last: the reply is complete
    raise_event(EV_MAIL);
}

// FM / RDS registers written by the box: into NVS once they stop changing
static void fm_save_poll() {
    uint8_t v[0x90 - 0x40];
    uint16_t p[SI_N];
    uint32_t t = fm_save_ms;

    if (!t || millis() - t < 3000) {
        return;
    }
    portENTER_CRITICAL(&lock);
    memcpy(v, regs + 0x40, sizeof(v));
    memcpy(p, si_val, sizeof(p));
    if (fm_save_ms == t) {
        fm_save_ms = 0;
    }
    portEXIT_CRITICAL(&lock);
    prefs.putBytes("fm", v, 6);
    prefs.putBytes("rds", v + 6, sizeof(v) - 6);
    prefs.putBytes("siprop", p, sizeof(p));
    Serial.println("bridge: FM settings saved");
}

// Time zone written by the box: keep it in NVS
static void tz_poll() {
    static int16_t saved = 0x7fff;
    int16_t tz = (int16_t) get16(0x14);

    if (saved == 0x7fff) {
        saved = tz;
    } else if (tz != saved) {
        saved = tz;
        prefs.putShort("tz", tz);
    }
}

// ---- WiFi state ------------------------------------------------------------------------------
static void wifi_poll() {
    static bool was;
    bool now = WiFi.status() == WL_CONNECTED;

    if (now != was) {
        was = now;
        set_present(P_WIFI, now);
        raise_event(EV_WIFI);
        if (now) {
            Serial.printf("bridge: WiFi %s\n", WiFi.localIP().toString().c_str());
            ntp_start();
        }
    }
}

// ---- setup / loop ------------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("bridge: NCAPPS ESP32-C3 bridge starting");

    memcpy(regs, "NCB1", 4);
    regs[0x04] = FW_VERSION;
    regs[0x08] = EV_MAIL;                           // doorbell: mailbox done
    regs[0x0A] = CFG_FRONT_IR | CFG_BELL | (PIN_BYPASS < 0 ? CFG_PANEL_OWN : 0);
    regs[0xA1] = regs[0xA2] = regs[0xA3] = 0x02;    // "---" until the box writes (segment g)
    regs[0xA4] = 0x41;                              // display on

    // IR line: RMT output, open drain with pull-up, idle released
    ir_init();

    if (PIN_BYPASS >= 0) {
        pinMode(PIN_BYPASS, OUTPUT);
        digitalWrite(PIN_BYPASS, HIGH);             // closed: the box reaches the FD650
    }

    // DS1302
    pinMode(PIN_DS_CE, OUTPUT);
    pinMode(PIN_DS_CLK, OUTPUT);
    digitalWrite(PIN_DS_CE, LOW);
    digitalWrite(PIN_DS_CLK, LOW);
    if (ds_present()) {
        uint32_t t = ds_get();

        set_present(P_DS, true);
        if (t) {
            set_system_time(t, 1);
        }
    }

    // sensor bus
    sens.begin(100);
    set_present(P_AHT, aht_init());
    set_present(P_BMP, bmp_init());
    set_present(P_SI, si_init());

    // front panel
    panel_take((regs[0x0A] & CFG_PANEL_OWN) != 0);

    // time zone, FM / RDS settings, WiFi from NVS
    prefs.begin("bridge", false);
    put16(0x14, (uint16_t) prefs.getShort("tz", 420));
    if (prefs.getBytesLength("fm") == 6) {
        prefs.getBytes("fm", regs + 0x40, 6);
    }
    if (prefs.getBytesLength("siprop") == sizeof(si_val)) {
        prefs.getBytes("siprop", si_val, sizeof(si_val));
    }
    siprop_dirty = true;
    if (prefs.getBytesLength("rds") == 0x90 - 0x46) {
        prefs.getBytes("rds", regs + 0x46, 0x90 - 0x46);
        rds_dirty = true;
    }
    fm_dirty = true;                                // saved settings, or carrier off
    sntp_set_time_sync_notification_cb(on_ntp_sync);
    if (prefs.getString("ssid", "").length()) {
        wifi_connect(prefs.getString("ssid", ""), prefs.getString("pass", ""));
    }

    // box link last: from here on the box can talk to us
    Wire.onReceive(on_box_receive);
    Wire.begin((uint8_t) BOX_ADDR, PIN_BOX_SDA, PIN_BOX_SCL, 100000);

    Serial.printf("bridge: present 0x%02x (AHT20 %d, BMP280 %d at 0x%02x, SI4713 %d, DS1302 %d, "
                  "panel %d)\n", regs[0x05], !!(regs[0x05] & P_AHT), !!(regs[0x05] & P_BMP),
                  bmp_addr, !!(regs[0x05] & P_SI), !!(regs[0x05] & P_DS), !!(regs[0x05] & P_PANEL));
}

void loop() {
    if (restart_pending) {
        delay(50);
        ESP.restart();
    }
    if (((regs[0x0A] & CFG_PANEL_OWN) != 0) != panel_owned) {
        panel_take(!panel_owned);
    }
    if (panel_dirty && panel_owned) {
        panel_apply();
    }
    front_keys_poll();
    if (siprop_dirty) {
        si_apply_props();
    }
    if (fm_dirty) {
        fm_apply();
    }
    if (rds_dirty) {
        rds_apply();
    }
    asq_poll();
    ct_poll();
    aht_poll();
    bmp_poll();
    local_adc_poll();
    clock_poll();
    wifi_poll();
    tz_poll();
    fm_save_poll();
    mailbox_poll();
    doorbell_poll();
    delay(1);
}
