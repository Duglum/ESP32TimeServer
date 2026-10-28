"""Upload a firmware image to the ESP32 Time Server (Over The Ethernet update over HTTPS).

The image (ESP32TimeServer.bin of the build folder, not the merged flash image) is sent to
POST https://<device>/api/ote of the web admin server. The web admin password of the device is sent with
HTTP Basic authentication inside the TLS connection (the device ignores the user name). The device refuses
scripted uploads while its web admin password is the factory default.

The device is addressed by an IP address or by a fully qualified name that is looked up in the DNS
(A and AAAA records, unicast). Names under .local and single-label names are refused: the operating system
would resolve them with multicast (mDNS, LLMNR) or NetBIOS. The addresses are tried in the order the
resolver returns them; -4 / -6 limit the lookup to one address family.

The certificate of the device is self-signed, so it is checked by its SHA-256 fingerprint (shown on the
web admin page under Status and on the serial console at every start). Without --fingerprint the upload
stops and prints the fingerprint the device presented; --insecure skips the check (only on a trusted LAN).

Example:
  python tools\\ote_upload.py -i timeserver1.example.net -f build\\ESP32TimeServer.bin --fingerprint AB:CD:...
The password is read from the environment variable WEB_ADMIN_PASSWORD, otherwise it is asked for, so that
several devices with the same password can be updated in a loop without a prompt.
"""
import argparse
import base64
import getpass
import hashlib
import hmac
import http.client
import ipaddress
import json
import os
import socket
import ssl
import sys
import time
from pathlib import Path

DEFAULT_PORT = 443
CHUNK_SIZE = 16384
CONNECT_TIMEOUT = 10.0  # per address
SOCKET_TIMEOUT = 30.0  # the device waits at most 15 s for data, and needs a moment per flash sector
TOTAL_TIMEOUT = 330.0  # the device ends an upload after 300 s
EARLY_ANSWER_WAIT = 2.0  # the device checks the password (about 0.3 s) before it reads the image
BASIC_USER = "admin"  # not checked by the device


def normalise_fingerprint(text):
    value = text.replace(":", "").replace(" ", "").upper()
    if len(value) != 64 or any(c not in "0123456789ABCDEF" for c in value):
        raise argparse.ArgumentTypeError("SHA-256 fingerprint: 64 hex digits (with or without ':')")
    return value


def format_fingerprint(value):
    return ":".join(value[i:i + 2] for i in range(0, len(value), 2))


def read_image(path):
    data = Path(path).read_bytes()
    # Image magic 0xE9; application description (magic 0xABCD5432) after the image and first segment header
    if len(data) < 112 or data[0] != 0xE9 or int.from_bytes(data[32:36], "little") != 0xABCD5432:
        raise SystemExit("error: not an application image (use ESP32TimeServer.bin of the build folder, "
                         "not the merged flash image)")
    version = data[48:80].split(b"\0", 1)[0].decode("ascii", "replace")
    project = data[80:112].split(b"\0", 1)[0].decode("ascii", "replace")
    return data, project, version


def is_address(host):
    try:
        ipaddress.ip_address(host)
        return True
    except ValueError:
        return False


def resolve(host, port, family):
    """Addresses of host: the literal itself, or the A / AAAA records of a fully qualified name."""
    if not is_address(host):
        name = host.rstrip(".").lower()
        if name == "local" or name.endswith(".local"):
            raise SystemExit(f"error: {host} is an mDNS name; give the DNS name (FQDN) or an IP address")
        if "." not in name:
            raise SystemExit(f"error: {host} is not fully qualified; give the DNS name (FQDN, e.g. "
                             f"{name}.example.net) or an IP address")
    try:
        results = socket.getaddrinfo(host, port, family, socket.SOCK_STREAM, socket.IPPROTO_TCP)
    except socket.gaierror as error:
        record = {socket.AF_INET: "A record", socket.AF_INET6: "AAAA record"}.get(family, "A or AAAA record")
        raise SystemExit(f"error: no {record} for {host}: {error}")
    addresses = []
    for result_family, _, _, _, address in results:
        if (result_family, address) not in addresses:
            addresses.append((result_family, address))
    return addresses


def connect(host, port, family, context):
    """TLS connection to the first address of host that answers (unicast TCP)."""
    addresses = resolve(host, port, family)
    if not is_address(host):
        print(f"{host}: " + ", ".join(address[0] for _, address in addresses))
    connection = http.client.HTTPSConnection(host, port, timeout=SOCKET_TIMEOUT, context=context)
    errors = []
    for address_family, address in addresses:
        sock = socket.socket(address_family, socket.SOCK_STREAM, socket.IPPROTO_TCP)
        try:
            sock.settimeout(CONNECT_TIMEOUT)
            sock.connect(address)
            sock.settimeout(SOCKET_TIMEOUT)
            connection.sock = context.wrap_socket(sock, server_hostname=None if is_address(host) else host)
            if len(addresses) > 1:
                print(f"Connected to {address[0]}")
            return connection
        except (OSError, ssl.SSLError) as error:
            sock.close()
            errors.append(f"{address[0]}: {error}")
    raise SystemExit(f"error: no connection to {host} port {port}\n  " + "\n  ".join(errors))


def early_answer(connection):
    """The answer of the device before the image is sent (a refusal), or None when it waits for the image.

    The device refuses (factory password, wrong password, lock-out, updates off) as soon as it has the headers and
    closes the connection without reading the image. If image data is still unread then, its TCP stack resets the
    connection and Windows drops the answer that has already arrived; so the image is only sent when no answer came.
    """
    connection.sock.settimeout(EARLY_ANSWER_WAIT)
    try:
        return connection.getresponse()
    except TimeoutError:  # no answer: the device reads the image
        return None
    finally:
        if connection.sock is not None:
            connection.sock.settimeout(SOCKET_TIMEOUT)


def main():
    parser = argparse.ArgumentParser(description="Over The Ethernet update of the ESP32 Time Server (HTTPS).")
    parser.add_argument("-i", "--host", required=True,
                        help="fully qualified DNS name (A / AAAA records) or IP address of the device "
                             "(IPv6 without brackets); no .local names")
    family = parser.add_mutually_exclusive_group()
    family.add_argument("-4", dest="family", action="store_const", const=socket.AF_INET, default=socket.AF_UNSPEC,
                        help="IPv4 only (A records)")
    family.add_argument("-6", dest="family", action="store_const", const=socket.AF_INET6,
                        help="IPv6 only (AAAA records)")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT, help="HTTPS port (default 443)")
    parser.add_argument("-f", "--file", required=True, help="application image (.bin)")
    parser.add_argument("--fingerprint", type=normalise_fingerprint,
                        help="SHA-256 fingerprint of the device certificate (web admin page, Status)")
    parser.add_argument("--insecure", action="store_true", help="do not check the certificate fingerprint")
    parser.add_argument("--password-env", default="WEB_ADMIN_PASSWORD", metavar="NAME",
                        help="environment variable with the web admin password (default WEB_ADMIN_PASSWORD)")
    args = parser.parse_args()

    image, project, version = read_image(args.file)
    print(f"Image: {project} {version}, {len(image)} bytes")

    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # self-signed: checked by the fingerprint below
    connection = connect(args.host, args.port, args.family, context)

    presented = hashlib.sha256(connection.sock.getpeercert(binary_form=True)).hexdigest().upper()
    if args.fingerprint is not None:
        if not hmac.compare_digest(presented, args.fingerprint):
            raise SystemExit("error: the certificate fingerprint does not match\n"
                             f"  expected {format_fingerprint(args.fingerprint)}\n"
                             f"  device   {format_fingerprint(presented)}")
    elif not args.insecure:
        raise SystemExit("error: give the certificate fingerprint with --fingerprint (or use --insecure)\n"
                         f"  the device presented {format_fingerprint(presented)}\n"
                         "  compare it with the fingerprint on the web admin page or the serial console")

    password = os.environ.get(args.password_env) or getpass.getpass("Web admin password: ")
    if not password:
        raise SystemExit("error: no web admin password")
    credentials = base64.b64encode(f"{BASIC_USER}:{password}".encode("utf-8")).decode("ascii")

    deadline = time.monotonic() + TOTAL_TIMEOUT
    connection.putrequest("POST", "/api/ote")
    connection.putheader("Authorization", "Basic " + credentials)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(len(image)))
    connection.putheader("Connection", "close")
    connection.endheaders()
    password = None
    credentials = None

    sent = 0
    send_error = None
    try:
        response = early_answer(connection)
        if response is None:
            while sent < len(image):
                if time.monotonic() > deadline:
                    raise SystemExit("error: upload deadline exceeded; check the device before trying again")
                try:
                    connection.send(image[sent:sent + CHUNK_SIZE])
                except OSError as error:  # the device refused the image and closed the connection: read its answer
                    send_error = error
                    break
                sent += min(CHUNK_SIZE, len(image) - sent)
                print(f"\rSent {sent * 100 // len(image):3d} %", end="", flush=True)
            print()
            response = connection.getresponse()
        body = response.read(4096)
    except (OSError, http.client.HTTPException) as error:
        raise SystemExit(f"error: no answer from the device ({send_error or error}); "
                         "the reason is shown on its LCD and serial console")
    finally:
        connection.close()
    try:
        data = json.loads(body.decode("utf-8"))
    except ValueError:
        data = {}
    if response.status == 200:
        print(data.get("message", "Firmware accepted."))
        return 0
    print(f"error: {response.status} {response.reason}: {data.get('error', body.decode('utf-8', 'replace'))}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
