# Web admin API

The web admin page of the ESP32 Time Server talks to the device through a small JSON API under `https://<FQDN or IP address>/api/`. This file lists every endpoint, how to call it and how the authentication works. Source: `main/web/web_server.cpp` (route table in `register_handlers()`), `main/web/web_auth.cpp` and `main/web/web_auth.h`.

## Authentication

1. **Allow-list.** Each new TCP connection is checked against the allow-list first. A source that is not allowed is closed before the TLS handshake. The allow-list applies to every path, the static files included.
2. **TLS.** Everything runs over HTTPS with the device's own certificate. It is self-signed and created at the first start. Compare its SHA-256 fingerprint with the one in the *Status* section or on the serial console.
3. **Login.** `POST /api/login` with `{"password": "..."}` returns:
   - a session cookie `__Host-sid` (a 256-bit token, `Secure; HttpOnly; SameSite=Strict`);
   - a CSRF token in the body: `{"csrf": "..."}`. `GET /api/state` returns it again (field `csrf`), so the page has it after a reload.

   Session limits:
   - at most 4 sessions;
   - a session ends after 15 min without use, and after 12 h at the latest.
4. **Read requests (GET)** need a valid session cookie.
5. **State-changing requests (POST)** need the session cookie and all of the following. Otherwise the device answers 403.
   - `Origin: https://<Host>`, exactly matching the `Host` header;
   - `Sec-Fetch-Site: same-origin`, if the client sends this header;
   - `Content-Type: application/json`;
   - `X-CSRF-Token: <csrf>`.
6. **Throttling.** One counter per source address covers failed logins, failed password checks and failed scripted uploads. After 5 failures:
   - the device answers `429 Too Many Requests` with `Retry-After`;
   - the lock-out starts at 30 s and doubles up to 15 min.
7. **Password storage.** The password is stored as a salted PBKDF2 hash (10000 iterations) in NVS. The compiled password from `ESP32TimeServerSettingsSecrets.h` (default `password`) is used until one is set on the page. While the default password is in use:
   - the page shows a red warning;
   - scripted uploads are refused.

Request bodies are limited to 4096 bytes, except the firmware upload. Errors come back as `{"error": "..."}` with a matching HTTP status.

## Endpoints

| Endpoint | Method | Authentication | What it does |
|---|---|---|---|
| `/api/info` | GET | **none** | Host name and FQDN (for the login page) |
| `/api/login` | POST | same origin, password in the body | Starts a session: cookie and CSRF token. `401` wrong password, `429` with `Retry-After` while locked out |
| `/api/logout` | POST | cookie + CSRF (without them only the cookie is cleared) | Ends the session |
| `/api/state` | GET | cookie | Settings in use (`settings`), `defaults`, `saved`, compiled NTP keys, allow-list, `status` (network, time, GNSS, PPS, ...), certificate fingerprint, CSRF token. Secrets (passwords, keys) are not sent back |
| `/api/diag` | GET | cookie | Diagnostics: servo, ADEV/TDEV, ISR histogram, NTP paths, basic mode TX delay, load shedding, RMC phase, UBX details |
| `/api/coredump` | GET | cookie | Downloads the stored core dump (`404` if there is none) |
| `/api/apply` | POST | cookie + CSRF | Applies settings now, without storing them. The body is a settings object with the field names of `settings` in `/api/state`; missing fields keep their current value. See the notes below the table. Errors: `400` invalid value, `409` while a network change waits for its confirmation |
| `/api/network/confirm` | POST | cookie + CSRF | Confirms the pending network change (body `{}`). A new static address must be confirmed over that address (`409` otherwise) |
| `/api/save` | POST | cookie + CSRF | Stores the settings (same body as `/api/apply`) in NVS; they are loaded at every start. `409` while network changes are not applied and confirmed yet |
| `/api/reboot` | POST | cookie + CSRF | Restarts with the saved settings; applied but unsaved settings are discarded |
| `/api/password` | POST | cookie + CSRF + current password | `{"current": "...", "new": "..."}`; the new password has 12 to 128 characters |
| `/api/allow-list` | POST | cookie + CSRF | `{"entries": ["192.168.8.0/24", "2001:db8::/64", ...]}`: source addresses or networks (CIDR) allowed to connect. `409` if your own address would no longer be allowed |
| `/api/factory-reset` | POST | cookie + CSRF + current password | *Reset to defaults* (`{"password": "..."}`): erases the saved settings, the password and the allow-list, then restarts. The TLS key and certificate are kept |
| `/api/ote/once` | POST | cookie + CSRF | Restarts once with firmware updates (OTE) switched on, until the next restart |
| `/api/ote` | POST | cookie + CSRF (page) **or** HTTP Basic (script) | Firmware upload. The body is the raw application image (`ESP32TimeServer.bin`, `application/octet-stream`). `403` while OTE updates are switched off. See below |
| `/api/coredump/erase` | POST | cookie + CSRF | Erases the stored core dump |

Notes on `/api/apply`:
- A network change must be confirmed within 120 s, otherwise the previous network settings are restored.
- A change of a restart field (FQDN, pins, LED module, MQTT, ...) writes a one-shot set and restarts once with it.

**Static files** need no login; only the allow-list applies: `/`, `/app.js`, `/app.css`, `/i18n.js`, `/favicon.ico`.

## Firmware upload from a script (`/api/ote`)

`tools/ote_upload.py` uses this path. The request carries `Authorization: Basic base64(<any user name>:<web admin password>)` inside the TLS connection. It needs no cookie, CSRF token or `Origin`.

- The upload is refused with `403` while the default password is in use.
- A wrong password counts for the throttling like a failed login: `401`, and later `429`.
- On a refusal the device answers as soon as it has the headers and the first byte of the body. It then closes the connection without reading the image. The script therefore sends the headers with the first byte and waits up to 2 s for an answer before it sends the rest. Otherwise Windows can lose the answer to a connection reset.

## Example with curl

The certificate is self-signed: `-k` skips the check, so compare the fingerprint yourself. Replace `HOST` with the FQDN or IP address.

```bash
# Log in: stores the session cookie in "jar" and prints the CSRF token
curl -k -c jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -d '{"password":"<password>"}' https://HOST/api/login

# Read the state
curl -k -b jar https://HOST/api/state

# State-changing request, e.g. a restart
curl -k -b jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -H "X-CSRF-Token: <csrf>" -d '{}' https://HOST/api/reboot
```

Keep this file up to date when an endpoint, its authentication or its behaviour changes.
