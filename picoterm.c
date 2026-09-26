/*
 * picoterm: talk to the WiFi coprocessor. Uses an ESP32-C3 on USB
 * (esp32c3/esp32c3.ino, USB Serial/JTAG) if one is plugged in, else the
 * Pico 2 W (pico/code.py) through the CH340.
 *
 *   usb start
 *   go ${a} [baud]          CH340 only: default 921600 (= BAUD in pico/code.py)
 *
 * Type a command (e.g. PING, TEMP, LED 1, WIFI ssid pass, HTTP url) and
 * press Enter; the reply is printed up to its ">" line.
 * Empty line or Ctrl-C quits.
 */
#include "uboot.h"
#include "ch340.h"
#include "cdcacm.h"

static int use_cdc;

static int link_read(void *data, int max) {
    return use_cdc ? cdc_read(data, max) : ch340_read(data, max);
}

static int link_write(const void *data, int n) {
    return use_cdc ? cdc_write(data, n) : ch340_write(data, n);
}

static u32 parse_dec(const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

/* Read the console until Enter; returns length (0 = quit) */
static int read_line(char *line, int max) {
    int n = 0;

    for (;;) {
        int c = getc();

        if (c == 3) {                   /* Ctrl-C */
            return 0;
        }
        if (c == '\r' || c == '\n') {
            puts("\n");
            return n;
        }
        if ((c == 8 || c == 127) && n > 0) {
            n--;
            puts("\b \b");
        } else if (c >= ' ' && n < max - 2) {
            line[n++] = c;
            putc(c);
        }
    }
}

/* Print the reply until a line that is just ">"; returns 0 or -1.
 * A read with no data returns -1 after EHCI's ~5 s timeout; slow commands
 * (WIFI, HTTP) can take longer, so keep waiting up to 6 timeouts. */
static int read_reply(void) {
    char rx[512];
    int col = 0, prompt = 0, waits = 0, i, k;

    for (;;) {
        k = link_read(rx, sizeof(rx));
        if (k < 0) {
            if (++waits < 6) {
                puts("[waiting]\n");
                continue;
            }
            printf("\n[no reply]\n");
            return -1;
        }
        for (i = 0; i < k; i++) {
            char c = rx[i];

            if (c == '\n') {
                if (prompt) {
                    return 0;
                }
                putc('\n');
                col = 0;
                continue;
            }
            prompt = (col == 0 && c == '>');
            if (!prompt) {
                putc(c);
            }
            col++;
        }
    }
}

int main(int argc, char *argv[]) {
    u32 baud = (argc > 1) ? parse_dec(argv[1]) : 921600;
    char line[200];
    int n, ver;

    if (cdc_open(ESP_VID, ESP32C3_PID) == 0) {
        use_cdc = 1;
        printf("picoterm: ESP32-C3 USB serial (EP %02x/%02x). Commands: HELP. "
                "Empty line quits.\n", cdc_ep_in, cdc_ep_out);
    } else {
        ver = ch340_open(baud);
        if (ver < 0) {
            printf("No ESP32-C3 (303a:1001) or CH340 (1a86:7523) found. "
                    "Run 'usb start' first.\n");
            return 1;
        }
        printf("picoterm: CH340 v%02x, %d baud. Commands: HELP. Empty line quits.\n",
                ver, baud);
    }

    for (;;) {
        puts(use_cdc ? "c3> " : "pico> ");
        n = read_line(line, sizeof(line));
        if (n == 0) {
            break;
        }
        line[n++] = '\n';
        if (link_write(line, n) != n) {
            printf("[USB write failed]\n");
            continue;
        }
        read_reply();
    }
    return 0;
}
