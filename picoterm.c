/*
 * picoterm: talk to the Pico 2 W (pico/code.py) through the CH340.
 *
 *   usb start
 *   go ${a} [baud]          default 115200
 *
 * Type a command (e.g. PING, TEMP, LED 1, WIFI ssid pass, HTTP url) and
 * press Enter; the Pico's reply is printed up to its ">" line.
 * Empty line or Ctrl-C quits.
 */
#include "uboot.h"
#include "ch340.h"

static u32 parse_dec (const char *s) {
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

/* Read the console until Enter; returns length (0 = quit) */
static int read_line (char *line, int max) {
    int n = 0;

    for (;;) {
        int c = getc ();

        if (c == 3) {                   /* Ctrl-C */
            return 0;
        }
        if (c == '\r' || c == '\n') {
            puts ("\n");
            return n;
        }
        if ((c == 8 || c == 127) && n > 0) {
            n--;
            puts ("\b \b");
        } else if (c >= ' ' && n < max - 2) {
            line[n++] = c;
            putc (c);
        }
    }
}

/* Print the reply until a line that is just ">"; returns 0 or -1.
 * A read with no data returns -1 after EHCI's ~5 s timeout; slow commands
 * (WIFI, HTTP) can take longer, so keep waiting up to 6 timeouts. */
static int read_reply (void) {
    char rx[40];
    int col = 0, prompt = 0, waits = 0, i, k;

    for (;;) {
        k = ch340_read (rx, 32);
        if (k < 0) {
            if (++waits < 6) {
                puts ("[waiting]\n");
                continue;
            }
            printf ("\n[no reply from Pico]\n");
            return -1;
        }
        for (i = 0; i < k; i++) {
            char c = rx[i];

            if (c == '\n') {
                if (prompt) {
                    return 0;
                }
                putc ('\n');
                col = 0;
                continue;
            }
            prompt = (col == 0 && c == '>');
            if (!prompt) {
                putc (c);
            }
            col++;
        }
    }
}

int main (int argc, char *argv[]) {
    u32 baud = (argc > 1) ? parse_dec (argv[1]) : 115200;
    char line[200];
    int n, ver;

    ver = ch340_open (baud);
    if (ver < 0) {
        printf ("No CH340 (1a86:7523) found. Run 'usb start' first.\n");
        return 1;
    }
    printf ("picoterm: CH340 v%02x, %d baud. Commands: HELP. Empty line quits.\n",
            ver, baud);

    for (;;) {
        puts ("pico> ");
        n = read_line (line, sizeof (line));
        if (n == 0) {
            break;
        }
        line[n++] = '\n';
        if (ch340_write (line, n) != n) {
            printf ("[USB write failed]\n");
            continue;
        }
        read_reply ();
    }
    return 0;
}
