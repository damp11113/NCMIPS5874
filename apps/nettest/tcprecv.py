"""Receive the box's NETTEST send test (YELLOW): listens on port 5002.
Usage: python apps/nettest/tcprecv.py"""
import socket
import time

ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("", 5002))
ls.listen(1)
print("waiting on port 5002 (press YELLOW in NETTEST)")
c, addr = ls.accept()
t0 = time.time()
total = 0
while True:
    d = c.recv(65536)
    if not d:
        break
    total += len(d)
dt = time.time() - t0
print("%d KB from %s in %.2f s = %.0f KB/s" % (total // 1024, addr[0], dt, total / 1024 / dt))
