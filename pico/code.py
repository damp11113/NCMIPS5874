# Pico 2 W side of the box link (CircuitPython).
# Copy this file to the CIRCUITPY drive as code.py. No extra libraries
# needed (HTTPS uses adafruit_requests if you add it to lib/).
#
# Wiring (CH340 set to 3.3 V):
#   CH340 TXD -> Pico GP1 (UART0 RX)
#   CH340 RXD -> Pico GP0 (UART0 TX)
#   GND       -> GND
#
# Protocol: the box sends one command line ending in "\n". The Pico always
# answers with zero or more lines and then a line ">" so the box knows the
# reply is complete (the box's USB reads block when nothing arrives).
#
# Commands:
#   PING                  -> PONG
#   LED 0|1               onboard LED
#   TEMP                  RP2350 internal temperature (deg C)
#   ADC n                 raw 16-bit reading of A0..A2 (GP26..28)
#   PIN n 0|1             drive GPn as output (stays set)
#   GET n                 read GPn as input with pull-up
#   WIFI ssid password    connect, prints IP
#   IP                    current IP (or "not connected")
#   HTTP url              GET url, prints status line + first 1024 bytes of body
#   HELP                  list commands

import board
import busio
import digitalio
import analogio
import microcontroller
import time
import wifi
import socketpool

BAUD = 921600

uart = busio.UART(board.GP0, board.GP1, baudrate=BAUD, timeout=0.01,
                  receiver_buffer_size=4096)
led = digitalio.DigitalInOut(board.LED)
led.direction = digitalio.Direction.OUTPUT
pins = {}       # GP number -> DigitalInOut kept alive so outputs stay set
pool = None


def reply(lines):
    for line in lines:
        uart.write((str(line).replace("\r", "") + "\n").encode())
    uart.write(b">\n")


def gp(n):
    if n in pins:
        pins[n].deinit()
        del pins[n]
    p = digitalio.DigitalInOut(getattr(board, "GP%d" % n))
    pins[n] = p
    return p


def cmd_wifi(args):
    if len(args) < 2:
        return ["ERR usage: WIFI ssid password"]
    # connect() on the Pico W sometimes raises "Unknown failure 1" although
    # the link comes up anyway (seen on hardware): check, retry once.
    err = None
    for _ in range(2):
        try:
            wifi.radio.connect(args[0], " ".join(args[1:]))
        except Exception as e:
            err = e
        for _ in range(20):             # up to ~5 s for DHCP
            if wifi.radio.connected and wifi.radio.ipv4_address:
                return ["OK " + str(wifi.radio.ipv4_address)]
            time.sleep(0.25)
    return ["ERR %s" % err]


def http_plain(url):
    # url = http://host[:port]/path, no library needed
    global pool
    if pool is None:
        pool = socketpool.SocketPool(wifi.radio)
    rest = url[7:]
    host, _, path = rest.partition("/")
    host, _, port = host.partition(":")
    port = int(port) if port else 80
    addr = pool.getaddrinfo(host, port)[0][-1]
    s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
    s.settimeout(10)
    try:
        s.connect(addr)
        s.send(("GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n"
                % (path, host)).encode())
        data = b""
        buf = bytearray(512)
        while len(data) < 4096:
            n = s.recv_into(buf)
            if n <= 0:
                break
            data += bytes(buf[:n])
    finally:
        s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    status = head.split(b"\r\n")[0].decode("utf-8", "ignore")
    return [status] + body[:1024].decode("utf-8", "ignore").split("\n")


def http_tls(url):
    import ssl
    import adafruit_requests
    global pool
    if pool is None:
        pool = socketpool.SocketPool(wifi.radio)
    s = adafruit_requests.Session(pool, ssl.create_default_context())
    r = s.get(url)
    out = ["STATUS %d" % r.status_code] + r.text[:1024].split("\n")
    r.close()
    return out


def cmd_http(args):
    if not args:
        return ["ERR usage: HTTP url"]
    if not wifi.radio.connected:
        return ["ERR WiFi not connected (use WIFI ssid password)"]
    url = args[0]
    if url.startswith("http://"):
        return http_plain(url)
    if url.startswith("https://"):
        try:
            return http_tls(url)
        except ImportError:
            return ["ERR https needs lib/adafruit_requests.mpy"]
    return ["ERR url must start with http:// or https://"]


def handle(line):
    parts = line.split()
    if not parts:
        return []
    c, a = parts[0].upper(), parts[1:]
    if c == "PING":
        return ["PONG"]
    if c == "LED":
        led.value = a[0] == "1"
        return ["OK"]
    if c == "TEMP":
        return ["%.1f" % microcontroller.cpu.temperature]
    if c == "ADC":
        with analogio.AnalogIn(getattr(board, "A%d" % int(a[0]))) as adc:
            return [str(adc.value)]
    if c == "PIN":
        p = gp(int(a[0]))
        p.direction = digitalio.Direction.OUTPUT
        p.value = a[1] == "1"
        return ["OK"]
    if c == "GET":
        p = gp(int(a[0]))
        p.direction = digitalio.Direction.INPUT
        p.pull = digitalio.Pull.UP
        return ["1" if p.value else "0"]
    if c == "WIFI":
        return cmd_wifi(a)
    if c == "IP":
        return [str(wifi.radio.ipv4_address) if wifi.radio.connected else "not connected"]
    if c == "HTTP":
        return cmd_http(a)
    if c == "HELP":
        return ["PING  LED 0|1  TEMP  ADC n  PIN n 0|1  GET n",
                "WIFI ssid pass  IP  HTTP url"]
    return ["ERR unknown command " + c]


led.value = True
time.sleep(0.2)
led.value = False
print("box link ready on GP0/GP1 at", BAUD)

buf = b""
while True:
    data = uart.read(64)
    if not data:
        continue
    buf += data
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        line = raw.decode("utf-8", "ignore").strip()
        try:
            reply(handle(line))
        except Exception as e:
            reply(["ERR %s" % e])
