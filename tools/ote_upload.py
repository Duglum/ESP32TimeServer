"""Upload a firmware image to the ESP32 Time Server (Over The Ethernet update over HTTPS).

The image (ESP32TimeServer.bin of the build folder, not the merged flash image) is sent to
POST https://<device>/api/ote of the web admin server. The OTE password of the device is sent as
"Authorization: Bearer <password>" inside the TLS connection.

The certificate of the device is self-signed, so it is checked by its SHA-256 fingerprint (shown on the
web admin page under Status and on the serial console at every start). Without --fingerprint the upload
stops and prints the fingerprint the device presented; --insecure skips the check (only on a trusted LAN).

Example:
  python tools\\ote_upload.py -i ESP32TimeServer.local -f build\\ESP32TimeServer.bin --fingerprint AB:CD:...
The password is read from the environment variable OTE_PASSWORD, otherwise it is asked for.
"""
import argparse
import getpass
import hashlib
import hmac
import http.client
import json
import os
import ssl
import sys
import time
from pathlib import Path

DEFAULT_PORT = 443
CHUNK_SIZE = 16384
SOCKET_TIMEOUT = 30.0  # the device waits at most 15 s for data, and needs a moment per flash sector
TOTAL_TIMEOUT = 330.0  # the device ends an upload after 300 s


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


def main():
    parser = argparse.ArgumentParser(description="Over The Ethernet update of the ESP32 Time Server (HTTPS).")
    parser.add_argument("-i", "--host", required=True, help="host name or address of the device (IPv6 without brackets)")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT, help="HTTPS port (default 443)")
    parser.add_argument("-f", "--file", required=True, help="application image (.bin)")
    parser.add_argument("--fingerprint", type=normalise_fingerprint,
                        help="SHA-256 fingerprint of the device certificate (web admin page, Status)")
    parser.add_argument("--insecure", action="store_true", help="do not check the certificate fingerprint")
    parser.add_argument("--password-env", default="OTE_PASSWORD", metavar="NAME",
                        help="environment variable with the OTE password (default OTE_PASSWORD)")
    args = parser.parse_args()

    image, project, version = read_image(args.file)
    print(f"Image: {project} {version}, {len(image)} bytes")

    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # self-signed: checked by the fingerprint below
    connection = http.client.HTTPSConnection(args.host, args.port, timeout=SOCKET_TIMEOUT, context=context)
    try:
        connection.connect()
    except (OSError, ssl.SSLError) as error:
        raise SystemExit(f"error: no connection to {args.host}:{args.port}: {error}")

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

    password = os.environ.get(args.password_env) or getpass.getpass("OTE password: ")
    if not password:
        raise SystemExit("error: no OTE password")

    deadline = time.monotonic() + TOTAL_TIMEOUT
    connection.putrequest("POST", "/api/ote")
    connection.putheader("Authorization", "Bearer " + password)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(len(image)))
    connection.putheader("Connection", "close")
    connection.endheaders()
    password = None

    sent = 0
    send_error = None
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

    try:
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
