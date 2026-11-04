"""Packet floods against ESP32 Time Server (taken from ESP32-WebTherm, testing/board/flood.py).

  udp:   UDP datagrams at a fixed rate to one port. Default port 9 (discard: nothing listens there). With --port 123
         the payload is 64 zero bytes by default: no NTP request, so the frames pass the hardware NTP path and lwIP
         hands them to the NTP socket task. With --ntp each datagram is a valid NTPv4 client request (48 bytes,
         mode 3), which the hardware NTP path takes (load shedding above 10000 per second).
  https: TCP connections to port 443 that are opened and closed without a TLS handshake, as fast as possible with
         a number of parallel connections. Tests the web server and the allow-list path.

Only use it against your own device in your own network.
Usage:
  python -I testing/flood/flood.py udp   <address> [--rate 15000] [--seconds 60] [--port 9] [--size 64] [--ntp]
  python -I testing/flood/flood.py https <address> [--seconds 60] [--parallel 32]
"""
import argparse
import socket
import struct
import threading
import time


def resolve(host, port, kind):
    family, _, _, _, address = socket.getaddrinfo(host, port, type=kind)[0]
    return family, address


def ntp_request():
    # LI 0, version 4, mode 3 (client), transmit time stamp set (a client's origin), everything else zero
    now = time.time() + 2208988800
    seconds = int(now)
    fraction = int((now - seconds) * 2 ** 32) & 0xFFFFFFFF
    return bytes([0x23]) + bytes(39) + struct.pack(">II", seconds, fraction)


def udp_flood(args):
    family, address = resolve(args.address, args.port, socket.SOCK_DGRAM)
    sock = socket.socket(family, socket.SOCK_DGRAM)
    payload = ntp_request() if args.ntp else bytes(args.size)
    sent = 0
    start = time.perf_counter()
    end = start + args.seconds
    interval = 1.0 / args.rate
    next_send = start
    while True:
        now = time.perf_counter()
        if now >= end:
            break
        # Send in bursts to reach high rates despite the timer resolution
        while next_send <= now:
            try:
                sock.sendto(payload, address)
                sent += 1
            except OSError:
                pass
            next_send += interval
    elapsed = time.perf_counter() - start
    print("UDP: %d datagrams (%s) in %.1f s (%.0f per second) to %s port %d" %
          (sent, "NTP requests" if args.ntp else "%d zero bytes" % args.size, elapsed, sent / elapsed, args.address, args.port))


def https_flood(args):
    family, address = resolve(args.address, 443, socket.SOCK_STREAM)
    end = time.perf_counter() + args.seconds
    counts = {"connected": 0, "failed": 0}
    lock = threading.Lock()

    def worker():
        while time.perf_counter() < end:
            sock = socket.socket(family, socket.SOCK_STREAM)
            sock.settimeout(3)
            try:
                sock.connect(address)
                key = "connected"
            except OSError:
                key = "failed"
            finally:
                sock.close()
            with lock:
                counts[key] += 1

    threads = [threading.Thread(target=worker) for _ in range(args.parallel)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    print("HTTPS: %d connections opened and closed, %d failed, in %d s" % (counts["connected"], counts["failed"], args.seconds))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["udp", "https"])
    parser.add_argument("address", help="IPv4 or IPv6 address or name of the device")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--rate", type=int, default=15000, help="UDP datagrams per second")
    parser.add_argument("--port", type=int, default=9, help="UDP port")
    parser.add_argument("--size", type=int, default=64, help="UDP payload bytes (without --ntp)")
    parser.add_argument("--ntp", action="store_true", help="valid NTPv4 client requests instead of zero bytes")
    parser.add_argument("--parallel", type=int, default=32, help="parallel TCP connections")
    args = parser.parse_args()
    if args.mode == "udp":
        udp_flood(args)
    else:
        https_flood(args)


if __name__ == "__main__":
    main()
