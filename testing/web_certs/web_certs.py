"""Certificates for the tests of the own web admin certificate of ESP32 Time Server (testing/README.md).

Writes into the output directory (PEM; paste a chain-*.pem into "Certificate chain" and the matching key-*.pem into
"Private key" under Security, TLS certificate of the web admin):
  ca.pem                       the test CA ("ESP32 Time Server own-certificate TEST CA"): import it into a browser to trust
                               the certificates below, or leave it and accept the warning
  chain-ecdsa.pem, key-ecdsa.pem   ECDSA P-256 server certificate followed by the CA, and its key
  chain-rsa.pem, key-rsa.pem       the same with RSA 2048
  chain-expired.pem            ECDSA, valid only in January 2025 (key-ecdsa.pem): stored with a warning
  key-other.pem                another ECDSA key: refused, it does not belong to the certificate
  key-encrypted.pem            key-ecdsa.pem encrypted with the password "test": refused
  key-p521.pem, chain-p521.pem a P-521 key and certificate: refused (only P-256, P-384, RSA 2048 to 4096)
SANs: the given DNS names and IP addresses. Needs OpenSSL 3.4 or newer (the one of Git for Windows will do).
Usage:
  python -I testing/web_certs/web_certs.py --ip 192.168.1.50 [--dns ESP32TimeServer --dns ESP32TimeServer.example.com] [--out web-certs]
The output directory holds private keys: it is in .gitignore; never commit a key from it.
"""
import argparse
import ipaddress
import os
import pathlib
import secrets
import shutil
import subprocess
import sys

CONFIG = """[req]
distinguished_name = dn
prompt = no
[dn]
CN = unused
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[server]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
subjectAltName = {names}
"""


def find_openssl(given):
    if given:
        if not os.path.isfile(given):
            sys.exit("OpenSSL not found at %s" % given)
        return given
    for candidate in (shutil.which("openssl"), r"C:\Program Files\Git\mingw64\bin\openssl.exe", r"C:\Program Files\Git\usr\bin\openssl.exe"):
        if candidate and os.path.isfile(candidate):
            return candidate
    sys.exit("OpenSSL not found: give its path with --openssl")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ip", action="append", default=[], help="IP address of the device (repeatable)")
    parser.add_argument("--dns", action="append", default=[], help="DNS name of the device (repeatable)")
    parser.add_argument("--out", default="web-certs", help="output directory (default web-certs)")
    parser.add_argument("--openssl", help="path of openssl")
    args = parser.parse_args()
    for ip in args.ip:
        try:
            ipaddress.ip_address(ip)
        except ValueError:
            sys.exit("Not an IP address: %s" % ip)
    if not args.ip and not args.dns:
        sys.exit("Give at least one --ip or --dns")
    openssl = find_openssl(args.openssl)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    names = ["DNS:" + d for d in args.dns] + ["IP:" + i for i in args.ip]
    (out / "openssl.cnf").write_text(CONFIG.format(names=", ".join(names)), encoding="ascii")
    cn = args.dns[0] if args.dns else args.ip[0]

    def run(*arguments):
        result = subprocess.run([openssl, *arguments], cwd=out, capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit("openssl %s failed:\n%s" % (arguments[0], result.stderr))

    def serial():
        return "0x" + secrets.token_hex(16)

    def server(name, key, validity):
        run("req", "-new", "-config", "openssl.cnf", "-key", key, "-subj", "/CN=%s/O=ESP32 Time Server own certificate TEST" % cn, "-out", name + ".csr")
        run("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-set_serial", serial(), *validity,
            "-extfile", "openssl.cnf", "-extensions", "server", "-out", name + ".crt")
        chain = (out / (name + ".crt")).read_text(encoding="ascii") + (out / "ca.pem").read_text(encoding="ascii")
        (out / ("chain-%s.pem" % name)).write_text(chain, encoding="ascii", newline="\n")
        (out / (name + ".csr")).unlink()
        (out / (name + ".crt")).unlink()

    run("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", "ca.key")
    run("req", "-x509", "-new", "-config", "openssl.cnf", "-key", "ca.key", "-subj", "/CN=ESP32 Time Server own-certificate TEST CA",
        "-days", "3650", "-set_serial", serial(), "-extensions", "ca", "-out", "ca.pem")
    run("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", "key-ecdsa.pem")
    run("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", "key-rsa.pem")
    run("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", "key-other.pem")
    run("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-521", "-out", "key-p521.pem")
    run("pkey", "-in", "key-ecdsa.pem", "-aes-128-cbc", "-passout", "pass:test", "-out", "key-encrypted.pem")
    server("ecdsa", "key-ecdsa.pem", ["-days", "365"])
    server("rsa", "key-rsa.pem", ["-days", "365"])
    server("expired", "key-ecdsa.pem", ["-not_before", "20250101000000Z", "-not_after", "20250201000000Z"])
    server("p521", "key-p521.pem", ["-days", "365"])
    (out / "openssl.cnf").unlink()
    (out / "ca.key").unlink()
    print("Written to %s:" % out.resolve())
    for path in sorted(out.iterdir()):
        print("  " + path.name)


if __name__ == "__main__":
    main()
