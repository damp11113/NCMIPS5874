"""Send data to the box's NETTEST receive test (GREEN, port 5001).
Usage: python apps/nettest/tcpsend.py <box address> [megabytes, default 8]"""
import socket
import sys
import time

host = sys.argv[1]
mb = int(sys.argv[2]) if len(sys.argv) > 2 else 8
block = bytes(range(256)) * 64
s = socket.create_connection((host, 5001), timeout=10)
t0 = time.time()
for _ in range(mb * 1024 * 1024 // len(block)):
    s.sendall(block)
s.shutdown(socket.SHUT_WR)
s.recv(1)
s.close()
dt = time.time() - t0
print("%d MB in %.2f s = %.0f KB/s" % (mb, dt, mb * 1024 / dt))
