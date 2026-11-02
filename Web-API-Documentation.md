# Web admin API

The web admin page of the ESP32 Time Server talks to the device through a small JSON API under `https://<FQDN or IP address>/api/`. This file lists every endpoint, how to call it and how the authentication works. Source: `main/web/web_server.cpp` (route table in `register_handlers()`), `main/web/web_auth.cpp` and `main/web/web_auth.h`.

## Authentication

1. **Allow-list.** Each new TCP connection is checked against the allow-list first. A source that is not allowed is closed before the TLS handshake. The allow-list applies to every path, the static files included.
2. **TLS.** Everything runs over HTTPS with the device's own certificate, TLS 1.3 only: a client that only knows TLS 1.2 (e.g. `curl.exe` of Windows 10, which uses Schannel) is refused; use a browser, Python or a curl built with OpenSSL. It is self-signed and created at the first start. Compare its SHA-256 fingerprint with the one in the *Status* section or on the serial console.
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
   - the lock-out starts at 30 s and doubles with every further failure up to 1 h (30 s, 1, 2, 4, 8, 16, 32 min, then 1 h per failure: at most one guess per hour);
   - the failures of a source are forgotten after 24 h without a failure (the counter lives in RAM: a restart clears it).

   More than 20 failures per minute from all sources together block every login for 1 min (doubling up to 1 h, forgotten after 24 h without such a block).
7. **Password storage.** The password is stored as a salted PBKDF2 hash (10000 iterations) in NVS. The default password `password` is used until one is set on the page (there is no settings file: every setting is made on the page). While the default password is in use:
   - the page shows a red warning;
   - scripted uploads are refused.

Request bodies are limited to 4096 bytes; `/api/apply`, `/api/save` and `/api/config` (import) take up to 12288 bytes (the MQTT CA certificate may have 4096 characters), the firmware upload more. A JSON body may nest at most 8 levels deep and have at most 512 separators (`,` and `:` outside strings); otherwise, or when it is no JSON object, the answer is `400`. Errors come back as `{"error": "..."}` with a matching HTTP status.

## Endpoints

| Endpoint | Method | Authentication | What it does |
|---|---|---|---|
| `/api/info` | GET | **none** | Host name and FQDN (for the login page) |
| `/api/login` | POST | same origin, password in the body | Starts a session: cookie and CSRF token. `401` wrong password, `429` with `Retry-After` while locked out |
| `/api/logout` | POST | cookie + CSRF (without them only the cookie is cleared) | Ends the session |
| `/api/state` | GET | cookie | Settings in use (`settings`), `defaults`, `saved`, allow-list, `status` (network: link state, speed, duplex and driver restarts in `link_up`, `link_speed_mbps`, `link_full_duplex`, `eth_recoveries`; MQTT: `mqtt_active`, `mqtt_connected`, `mqtt_broker`, `mqtt_port`, `mqtt_tls`, `mqtt_error` with `mqtt_error_age_s`, `mqtt_connected_s`, `mqtt_published`, `mqtt_publish_age_s`, `mqtt_tf_queue`; time, GNSS, PPS, ...), certificate fingerprint, CSRF token. Secrets (passwords, keys) are not sent back |
| `/api/diag` | GET | cookie | Diagnostics: servo, ADEV/TDEV, ISR histogram, NTP paths, basic mode TX delay, load shedding, RMC phase, UBX details |
| `/api/coredump` | GET | cookie | Downloads the stored core dump (`404` if there is none) |
| `/api/apply` | POST | cookie + CSRF | Applies settings now, without storing them. The body is a settings object with the field names of `settings` in `/api/state`; missing fields keep their current value. See the notes below the table. Errors: `400` invalid value, `409` while a network change waits for its confirmation |
| `/api/network/confirm` | POST | cookie + CSRF | Confirms the pending network change (body `{}`). A new static address must be confirmed over that address (`409` otherwise) |
| `/api/save` | POST | cookie + CSRF | Stores the settings (same body as `/api/apply`) in NVS; they are loaded at every start. `409` while network changes are not applied and confirmed yet |
| `/api/config` | GET | cookie + CSRF header | *Export configuration*: the configuration file (JSON, `Content-Disposition: attachment; filename="timeserver-config-<host name>.json"`), see *Configuration file* below. Contains secrets, so besides the cookie it needs the `X-CSRF-Token` header, and a `Sec-Fetch-Site` header (sent by browsers) must be `same-origin` (`403` otherwise) |
| `/api/config` | POST | cookie + CSRF | *Import configuration*: the body is a configuration file. Replaces every setting and the allow-list, see *Configuration file* below. Errors: `400` not a configuration file or an invalid value, `409` while a network change waits for its confirmation or when the allow-list of the file does not allow your own address |
| `/api/reboot` | POST | cookie + CSRF | Restarts with the saved settings; applied but unsaved settings are discarded |
| `/api/password` | POST | cookie + CSRF + current password | `{"current": "...", "new": "..."}`; the new password has 12 to 128 characters |
| `/api/allow-list` | POST | cookie + CSRF | `{"entries": ["192.168.8.0/24", "2001:db8::/64", ...]}`: source addresses or networks (CIDR) allowed to connect. `409` if your own address would no longer be allowed |
| `/api/factory-reset` | POST | cookie + CSRF + current password | *Reset to defaults* (`{"password": "..."}`): erases the saved settings, the password and the allow-list, then restarts. The TLS key and certificate are kept |
| `/api/ote/once` | POST | cookie + CSRF | Restarts once with firmware updates (OTE) switched on, until the next restart |
| `/api/ote` | POST | cookie + CSRF (page) **or** HTTP Basic (script) | Firmware upload. The body is the raw application image (`ESP32TimeServer.bin`, `application/octet-stream`). `403` while OTE updates are switched off. See below |
| `/api/coredump/erase` | POST | cookie + CSRF | Erases the stored core dump |
| `/api/gnss/command` | POST | cookie + CSRF | GNSS console: sends one command to the GNSS receiver. See below |
| `/api/gnss/console` | GET | cookie | GNSS console: the recorded traffic after `?after=<seq>`. See below |

Notes on `/api/apply`:
- A network change must be confirmed within 120 s, otherwise the previous network settings are restored.
- A change of a restart field (FQDN, pins, LED module, MQTT, ...) writes a one-shot set and restarts once with it.
- MQTT broker: `mqtt_broker` (IPv4 or IPv6 address or DNS name, no `.local`), `mqtt_port` (0 = 8883 with TLS, 1883 without), `mqtt_tls`, `mqtt_ca_pem` (CA certificate of the broker in PEM, at most 4096 characters, required with TLS; parsed with mbedTLS, `400` if it cannot be read). With TLS the broker certificate is checked against this CA and the broker name or IP address (SAN). The former field `mqtt_uri` (`mqtt://host[:port]` or `mqtts://host[:port]`) is still accepted and sets broker, port and TLS; fields sent with it replace these values.

**Static files** need no login; only the allow-list applies: `/`, `/app.js`, `/app.css`, `/i18n.js`, `/favicon.ico`.

## Configuration file (`/api/config`)

```json
{
  "timeserver_config": 1,
  "settings": { "fqdn": "ESP32TimeServer", "...": "every field of settings in /api/state, with the secrets" },
  "hostname": "ESP32TimeServer",
  "firmware": "3.0.5-WebIf",
  "exported": "2026-11-01T10:15:00Z",
  "allow_list": ["192.168.8.0/24"]
}
```

- `timeserver_config`: the format (1). A file without it, or of another format, is refused.
- `settings`: the settings in use, with the field names of `settings` in `/api/state`, but with the MQTT password in clear text (`mqtt_password`) in place of `mqtt_password_set`; the NTP keys (`ntp_keys`) are there as on the page. On import a field the file lacks gets its default. A file of an earlier firmware with `mqtt_uri` instead of `mqtt_broker`, `mqtt_port` and `mqtt_tls` is read as with `/api/apply`; with `mqtts://` and MQTT on it needs `mqtt_ca_pem` as well.
- `hostname`, `firmware`, `exported` (UTC, only when the clock is set): for information, ignored on import.
- `allow_list`: at most 8 entries as for `/api/allow-list`. On import a file without it leaves the allow-list unchanged; `[]` clears it.
- Not in the file: the password of the web page and the TLS key and certificate.

Import (`POST /api/config`): the settings are checked like `/api/apply` and `/api/save`, then the allow-list is stored (refused with `409` if it would not allow your own address). Then:
- without a change of the network settings the set is saved in NVS and takes effect; if a restart field changed, the device restarts with it (`"restart": true`);
- with a change of the network settings the set is applied like `/api/apply` and not saved: confirm within 120 s (`network_pending`), then save it with `/api/save`; with a restart field it is written as a one-shot set and the device restarts once.

Answer: `{"ok": true, "message": "...", "restart": ..., "network_changed": ..., "saved": ..., "network_pending": {...}}`.

## GNSS console (`/api/gnss/command`, `/api/gnss/console`)

The *GNSS console* sub-panel of *GNSS and PPS* sends commands typed by the user to the GNSS receiver. The firmware itself sends nothing through it.

**`POST /api/gnss/command`**, body `{"mode": "text" | "ubx", "command": "...", "checksum": true, "crlf": true}`:
- `text` sends the command as typed (1 to 200 printable ASCII characters). The two options are independent:
  - `checksum` appends `*hh`: the XOR of the characters after a leading `$` or `!`, or of all characters without one. A command that already contains `*` is refused while this is on.
  - `crlf` appends CR LF.
- `ubx` takes hexadecimal bytes. Spaces, commas, colons, dashes and `0x` are allowed.
  - `<class> <ID> <payload>`: the sync bytes, the length and the checksum are added. The payload has at most 100 bytes.
  - Or a whole frame starting with `B5 62`: it is sent only if its length and checksum are right.
  - It is sent between the firmware's own UBX commands. It is only allowed with the protocol U-Blox UBX (`409` otherwise).
- The answer is `{"ok": true, "sent": "<the command as recorded>"}`.
- Errors:
  - `400` an invalid command (the reason in `error`);
  - `409` UART not running, or UBX without the u-blox protocol;
  - `429` more than 4 commands per second;
  - `503` receiver busy for 2 s, or out of memory.
- Every command is logged on the serial console with the source address.

**`GET /api/gnss/console?after=<seq>`** returns the entries after the sequence number `seq`. Each call keeps the recording on for 10 s; without calls the reader records nothing. The answer:
- `entries`: a list of `{"seq", "kind", "periodic", "text"}`.
  - `kind` is `sent` (a command), `text` (a received text line, any printable line) or `ubx` (a received UBX frame: class-ID, ACK/NAK, the MON-VER strings or the payload in hex).
  - `periodic` marks the per-second messages: the standard NMEA sentences (RMC, GGA, GSV, ...), UBX NAV and TIM-TP.
- `last`: the sequence number for the next call.
- `skipped`: older entries were overwritten (the ring keeps 48).
- `reset`: the number was from before a restart, so the list starts again.
- `running`: the GNSS UART runs.
- `ublox`: the protocol is U-Blox UBX.

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

# Export the configuration (contains secrets)
curl -k -b jar -H "X-CSRF-Token: <csrf>" -o timeserver-config.json https://HOST/api/config

# Import it again
curl -k -b jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -H "X-CSRF-Token: <csrf>" --data-binary @timeserver-config.json https://HOST/api/config

# State-changing request, e.g. a restart
curl -k -b jar -H "Origin: https://HOST" -H "Content-Type: application/json" \
     -H "X-CSRF-Token: <csrf>" -d '{}' https://HOST/api/reboot
```

Keep this file up to date when an endpoint, its authentication or its behaviour changes.
