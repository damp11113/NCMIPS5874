# Network: WiFi + lwIP for NCAPPS

The IPTV box's internal WiFi (RTL8188FTV module on the SoC's second USB
controller) as a service of the launcher's system (Multitasking on), with
lwIP 2.2.1 as the TCP/IP stack. Apps use it through `sdk/net.h`.

```
 app (UI task)          sdk_tcp_connect / sdk_net_recv / ... (sdk/net.h)
    |  @sys v3 -> sys->net (struct sdk_net_ops)
 net/netsvc.c           sockets for apps (lwip_* API), WIFI.TXT, scan
    |
 lwIP tcpip thread (3)  DHCP ("ncwifi"), DNS, ARP, IPv4, TCP, UDP, raw
    |  netif "wl": linkoutput -> wlan_send / input <- wifi_rx
 net task (2)           join, RX pump, link loss -> join again
    |
 wlan.h                 WPA2-PSK station: scan, auth, assoc, 4-way,
                        group / pairwise rekey, CCMP in the chip
 rtl8188.h              RTL8188FTV driver (from Linux rtl8xxxu)
 ehci.h                 own EHCI host driver, controller 0xbf070000
```

## Files

| File | What |
|---|---|
| `../ehci.h` | polled EHCI driver for the WiFi controller (U-Boot keeps the stick on 0xbf060000) |
| `../rtl8188.h`, `../wlan.h`, `../wpa.h` | chip driver, WPA2 station, crypto (GPL-2.0-only parts from Linux) |
| `netsvc.c`, `netsvc.h` | the service: net task, lwIP interface, app sockets |
| `lwipopts.h`, `port/arch/cc.h` | lwIP configuration and architecture header |
| `lwip/` | lwIP 2.2.1 (BSD-3-Clause, `lwip/COPYING`): core, IPv4, API, Ethernet, headers, FreeRTOS `sys_arch.c` from contrib |
| `../launcher/wifiui.c` | SETTINGS > WiFi page, on-screen keyboard |
| `../apps/nettest/` | test app: ping, DNS, HTTP, TCP speed tests (`tcpsend.py`, `tcprecv.py` on the PC) |

## On the stick

| File | |
|---|---|
| `/NCAPPS/RTLWIFI.BIN` | chip firmware, `rtl8188fufw.bin` from linux-firmware (`stage.sh` copies it from the project root) |
| `/NCAPPS/WIFI.TXT` | 512 bytes, written in place by the launcher: `on=`, `ssid=`, `psk=` (passphrase in plain text: keep the stick private) |

Read-only fallbacks: `/RTL8188FUFW.BIN` and `/WIFI.TXT` in the stick's root
(the files of the stand-alone `rtl*` tools).

## Behaviour

* Start: the net task loads the firmware, resets the WiFi controller, powers
  the chip up (~0.5 s at the UI's priority), then joins the saved network.
* Join fails: retry after 10 s, doubling up to 2 min. Deauthentication or no
  frame from the access point for 10 s: link down, join again after 2 s.
* SETTINGS > WiFi > GREEN scans (the link drops for ~2 s), OK on a network
  asks for the passphrase (on-screen keyboard), saves WIFI.TXT and joins.
* EXIT to the stock firmware: the chip is switched off first. After a
  watchdog restart (big memory, standby) the chip is still on; both our
  init (`rtl: chip still running ...`) and the stock firmware cope with it.
* Only WPA2-PSK with CCMP. Open, WEP, WPA1, WPA3 and 802.1X networks are
  listed but cannot be joined.

## Test without staging (serial)

`setenv nc_system 1` in U-Boot forces Multitasking on for that start;
the launcher can be uploaded with `SSerHial/boxctl.py send
launcher/LAUNCHER.BIN --addr 0x80800000` after the AV init lines of
`iptv/scripts/ncboot.txt`, then `usb start; go 0x80800000`.

## Measured (IPTV box, 2026-10-09)

* Join + DHCP: address a few seconds after the launcher starts.
* Ping from the PC: 0-6 ms (first ~200 ms with ARP), 1472-byte and
  fragmented 8000-byte packets fine.
* NETTEST: ping router 1-8 ms, DNS 20 ms, TCP connect 20 ms, HTTP 200;
  TCP receive ~1.9-2.0 MB/s, send ~1.4-1.5 MB/s.
