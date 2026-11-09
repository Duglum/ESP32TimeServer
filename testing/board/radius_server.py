"""A RADIUS test server for the board tests (Python standard library only): answers Access-Requests like Cisco ISE or
FreeRADIUS answer a Cisco switch for an administrator login, and can misbehave on purpose.

As a program (the owner's own tries, or a second server for the failover):

    python -I testing/board/radius_server.py --secret testing123 [--port 1812] [--mode normal] [--bind 0.0.0.0]

As a module (radius_logins.py): RadiusTestServer(port, secret) runs in a thread; .mode, .users and .requests (the
decoded requests) can be changed and read while it runs.

Users (password "radius-test-pw" each, --password changes it):
    admin15    Access-Accept, Service-Type = Administrative-User, Cisco-AVPair = "shell:priv-lvl=15"
    adminst    Access-Accept, Service-Type = Administrative-User (no AV pair: level 15 by the Service-Type)
    helpdesk   Access-Accept, Service-Type = NAS-Prompt-User, Cisco-AVPair = "shell:priv-lvl=1"
    classed    Access-Accept, Cisco-AVPair = "shell:priv-lvl=15", Class = "CS-ADMIN"
    anything else, or a wrong password: Access-Reject with Reply-Message
Modes:
    normal         as above
    silent         no answer (timeouts, failover)
    bad_auth       a wrong Response Authenticator (must be dropped)
    bad_ma         a wrong Message-Authenticator (must be dropped)
    no_ma          no Message-Authenticator (dropped unless "require_message_authenticator" is off)
    wrong_id       another identifier (must be dropped)
    challenge      Access-Challenge (not supported: a refusal)
    accept_all     Access-Accept with level 15 for any user and password
Every request must carry a valid Message-Authenticator; one without is logged and not answered.
"""
import argparse
import hashlib
import hmac
import socket
import struct
import threading
import time

ACCESS_REQUEST, ACCESS_ACCEPT, ACCESS_REJECT, ACCESS_CHALLENGE = 1, 2, 3, 11
NAMES = {1: "User-Name", 2: "User-Password", 4: "NAS-IP-Address", 5: "NAS-Port", 6: "Service-Type", 31: "Calling-Station-Id",
         32: "NAS-Identifier", 61: "NAS-Port-Type", 80: "Message-Authenticator", 87: "NAS-Port-Id", 95: "NAS-IPv6-Address"}
DEFAULT_PASSWORD = "radius-test-pw"


def attr(t, v):
    return bytes([t, 2 + len(v)]) + v


def cisco(avpair):
    return attr(26, struct.pack(">I", 9) + attr(1, avpair.encode()))


def service(value):
    return attr(6, struct.pack(">I", value))


def unhide(hidden, secret, authenticator):
    out, prev = b"", authenticator
    for i in range(0, len(hidden), 16):
        b = hashlib.md5(secret + prev).digest()
        block = hidden[i:i + 16]
        out += bytes(x ^ y for x, y in zip(block, b))
        prev = block
    return out.rstrip(b"\0")


def parse_attributes(data):
    attrs, at = [], 0
    while at + 2 <= len(data):
        t, n = data[at], data[at + 1]
        if n < 2 or at + n > len(data):
            raise ValueError("malformed attribute")
        attrs.append((t, data[at + 2:at + n], at))
        at += n
    if at != len(data):
        raise ValueError("trailing bytes")
    return attrs


def build_answer(code, identifier, request_authenticator, secret, attributes, mode):
    with_ma = mode != "no_ma"
    body = (attr(80, b"\0" * 16) if with_ma else b"") + attributes
    length = 20 + len(body)
    if with_ma:
        tmp = bytes([code, identifier]) + struct.pack(">H", length) + request_authenticator + body
        mac = hmac.new(secret, tmp, hashlib.md5).digest()
        if mode == "bad_ma":
            mac = bytes([mac[0] ^ 0x55]) + mac[1:]
        body = body[:2] + mac + body[18:]
    auth = hashlib.md5(bytes([code, identifier]) + struct.pack(">H", length) + request_authenticator + body + secret).digest()
    if mode == "bad_auth":
        auth = bytes([auth[0] ^ 0x01]) + auth[1:]
    if mode == "wrong_id":
        identifier = (identifier + 1) & 0xFF
    return bytes([code, identifier]) + struct.pack(">H", length) + auth + body


class RadiusTestServer:
    def __init__(self, port=1812, secret="testing123", bind="0.0.0.0", password=DEFAULT_PASSWORD, verbose=False):
        self.secret = secret.encode()
        self.mode = "normal"
        self.password = password
        self.verbose = verbose
        self.requests = []  # decoded requests: dict of attribute names, plus "_ma_ok", "_password", "_from"
        self.lock = threading.Lock()
        family = socket.AF_INET6 if ":" in bind else socket.AF_INET
        self.sock = socket.socket(family, socket.SOCK_DGRAM)
        self.sock.bind((bind, port))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(2)
        self.sock.close()

    def clear(self):
        with self.lock:
            self.requests.clear()

    def last(self):
        with self.lock:
            return dict(self.requests[-1]) if self.requests else None

    def count(self):
        with self.lock:
            return len(self.requests)

    def answer_for(self, user, password):
        if self.mode == "accept_all":
            return ACCESS_ACCEPT, service(6) + cisco("shell:priv-lvl=15")
        if self.mode == "challenge":
            return ACCESS_CHALLENGE, attr(18, b"Enter your token code")
        if password == self.password:
            if user == "admin15":
                return ACCESS_ACCEPT, service(6) + cisco("shell:priv-lvl=15")
            if user == "adminst":
                return ACCESS_ACCEPT, service(6)
            if user == "helpdesk":
                return ACCESS_ACCEPT, service(7) + cisco("shell:priv-lvl=1")
            if user == "classed":
                return ACCESS_ACCEPT, cisco("shell:priv-lvl=15") + attr(25, b"CS-ADMIN")
        return ACCESS_REJECT, attr(18, b"Authentication failed")

    def handle(self, packet, source):
        if len(packet) < 20 or packet[0] != ACCESS_REQUEST:
            return None
        identifier = packet[1]
        length = struct.unpack(">H", packet[2:4])[0]
        authenticator = packet[4:20]
        attrs = parse_attributes(packet[20:length])
        decoded = {"_from": source[0], "_identifier": identifier, "_order": [NAMES.get(t, str(t)) for t, _, _ in attrs]}
        ma_ok = False
        for t, v, at in attrs:
            name = NAMES.get(t, str(t))
            if t == 80:
                zeroed = packet[:20 + at + 2] + b"\0" * 16 + packet[20 + at + 18:length]
                ma_ok = hmac.compare_digest(hmac.new(self.secret, zeroed, hashlib.md5).digest(), v)
            elif t == 2:
                decoded["_password"] = unhide(v, self.secret, authenticator).decode("utf-8", "replace")
            elif t in (5, 6, 61):
                decoded[name] = struct.unpack(">I", v)[0]
            elif t == 4:
                decoded[name] = socket.inet_ntop(socket.AF_INET, v)
            elif t == 95:
                decoded[name] = socket.inet_ntop(socket.AF_INET6, v)
            else:
                decoded[name] = v.decode("utf-8", "replace")
        decoded["_ma_ok"] = ma_ok
        with self.lock:
            self.requests.append(decoded)
        if self.verbose:
            shown = {k: v for k, v in decoded.items() if k != "_password"}
            print(f"{time.strftime('%H:%M:%S')} request from {source[0]}: {shown}", flush=True)
        if not ma_ok:
            print("  request without a valid Message-Authenticator: not answered", flush=True)
            return None
        if self.mode == "silent":
            return None
        code, attributes = self.answer_for(decoded.get("User-Name", ""), decoded.get("_password", ""))
        if self.verbose:
            print(f"  -> {({2: 'Access-Accept', 3: 'Access-Reject', 11: 'Access-Challenge'})[code]} ({self.mode})", flush=True)
        return build_answer(code, identifier, authenticator, self.secret, attributes, self.mode)

    def serve(self):
        while self.running:
            try:
                packet, source = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                reply = self.handle(packet, source)
            except ValueError as e:
                print(f"  malformed request from {source[0]}: {e}", flush=True)
                continue
            if reply:
                self.sock.sendto(reply, source)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=1812)
    p.add_argument("--secret", default="testing123")
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--mode", default="normal", choices=["normal", "silent", "bad_auth", "bad_ma", "no_ma", "wrong_id", "challenge", "accept_all"])
    p.add_argument("--password", default=DEFAULT_PASSWORD, help="the password of the test users")
    a = p.parse_args()
    server = RadiusTestServer(a.port, a.secret, a.bind, a.password, verbose=True)
    server.mode = a.mode
    print(f"RADIUS test server on {a.bind} UDP {server.port}, mode {a.mode} (Ctrl+C ends)", flush=True)
    server.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        server.stop()


if __name__ == "__main__":
    main()
