# Pico 2 W side of the box link (MicroPython). The CircuitPython version
# (code.py) is the one tested on hardware; this is the same protocol.
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
#   ADC n                 raw 16-bit reading of ADC0..3 (GP26..29)
#   PIN n 0|1             drive GPn as output
#   GET n                 read GPn as input (pull-up)
#   WIFI ssid password    connect, prints IP
#   IP                    current IP (or "not connected")
#   HTTP url              GET url, prints status + first 1024 bytes of body
#   HELP                  list commands

import machine
import network
import time

BAUD = 115200

uart = machine.UART(0, baudrate=BAUD, tx=machine.Pin(0), rx=machine.Pin(1),
                    timeout=10, rxbuf=1024)
led = machine.Pin("LED", machine.Pin.OUT)
wlan = network.WLAN(network.STA_IF)


def reply(*lines):
    for line in lines:
        uart.write(str(line).replace("\r", "") + "\n")
    uart.write(">\n")


def cmd_temp():
    raw = machine.ADC(4).read_u16() * 3.3 / 65535
    return "%.1f" % (27 - (raw - 0.706) / 0.001721)


def cmd_wifi(args):
    if len(args) < 2:
        return ["ERR usage: WIFI ssid password"]
    wlan.active(True)
    wlan.connect(args[0], " ".join(args[1:]))
    for _ in range(40):                 # up to ~20 s
        if wlan.isconnected():
            return ["OK " + wlan.ifconfig()[0]]
        time.sleep(0.5)
    return ["ERR not connected, status %d" % wlan.status()]


def cmd_http(args):
    import urequests
    if not args:
        return ["ERR usage: HTTP url"]
    r = urequests.get(args[0])
    out = ["STATUS %d" % r.status_code]
    body = r.text[:1024]
    r.close()
    return out + body.split("\n")


def handle(line):
    parts = line.split()
    if not parts:
        return []
    c, a = parts[0].upper(), parts[1:]
    if c == "PING":
        return ["PONG"]
    if c == "LED":
        led.value(int(a[0]))
        return ["OK"]
    if c == "TEMP":
        return [cmd_temp()]
    if c == "ADC":
        return [str(machine.ADC(int(a[0])).read_u16())]
    if c == "PIN":
        machine.Pin(int(a[0]), machine.Pin.OUT).value(int(a[1]))
        return ["OK"]
    if c == "GET":
        return [str(machine.Pin(int(a[0]), machine.Pin.IN, machine.Pin.PULL_UP).value())]
    if c == "WIFI":
        return cmd_wifi(a)
    if c == "IP":
        return [wlan.ifconfig()[0] if wlan.isconnected() else "not connected"]
    if c == "HTTP":
        return cmd_http(a)
    if c == "HELP":
        return ["PING  LED 0|1  TEMP  ADC n  PIN n 0|1  GET n",
                "WIFI ssid pass  IP  HTTP url"]
    return ["ERR unknown command " + c]


buf = b""
led.value(1)
time.sleep(0.2)
led.value(0)
while True:
    data = uart.read()
    if not data:
        continue
    buf += data
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        line = raw.decode("utf-8", "ignore").strip()
        try:
            reply(*handle(line))
        except Exception as e:
            reply("ERR " + str(e))
