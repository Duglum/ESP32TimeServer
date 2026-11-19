"""Packet floods against ESP32 Time Server (taken from ESP32-WebTherm, testing/board/flood.py).

  udp:   UDP datagrams at a fixed rate to one port. Default port 9 (discard: nothing listens there). With --port 123
         the payload is 64 zero bytes by default: no NTP request, so the frames pass the hardware NTP path and lwIP
         hands them to the NTP socket task. With --ntp each datagram is a valid NTPv4 client request (48 bytes,
         mode 3), which the hardware NTP path takes (load shedding above 10000 per second).
  https: TCP connections to port 443 that are opened and closed without a TLS handshake, as fast as possible with
         a number of parallel connections. Tests the web server and the allow-list path.
  slow:  one slow client at a time for --seconds, a new one whenever the device closes the last (--kind: connect = a
         TCP connection that sends nothing, tls = a TLS record of a ClientHello 1 byte every 2 s, header = request
         headers 1 byte every 2 s, login = POST /api/login with Content-Length 4096 and 1 body byte every 4 s,
         purge = Content-Length 100000 (answer 413) and the body 1 byte every 2 s, read = GET /i18n.js read at about
         500 bytes per second); meanwhile GET /api/info about every second over new connections. The web server has
         one task: the device has to close each slow client after its time budget (5 s for the handshake, 15 s per
         request) and /api/info must never wait much longer than that (taken from ESP32-ConsoleServer).
  idle:  --parallel TLS connections that complete the handshake and send nothing, held for --seconds; then
         GET /api/info over one more connection, and for each held connection when the device closed it. Run a
         certificate change (/api/tls) meanwhile to see whether it closes connections without a request.

Only use it against your own device in your own network.
Usage:
  python -I testing/flood/flood.py udp   <address> [--rate 15000] [--seconds 60] [--port 9] [--size 64] [--ntp]
  python -I testing/flood/flood.py https <address> [--seconds 60] [--parallel 32]
  python -I testing/flood/flood.py slow  <address> --kind login [--seconds 60]
  python -I testing/flood/flood.py idle  <address> [--parallel 1] [--seconds 60]
"""
import argparse
import http.client
import socket
import ssl
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


def tls_context():
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context


def info(args, context, timeout=60):
    """GET /api/info over a new connection: (seconds, status or None)."""
    start = time.perf_counter()
    connection = http.client.HTTPSConnection(args.address, 443, timeout=timeout, context=context)
    try:
        connection.request("GET", "/api/info")
        response = connection.getresponse()
        response.read()
        return time.perf_counter() - start, response.status
    except (OSError, http.client.HTTPException):
        return time.perf_counter() - start, None
    finally:
        connection.close()


def closed(sock, wait):
    """True when the device closed the connection (waits up to wait seconds for it)."""
    sock.settimeout(wait)
    try:
        return sock.recv(1024) == b""
    except (socket.timeout, ssl.SSLWantReadError):
        return False
    except OSError:
        return True


def slow_test(args):
    context = tls_context()
    family, address = resolve(args.address, 443, socket.SOCK_STREAM)
    host = "[%s]" % args.address if ":" in args.address else args.address
    post = "POST /api/login HTTP/1.1\r\nHost: %s\r\nOrigin: https://%s\r\nContent-Type: application/json\r\n" % (host, host)
    first, step, interval = {
        "connect": (b"", None, 1),
        "tls": (bytes([0x16, 0x03, 0x01, 0x02, 0x00, 0x01]), b"\x00", 2),
        "header": (("GET /api/info HTTP/1.1\r\nHost: %s\r\nX-Slow: " % host).encode(), b"a", 2),
        "login": ((post + "Content-Length: 4096\r\n\r\n ").encode(), b" ", 4),
        "purge": ((post + "Content-Length: 100000\r\n\r\nx").encode(), b"x", 2),
        "read": (("GET /i18n.js HTTP/1.1\r\nHost: %s\r\n\r\n" % host).encode(), None, 2),
    }[args.kind]
    worst = [0.0, 0, 0]  # longest wait for /api/info, answers, failures
    stop = threading.Event()
    t0 = time.monotonic()

    def poll():
        while not stop.is_set():
            seconds, status = info(args, context)
            worst[0] = max(worst[0], seconds)
            worst[1 if status is not None else 2] += 1
            if seconds > 1.0 or status is None:
                print("[%6.1f] /api/info: %s after %.1f s" % (time.monotonic() - t0, status or "no answer", seconds), flush=True)
            stop.wait(1)

    poller = threading.Thread(target=poll, daemon=True)
    poller.start()
    end = time.monotonic() + args.seconds
    kept = []
    while time.monotonic() < end:
        sock = socket.socket(family, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # the slow reader: a small TCP window
        sock.settimeout(15)
        start = time.monotonic()
        how = "closed by this script at the end"
        try:
            sock.connect(address)
            request = first
            if args.kind == "read":
                # The handshake through memory BIOs, then the raw TCP stream is read slowly and never decrypted, so
                # that the reading speed is what the device sees
                incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
                tls = context.wrap_bio(incoming, outgoing)
                while True:
                    try:
                        tls.do_handshake()
                        break
                    except ssl.SSLWantReadError:
                        sock.sendall(outgoing.read())
                        data = sock.recv(4096)
                        if not data:
                            raise OSError("closed in the handshake")
                        incoming.write(data)
                tls.write(first)
                request = outgoing.read()
            elif args.kind not in ("connect", "tls"):
                sock = context.wrap_socket(sock)
            sock.sendall(request)
            while time.monotonic() < end:
                if step is not None:
                    time.sleep(interval)
                    sock.sendall(step)
                elif args.kind == "read":
                    time.sleep(interval)
                    if not sock.recv(1024):
                        how = "closed by the device"
                        break
                elif closed(sock, interval):
                    how = "closed by the device"
                    break
        except OSError as error:
            how = "closed by the device (%s)" % type(error).__name__
        sock.close()
        kept.append(time.monotonic() - start)
        print("[%6.1f] slow client %d: %s after %.1f s" % (time.monotonic() - t0, len(kept), how, kept[-1]), flush=True)
    stop.set()
    poller.join(65)
    print("%s: %d slow clients, the longest kept %.1f s (the last one was closed by this script at the end); /api/info: "
          "%d answers, the longest wait %.1f s, %d failures" % (args.kind, len(kept), max(kept[:-1] or kept), worst[1], worst[0], worst[2]))


def idle_test(args):
    context = tls_context()
    family, address = resolve(args.address, 443, socket.SOCK_STREAM)
    held = []
    for _ in range(args.parallel):
        try:
            held.append([context.wrap_socket(socket.create_connection(address, timeout=15)), time.monotonic(), None])
        except OSError as error:
            print("connection %d: %s" % (len(held) + 1, type(error).__name__))
            break
    print("%d TLS connections open, nothing sent" % len(held), flush=True)
    seconds, status = info(args, context, 15)
    print("/api/info with them held: %s after %.1f s" % (status or "no answer", seconds), flush=True)
    end = time.monotonic() + args.seconds
    while time.monotonic() < end and any(entry[2] is None for entry in held):
        for number, entry in enumerate(held, 1):
            if entry[2] is None and closed(entry[0], 0.1):
                entry[2] = time.monotonic() - entry[1]
                print("connection %d: closed by the device after %.1f s" % (number, entry[2]), flush=True)
        time.sleep(0.4)
    for number, entry in enumerate(held, 1):
        if entry[2] is None:
            print("connection %d: still open after %.1f s, closed by this script" % (number, time.monotonic() - entry[1]))
        entry[0].close()
    seconds, status = info(args, context, 15)
    print("/api/info after closing them: %s after %.1f s" % (status or "no answer", seconds))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["udp", "https", "slow", "idle"])
    parser.add_argument("address", help="IPv4 or IPv6 address or name of the device")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--rate", type=int, default=15000, help="UDP datagrams per second")
    parser.add_argument("--port", type=int, default=9, help="UDP port")
    parser.add_argument("--size", type=int, default=64, help="UDP payload bytes (without --ntp)")
    parser.add_argument("--ntp", action="store_true", help="valid NTPv4 client requests instead of zero bytes")
    parser.add_argument("--parallel", type=int, default=None, help="parallel TCP connections (https: 32, idle: 1)")
    parser.add_argument("--kind", choices=["connect", "tls", "header", "login", "purge", "read"], default="login", help="slow client")
    args = parser.parse_args()
    if args.parallel is None:
        args.parallel = 1 if args.mode == "idle" else 32
    if args.mode == "udp":
        udp_flood(args)
    elif args.mode == "https":
        https_flood(args)
    elif args.mode == "slow":
        slow_test(args)
    else:
        idle_test(args)


if __name__ == "__main__":
    main()
