// ESP32-C3 (Super Mini) side of the box link, Arduino-ESP32 3.x.
// Same line protocol as pico/code.py, but over the C3's own USB-C port
// (USB Serial/JTAG), plugged into the box's USB (through the hub).
//
// Arduino IDE: board "ESP32C3 Dev Module", Tools -> USB CDC On Boot:
// Enabled (so Serial = the USB port). Upload, then move the C3 to the box.
//
// Protocol: the box sends one command line ending in "\n". The C3 always
// answers with zero or more lines and then a line ">" so the box knows the
// reply is complete. Lines end in "\n" only (no "\r"): picoterm needs that
// to spot the ">" line.
//
// Commands:
//   PING                  -> PONG
//   LED 0|1               onboard LED (GPIO8, active low)
//   TEMP                  chip temperature (deg C)
//   ADC n                 raw 12-bit reading of GPIOn (0..4)
//   PIN n 0|1             drive GPIOn as output (stays set)
//   GET n                 read GPIOn as input with pull-up
//   WIFI ssid password    connect, prints IP
//   IP                    current IP (or "not connected")
//   HTTP url              GET url, prints status + first 1024 bytes of body
//                         (https: certificate is NOT checked)
//   HELP                  list commands

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define LED_PIN 8

static String line;

static void out (const String &s) {
    for (size_t i = 0; i < s.length (); i++) {
        if (s[i] != '\r') {
            Serial.print (s[i]);
        }
    }
    Serial.print ('\n');
}

static void done () {
    Serial.print (">\n");
    Serial.flush ();
}

// Word number i of the command line (0 = command)
static String arg (int i, bool rest = false) {
    int pos = 0, n = line.length ();

    for (int k = 0; k < i; k++) {
        while (pos < n && line[pos] != ' ') {
            pos++;
        }
        while (pos < n && line[pos] == ' ') {
            pos++;
        }
    }
    if (rest) {
        return line.substring (pos);
    }
    int end = line.indexOf (' ', pos);
    return line.substring (pos, end < 0 ? n : end);
}

static void cmd_wifi () {
    String ssid = arg (1), pass = arg (2, true);

    if (!ssid.length () || !pass.length ()) {
        out ("ERR usage: WIFI ssid password");
        return;
    }
    WiFi.mode (WIFI_STA);
    WiFi.begin (ssid.c_str (), pass.c_str ());
    // Super Mini boards often fail to connect at full TX power
    WiFi.setTxPower (WIFI_POWER_8_5dBm);
    for (int i = 0; i < 60 && WiFi.status () != WL_CONNECTED; i++) {
        delay (250);
    }
    if (WiFi.status () == WL_CONNECTED) {
        out ("OK " + WiFi.localIP ().toString ());
    } else {
        out ("ERR status " + String (WiFi.status ()));
    }
}

static void cmd_http () {
    String url = arg (1);
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure tls;
    bool ok;

    if (!url.length ()) {
        out ("ERR usage: HTTP url");
        return;
    }
    if (WiFi.status () != WL_CONNECTED) {
        out ("ERR WiFi not connected (use WIFI ssid password)");
        return;
    }
    if (url.startsWith ("https://")) {
        tls.setInsecure ();
        ok = http.begin (tls, url);
    } else if (url.startsWith ("http://")) {
        ok = http.begin (plain, url);
    } else {
        out ("ERR url must start with http:// or https://");
        return;
    }
    if (!ok) {
        out ("ERR bad url");
        return;
    }
    http.setTimeout (10000);
    // HTTP/1.0: no chunked encoding, so the raw stream below is just the body
    http.useHTTP10 (true);
    int code = http.GET ();
    if (code <= 0) {
        out ("ERR " + http.errorToString (code));
        http.end ();
        return;
    }
    out ("STATUS " + String (code));

    WiFiClient *s = http.getStreamPtr ();
    String body;
    unsigned long t0 = millis ();
    while (body.length () < 1024 && millis () - t0 < 5000 &&
           (s->connected () || s->available ())) {
        if (s->available ()) {
            body += (char) s->read ();
        } else {
            delay (1);
        }
    }
    http.end ();

    int start = 0;
    while (start <= (int) body.length ()) {
        int nl = body.indexOf ('\n', start);
        if (nl < 0) {
            if (start < (int) body.length ()) {
                out (body.substring (start));
            }
            break;
        }
        out (body.substring (start, nl));
        start = nl + 1;
    }
}

static void handle () {
    String c = arg (0);

    c.toUpperCase ();
    if (!c.length ()) {
        return;
    } else if (c == "PING") {
        out ("PONG");
    } else if (c == "LED") {
        digitalWrite (LED_PIN, arg (1) == "1" ? LOW : HIGH);
        out ("OK");
    } else if (c == "TEMP") {
        out (String (temperatureRead (), 1));
    } else if (c == "ADC") {
        out (String (analogRead (arg (1).toInt ())));
    } else if (c == "PIN") {
        int p = arg (1).toInt ();
        pinMode (p, OUTPUT);
        digitalWrite (p, arg (2) == "1" ? HIGH : LOW);
        out ("OK");
    } else if (c == "GET") {
        int p = arg (1).toInt ();
        pinMode (p, INPUT_PULLUP);
        out (digitalRead (p) ? "1" : "0");
    } else if (c == "WIFI") {
        cmd_wifi ();
    } else if (c == "IP") {
        out (WiFi.status () == WL_CONNECTED ? WiFi.localIP ().toString () : String ("not connected"));
    } else if (c == "HTTP") {
        cmd_http ();
    } else if (c == "HELP") {
        out ("PING  LED 0|1  TEMP  ADC n  PIN n 0|1  GET n");
        out ("WIFI ssid pass  IP  HTTP url");
    } else {
        out ("ERR unknown command " + c);
    }
}

void setup () {
    Serial.setTxBufferSize (4096);
    Serial.setRxBufferSize (512);
    Serial.begin (115200);          // speed ignored on USB
    pinMode (LED_PIN, OUTPUT);
    digitalWrite (LED_PIN, LOW);
    delay (200);
    digitalWrite (LED_PIN, HIGH);
}

void loop () {
    while (Serial.available ()) {
        char ch = Serial.read ();

        if (ch == '\n') {
            line.trim ();
            handle ();
            done ();
            line = "";
        } else if (ch != '\r' && line.length () < 300) {
            line += ch;
        }
    }
    delay (1);
}
