"""Board test of the RADIUS and TOTP logins of the web page, against the RADIUS test server of this folder
(radius_server.py) that runs on this PC during the test. Taken from ESP32-ConsoleServer (tests 1 to 9 and 16 to 19,
without the SSH tests 10 to 15, 20 and 21).

    python -I testing/board/radius_logins.py <device name> [--pc-address 192.168.8.100] [--only 1,3] [--no-throttle]

Takes the web password from the environment variable WEB_ADMIN_PASSWORD or asks for it (a login of the page is needed
to change the settings). The settings are changed with Apply
only (never saved) and set back at the end; a restart would set them back as well. Every test login logs out again
(the device keeps at most four sessions: a new login replaces the one used least recently). The PC must accept UDP on ports 18120 and
18121 from the device (the Windows firewall asks once for Python).

What is tested (numbers for --only):
  1  the RADIUS test of Security (POST /api/radius/test): accept, privilege level too low, reject, required attribute
  2  web logins by RADIUS: admin15 accepted (user and method in "session" of /api/state), helpdesk (level 1) and a wrong
     password refused with the same answer, an unknown user refused
  3  the attributes of the Access-Request: Message-Authenticator first and valid, NAS-Identifier, NAS-IP-Address,
     NAS-Port 0, NAS-Port-Type 5, Service-Type 1, NAS-Port-Id https, Calling-Station-Id = this PC
  4  answers that must be dropped: wrong Response Authenticator, wrong or missing Message-Authenticator, another
     identifier; an Access-Challenge refused; no Message-Authenticator accepted once "require" is off
  5  no answer: "No RADIUS server answered"; the local password of admin as the fallback; failover to server 2; the
     dead time (the dead server is skipped at once); both dead: the fallback at once
  6  local password first (local_fallback off): admin with the local password, other names by RADIUS
  7  the lock-out guard: the local password can only be switched off in a RADIUS session; then admin is refused
  8  one login per source address at a time (503 for the second while the first waits)
  9  the throttling: five refused RADIUS logins lock the source out (429), a success later forgets them (skipped with
     --no-throttle: it locks this PC out of the web page for 30 s)
TOTP of the web page (the device needs the time from GNSS; the codes are computed here like an authenticator app):
 16  a token for admin15 set up on the page (secret, URI, QR code), confirmed with a code: 10 recovery codes, in "totp"
 17  login of admin15 with the second factor: the pending login, a wrong code, the right one; the same code again is
     refused (each code once); a recovery code works once; three wrong codes: the password again
 18  the local user: "Required for the local user" refused without a token; with one admin needs its code; its token
     cannot be deleted while it is required
 19  set up at the first login (totp_enroll): adminst gets a new token with the login, the first code stores it
"""
import argparse
import base64
import getpass
import hashlib
import hmac
import http.client
import json
import os
import ssl
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import radius_server as rs  # noqa: E402

SECRET1 = "radius-test-secret-1"
SECRET2 = "radius-test-secret-2"
PW = rs.DEFAULT_PASSWORD
results = []


class Device:
    """One HTTPS connection to the device with its session cookie and CSRF token (the certificate is not checked)."""

    def __init__(self, host):
        self.host = host
        self.context = ssl.create_default_context()
        self.context.check_hostname = False
        self.context.verify_mode = ssl.CERT_NONE
        self.connection = None
        self.cookie = ""
        self.csrf = ""

    def request(self, method, path, body=None):
        headers = {"Origin": f"https://{self.host}", "Content-Type": "application/json"}
        if self.cookie:
            headers["Cookie"] = self.cookie
        if self.csrf:
            headers["X-CSRF-Token"] = self.csrf
        for attempt in range(2):
            try:
                if self.connection is None:
                    self.connection = http.client.HTTPSConnection(self.host, 443, timeout=30, context=self.context)
                self.connection.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
                response = self.connection.getresponse()
                data = response.read()
                break
            except (http.client.HTTPException, OSError):
                # the device has 3 sockets and closes the one used least recently for a new connection
                self.connection = None
                if attempt == 1:
                    raise
        for name, value in response.getheaders():
            if name.lower() == "set-cookie" and value.startswith("__Host-sid="):
                self.cookie = value.split(";", 1)[0]
        try:
            parsed = json.loads(data) if data else {}
        except ValueError:
            parsed = {}
        return response.status, parsed

    def state(self):
        status, state = self.request("GET", "/api/state")
        if status != 200:
            raise RuntimeError(f"/api/state: {status} {state.get('error', '')}")
        return state


def result(ok, name, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
    return ok


class Web:
    """A web session of the test (logged in as admin with the web password, or by RADIUS)."""

    def __init__(self, host):
        self.d = Device(host)

    def login(self, user, password):
        body = {"user": user, "password": password}
        status, reply = self.d.request("POST", "/api/login", body)
        if status == 200:
            self.d.csrf = reply.get("csrf", "")
        return status, reply

    def logout(self):
        if self.d.csrf:
            self.d.request("POST", "/api/logout", {})
        self.d.csrf = ""
        self.d.cookie = ""
        if self.d.connection is not None:
            self.d.connection.close()  # one socket less in use on the device
            self.d.connection = None

    def apply(self, body):
        return self.d.request("POST", "/api/apply", body)


def totp_code(secret_base32, at=None, offset=0):
    """The 6-digit code of an authenticator app (RFC 6238, SHA-1, 30 s)"""
    key = base64.b32decode(secret_base32.replace(" ", "").upper() + "=" * (-len(secret_base32.replace(" ", "")) % 8))
    counter = int((time.time() if at is None else at) // 30) + offset
    mac = hmac.new(key, counter.to_bytes(8, "big"), hashlib.sha1).digest()
    o = mac[19] & 15
    return f"{(int.from_bytes(mac[o:o + 4], 'big') & 0x7FFFFFFF) % 1000000:06d}"


def next_step_code(secret):
    """A code of a time step not used yet: waits for the next step (each code works once)"""
    time.sleep(30 - time.time() % 30 + 0.5)
    return totp_code(secret)


def totp_tests(a, admin, apply, wanted, web_on, srv1, web_password):
    st = admin.d.state()
    if not st["totp"]["time_ok"]:
        result(False, "16 the clock of the device is set (GNSS): needed for TOTP")
        return
    created = []

    def enroll(user):
        status, reply = admin.d.request("POST", "/api/totp/enroll", {"user": user})
        if status != 200:
            return None, f"{status} {reply}"
        code = totp_code(reply["secret"])
        status, confirm = admin.d.request("POST", "/api/totp/confirm", {"user": user, "code": code})
        if status == 200:
            created.append(user)
        return (reply, confirm) if status == 200 else (None, f"{status} {confirm}"), ""

    def web_login(user, password):
        w = Web(a.device)
        status, reply = w.login(user, password)
        return w, status, reply

    def second(w, pending, code):
        body = {"pending": pending, "code": code}
        status, reply = w.d.request("POST", "/api/login/totp", body)
        if status == 200 and reply.get("csrf"):
            w.d.csrf = reply["csrf"]
        return status, reply

    try:
        if wanted(16):
            (reply, confirm), err = enroll("admin15")
            ok = reply is not None
            result(ok and len(reply["secret"].replace(" ", "")) == 32 and reply["uri"].startswith("otpauth://totp/") and
                   reply["qr"]["size"] >= 21 and len(reply["qr"]["rows"]) == reply["qr"]["size"], "16 a new token: secret, URI, QR code",
                   err or reply["uri"].split("?")[0])
            result(ok and len(confirm.get("recovery_codes", [])) == 10, "16 confirmed: 10 recovery codes", err or str(confirm.get("recovery_codes", [])[:2]))
            tokens = {t["user"]: t for t in admin.d.state()["totp"]["tokens"]}
            result("admin15" in tokens and tokens["admin15"]["recovery_left"] == 10, "16 in totp.tokens", str(tokens.get("admin15")))
            secret15 = reply["secret"] if ok else ""
            recovery15 = confirm.get("recovery_codes", []) if ok else []
            status, bad = admin.d.request("POST", "/api/totp/confirm", {"user": "admin15", "code": "000000"})
            result(status == 409, "16 a second confirm without a new setup is refused", f"{status} {bad}")
        if wanted(17) and "admin15" in created:
            apply(dict(web_on, totp_radius=True))
            w, status, reply = web_login("admin15", PW)
            pending = reply.get("pending", "")
            result(status == 200 and reply.get("totp") is True and pending and "csrf" not in reply, "17 the password: a pending login", f"{status} {reply}")
            status, reply = second(w, pending, "000000" if totp_code(secret15) != "000000" else "111111")
            result(status == 401 and reply.get("error") == "Wrong code", "17 a wrong code", f"{status} {reply}")
            code = totp_code(secret15)
            status, reply = second(w, pending, code)
            result(status == 200 and reply.get("user") == "admin15" and reply.get("radius") is True, "17 the right code: logged in", f"{status} {reply}")
            w.logout()
            w, status, reply = web_login("admin15", PW)
            status, reply = second(w, reply.get("pending", ""), code)
            result(status == 401, "17 the same code again is refused", f"{status} {reply}")
            w, status, reply = web_login("admin15", PW)
            status, reply = second(w, reply.get("pending", ""), recovery15[0] if recovery15 else "")
            result(status == 200, "17 a recovery code", f"{status} {reply.get('error', '')}")
            w.logout()
            left = {t["user"]: t for t in admin.d.state()["totp"]["tokens"]}["admin15"]["recovery_left"]
            result(left == 9, "17 one recovery code used", f"{left} left")
            w, status, reply = web_login("admin15", PW)
            pending = reply.get("pending", "")
            status, reply = second(w, pending, recovery15[0] if recovery15 else "")
            result(status == 401, "17 the used recovery code is refused", f"{status} {reply}")
            second(w, pending, "000001")
            status, reply = second(w, pending, "000002")
            result(status == 401 and "log in again" in reply.get("error", ""), "17 three wrong codes: the password again", f"{status} {reply}")
            status, reply = second(w, pending, totp_code(secret15))
            result(status == 401 and "expired" in reply.get("error", ""), "17 the pending login is gone", f"{status} {reply}")
            apply(web_on)
            # a success forgets the failures of the codes (5 lock out)
            Web(a.device).login("admin15", PW)
        if wanted(18):
            status, reply = admin.apply({"web_login": dict(web_on, totp_local=True)})
            result(status == 400 and "TOTP token for admin" in reply.get("error", ""), "18 the local second factor needs a token for admin", f"{status} {reply}")
            (reply, confirm), err = enroll("admin")
            secret_admin = reply["secret"] if reply else ""
            if result(reply is not None, "18 a token for admin", err):
                apply(dict(web_on, totp_local=True, local_fallback=False))
                w, status, reply = web_login("", web_password)
                result(status == 200 and reply.get("totp") is True, "18 admin: the password, then the code", f"{status} {reply}")
                status, reply = second(w, reply.get("pending", ""), next_step_code(secret_admin))
                result(status == 200 and reply.get("user") == "admin", "18 admin logged in with its code", f"{status} {reply.get('error', '')}")
                w.logout()
                status, reply = admin.d.request("POST", "/api/totp/delete", {"user": "admin"})
                result(status == 409, "18 admin's token cannot be deleted while it is required", f"{status} {reply}")
                apply(web_on)
                status, reply = admin.d.request("POST", "/api/totp/delete", {"user": "admin"})
                result(status == 200, "18 deleted when no longer required", f"{status} {reply}")
                if status == 200:
                    created.remove("admin")
        if wanted(19):
            apply(dict(web_on, totp_radius=True, totp_enroll=True))
            w, status, reply = web_login("adminst", PW)
            ok = status == 200 and reply.get("totp_enroll") is True and reply.get("secret")
            result(bool(ok), "19 the first login of adminst: a new token with the login", f"{status} {reply.get('error', '')}")
            if ok:
                status, done = second(w, reply["pending"], totp_code(reply["secret"]))
                result(status == 200 and len(done.get("recovery_codes", [])) == 10 and done.get("csrf"), "19 the first code stores it and logs in",
                       f"{status} {done.get('error', '')}")
                created.append("adminst")
                w.logout()
                tokens = {t["user"] for t in admin.d.state()["totp"]["tokens"]}
                result("adminst" in tokens, "19 adminst has a token now", str(tokens))
            apply(dict(web_on, totp_radius=True, totp_enroll=False))
            w, status, reply = web_login("classed", PW)
            result(status == 401 and "No TOTP token" in reply.get("error", ""), "19 without totp_enroll a user without a token is refused", f"{status} {reply}")
            apply(web_on)
            Web(a.device).login("admin15", PW)
    finally:
        apply(web_on)
        for user in created:
            status, reply = admin.d.request("POST", "/api/totp/delete", {"user": user})
            print(f"token of {user} deleted: {status} {reply.get('error', '')}", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("device")
    p.add_argument("--pc-address", default="192.168.8.100", help="the address of this PC as the device reaches it")
    p.add_argument("--only", default="", help="comma separated test numbers")
    p.add_argument("--no-throttle", action="store_true")
    a = p.parse_args()
    only = {int(x) for x in a.only.split(",") if x.strip()}

    def wanted(n):
        return not only or n in only

    web_password = os.environ.get("WEB_ADMIN_PASSWORD") or getpass.getpass("Password of the web page: ")
    admin = Web(a.device)
    status, reply = admin.login("", web_password)
    if status != 200:
        raise SystemExit(f"Login of the web page: {status} {reply.get('error', '')}")
    state = admin.d.state()
    original = {"radius": state["settings"]["radius"], "web_login": state["settings"]["web_login"]}
    for server in original["radius"]["servers"]:
        server.pop("secret_set", None)  # an empty secret keeps the stored one

    srv1 = rs.RadiusTestServer(18120, SECRET1).start()
    srv2 = rs.RadiusTestServer(18121, SECRET2).start()
    servers = [{"host": a.pc_address, "port": 18120, "secret": SECRET1}, {"host": "", "port": 1812, "secret": ""}]
    radius = {"servers": servers, "timeout_s": 1, "retries": 1, "dead_time_min": 1, "nas_identifier": "", "nas_ip": "",
              "nas_ipv6": "", "service_type": 1, "nas_port_type": 5, "min_privilege": 15, "required": "",
              "require_message_authenticator": True}

    def apply(web_login=None, radius_changes=None, session=None):
        body = {"radius": dict(radius, **(radius_changes or {}))}
        if web_login is not None:
            body["web_login"] = web_login
        status, reply = (session or admin).apply(body)
        if status != 200:
            raise RuntimeError(f"Apply: {status} {reply.get('error', '')}")

    def login(user, password):
        w = Web(a.device)
        t0 = time.time()
        status, reply = w.login(user, password)
        return w, status, reply, time.time() - t0

    def try_login(user, password):
        w, status, reply, took = login(user, password)
        own = None
        if status == 200:
            own = w.d.state()["session"]
            w.logout()
        return status, reply, own, took

    def fresh():
        # Between the tests: the servers answer again, Apply forgets their dead marks, and a successful login
        # forgets the failures of this PC (the refusals of a test count for the lock-out, 5 lock it out)
        srv1.mode = srv2.mode = "normal"
        apply(web_on)
        status, reply, own, took = try_login("admin15", PW)
        if status != 200:
            print(f"  (fresh: {status} {reply.get('error', '')})", flush=True)

    web_on = {"local": True, "radius": True, "local_fallback": True, "totp_local": False, "totp_radius": False, "totp_enroll": False}
    try:
        if wanted(1):
            for user, mode, expected in [("admin15", "normal", "accept"), ("helpdesk", "normal", "low_privilege"),
                                         ("nobody", "normal", "reject"), ("adminst", "normal", "accept")]:
                srv1.mode = mode
                status, reply = admin.d.request("POST", "/api/radius/test", {"user": user, "password": PW, "radius": radius})
                result(status == 200 and reply.get("result") == expected, f"1 test of Security: {user}", f"{status} {reply}")
            status, reply = admin.d.request("POST", "/api/radius/test",
                                            {"user": "admin15", "password": PW, "radius": dict(radius, required="Class=CS-ADMIN")})
            result(reply.get("result") == "missing_attribute", "1 test: required attribute missing", str(reply))
            status, reply = admin.d.request("POST", "/api/radius/test",
                                            {"user": "classed", "password": PW, "radius": dict(radius, required="Class=CS-ADMIN")})
            result(reply.get("result") == "accept", "1 test: required attribute present", str(reply))
            status, reply = admin.d.request("POST", "/api/radius/test", {"user": "", "password": PW, "radius": radius})
            result(status == 400, "1 test: an empty user name is refused", f"{status} {reply}")

        apply(web_on)
        if wanted(2):
            srv1.mode = "normal"
            status, reply, own, took = try_login("admin15", PW)
            result(status == 200 and reply.get("user") == "admin15" and reply.get("radius") is True, "2 admin15 accepted by RADIUS",
                   f"{status} {reply.get('error', '')} {took:.1f} s")
            result(own is not None and own.get("user") == "admin15" and own.get("radius") is True, "2 session: user and method", str(own))
            for user, password, why in [("helpdesk", PW, "privilege level 1"), ("admin15", "wrong-password", "wrong password"),
                                        ("nobody", PW, "unknown user")]:
                status, reply, own, took = try_login(user, password)
                result(status == 401 and reply.get("error") == "Wrong user name or password", f"2 refused: {why}", f"{status} {reply}")
            info_status, info = Web(a.device).d.request("GET", "/api/info")
            result(info.get("login_user") is True, "2 /api/info: the login page asks for a user name", str(info))

        if wanted(3):
            srv1.clear()
            try_login("admin15", PW)
            r = srv1.last() or {}
            ok = (r.get("_order", [""])[0] == "Message-Authenticator" and r.get("_ma_ok") and r.get("NAS-Port") == 0 and
                  r.get("NAS-Port-Type") == 5 and r.get("Service-Type") == 1 and r.get("NAS-Port-Id") == "https" and
                  r.get("Calling-Station-Id") == a.pc_address and r.get("NAS-IP-Address") not in (None, "0.0.0.0") and r.get("NAS-Identifier"))
            result(bool(ok), "3 the attributes of the Access-Request", str({k: v for k, v in r.items() if k != "_password"}))
            apply(web_on, {"nas_identifier": "ts-radius-test", "nas_ip": "192.0.2.44", "service_type": 6, "nas_port_type": 0})
            try_login("admin15", PW)
            r = srv1.last() or {}
            result(r.get("NAS-Identifier") == "ts-radius-test" and r.get("NAS-IP-Address") == "192.0.2.44" and r.get("Service-Type") == 6 and
                   r.get("NAS-Port-Type") == 0, "3 the attributes as set", str({k: v for k, v in r.items() if k != "_password"}))
            apply(web_on, {"service_type": 0, "nas_port_type": -1})
            try_login("admin15", PW)
            r = srv1.last() or {}
            result("Service-Type" not in r and "NAS-Port-Type" not in r, "3 Service-Type and NAS-Port-Type not sent", str(r.get("_order")))
            apply(web_on)

        if wanted(4):
            for mode in ["bad_auth", "bad_ma", "no_ma", "wrong_id"]:
                fresh()
                srv1.mode = mode
                before = admin.d.state()["radius_status"]["servers"][0]["dropped"]
                status, reply, own, took = try_login("admin15", PW)
                after = admin.d.state()["radius_status"]["servers"][0]["dropped"]
                result(status == 401 and reply.get("error") == "No RADIUS server answered" and after >= before + 2,
                       f"4 {mode}: dropped, no answer", f"{status} {reply.get('error', '')} dropped {before}->{after} {took:.1f} s")
            fresh()
            srv1.mode = "challenge"
            status, reply, own, took = try_login("admin15", PW)
            result(status == 401 and reply.get("error") == "Wrong user name or password", "4 Access-Challenge refused", f"{status} {reply}")
            fresh()
            srv1.mode = "no_ma"
            apply(web_on, {"require_message_authenticator": False})
            status, reply, own, took = try_login("admin15", PW)
            result(status == 200, "4 no Message-Authenticator accepted with require off", f"{status} {reply.get('error', '')}")
            fresh()

        if wanted(5):
            srv1.mode = "silent"
            status, reply, own, took = try_login("admin15", PW)
            result(status == 401 and reply.get("error") == "No RADIUS server answered" and 1.5 < took < 4, "5 no answer (2 tries of 1 s)",
                   f"{status} {reply} {took:.1f} s")
            st = admin.d.state()["radius_status"]["servers"][0]
            result(st["dead"] and st["timeouts"] >= 1, "5 server 1 dead", str(st))
            status, reply, own, took = try_login("", web_password)
            result(status == 200 and reply.get("radius") is False and took < 1.9, "5 admin with the local password while the server is dead",
                   f"{status} {reply.get('error', '')} {took:.1f} s")
            apply(web_on, {"dead_time_min": 0})  # a change of the address forgets the dead mark; dead time 0: never dead
            status, reply, own, took = try_login("", web_password)
            result(status == 200 and reply.get("radius") is False and 1.5 < took < 4, "5 admin with the local password after no answer",
                   f"{status} {reply.get('error', '')} {took:.1f} s")
            status, reply, own, took = try_login("", "wrong-password")
            result(status == 401 and reply.get("error") == "Wrong user name or password", "5 a wrong local password after no answer", f"{status} {reply}")
            fresh()
            srv1.mode = "silent"
            servers[1] = {"host": a.pc_address, "port": 18121, "secret": SECRET2}
            apply(web_on, {"dead_time_min": 1})
            status, reply, own, took = try_login("admin15", PW)
            result(status == 200 and 0.8 < took < 3, "5 failover to server 2", f"{status} {reply.get('error', '')} {took:.1f} s")
            status, reply, own, took = try_login("admin15", PW)
            result(status == 200 and took < 0.8, "5 the dead server 1 is skipped", f"{status} {took:.2f} s")
            srv2.mode = "silent"
            status, reply, own, took = try_login("admin15", PW)
            result(status == 401 and reply.get("error") == "No RADIUS server answered", "5 server 2 silent too", f"{status} {reply} {took:.1f} s")
            status, reply, own, took = try_login("", web_password)
            result(status == 200 and took < 1.9, "5 both dead: the local password at once", f"{status} {took:.2f} s")
            servers[1] = {"host": "", "port": 1812, "secret": ""}
            fresh()

        if wanted(6):
            apply(dict(web_on, local_fallback=False))
            srv1.clear()
            status, reply, own, took = try_login("", web_password)
            result(status == 200 and reply.get("radius") is False and srv1.count() == 0, "6 admin: the local password, RADIUS not asked",
                   f"{status} {reply.get('error', '')} asked {srv1.count()}")
            status, reply, own, took = try_login("admin15", PW)
            result(status == 200 and reply.get("radius") is True, "6 another user by RADIUS", f"{status} {reply.get('error', '')}")
            status, reply, own, took = try_login("admin", "wrong-password")
            result(status == 401 and srv1.count() == 1, "6 a wrong local password: refused, RADIUS not asked", f"{status} asked {srv1.count()}")
            fresh()

        if wanted(7):
            status, reply = admin.apply({"web_login": dict(web_on, local=False)})
            result(status == 400 and "logged in by RADIUS" in reply.get("error", ""), "7 local off refused in a local session", f"{status} {reply}")
            r = Web(a.device)
            status, reply = r.login("admin15", PW)
            if result(status == 200, "7 RADIUS session", f"{status} {reply.get('error', '')}"):
                status, reply = r.apply({"web_login": dict(web_on, local=False)})
                result(status == 200, "7 local off in a RADIUS session", f"{status} {reply.get('error', '')}")
                srv1.mode = "silent"
                status, reply2, own, took = try_login("", web_password)
                result(status == 401, "7 admin refused while the local password is off (also without an answer)", f"{status} {reply2}")
                srv1.mode = "normal"
                status, reply = r.apply({"web_login": web_on, "radius": dict(radius, dead_time_min=1)})
                result(status == 200, "7 local on again", f"{status} {reply.get('error', '')}")
                r.logout()
            fresh()

        if wanted(8):
            srv1.mode = "silent"
            first = {}

            def slow():
                w, status, reply, took = login("admin15", PW)
                first["status"] = status

            t = threading.Thread(target=slow)
            t.start()
            time.sleep(0.5)
            status, reply, own, took = try_login("admin15", PW)
            result(status == 503, "8 a second login from the same address while the first waits", f"{status} {reply}")
            t.join()
            result(first.get("status") == 401, "8 the first one ends with no answer", str(first))
            fresh()

        if any(wanted(n) for n in range(16, 20)):
            fresh()
            totp_tests(a, admin, apply, wanted, web_on, srv1, web_password)

        if wanted(9) and not a.no_throttle:
            fresh()
            refused = 0
            for i in range(5):
                status, reply, own, took = try_login("nobody", PW)
                refused += status == 401
            status, reply, own, took = try_login("admin15", PW)
            result(refused == 5 and status == 429, "9 five refused RADIUS logins lock the source out", f"{refused} refused, then {status} {reply}")
            wait = int(reply.get("retry_after", 30)) + 1
            print(f"  waiting {wait} s for the lock-out to end", flush=True)
            time.sleep(wait)
            status, reply, own, took = try_login("admin15", PW)
            result(status == 200, "9 a RADIUS login after the lock-out", f"{status} {reply.get('error', '')}")
    finally:
        try:
            status, reply = admin.apply(original)
            print(f"settings set back: {status} {reply.get('error', '')}", flush=True)
        finally:
            admin.logout()
            srv1.stop()
            srv2.stop()
    print(f"{sum(results)} of {len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
